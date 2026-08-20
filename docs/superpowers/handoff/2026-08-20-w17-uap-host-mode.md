# W17 handoff: Wi-Fi HOST mode — the M.2 card as an access point (uAP)

u-blox **M2-MAYA-W161** (IW416/SD8978) on **MIMXRT1170-EVKB RevC3**, repo
`~/Development/rt1176-evkb-m2-maya-w161`, branch **`master`** (the W16 merge —
everything now lives on masters). Pins, all verified on their repos' masters:
**M2Radio `c7d0510`**, **teensy-cores `fcd22b0`**, **lwip `c6b2548`**; QEMU
model needs **qemu2 ≥ `7e17eff5d3`** (gitlab.com/Newdigate/qemu-rt1170 —
older models red EVERY m2_rx_demo gate, not just the new ones).

**Read before forming any theory:**
`examples/networking/m2_throughput_test/transcript_hw_evkb.txt` — W11→W16,
now including W16's three retractions and the method that survived them
(bisect one variable, controls at both ends). Sweep baseline is **108/0/0**;
run it from `/tmp/ev`, run the licence audit from the REAL path (through the
symlink every depfile reads as outside the swept roots and it fails for
non-licence reasons).

## The question, and what is already established

Can the card HOST an AP instead of (or as well as) joining one? The hardware
and firmware say yes; our driver has none of it. Established this session,
with sources:

* **Module datasheet** (MAYA-W1, UBX-21006380 R09): "can work … as a simple
  access-point"; "Supports simultaneous station, access point and P2P modes";
  "Supports up to eight stations in AP mode".
* **NXP's stack has the full uAP layer** (mcuxsdk `middleware/wifi_nxp`,
  BSD-3-Clause — reference for a clean-room re-implementation, never vendored,
  same rule as the existing Iw416 header states):
  - commands in `wifidriver/incl/mlan_fw.h`: `HOST_CMD_APCMD_SYS_CONFIGURE`
    **0x00b0**, `APCMD_BSS_START` **0x00b1**, `APCMD_BSS_STOP` **0x00b2**,
    `APCMD_STA_LIST` **0x00b3**;
  - implementation to read: `wifidriver/mlan_uap_cmdevent.c`,
    `mlan_uap_ioctl.c`; the high-level flow in `wlcmgr/wlan.c`
    (`wlan_start_network`, `WLAN_BSS_ROLE_UAP`);
  - events (`mlan_fw.h`): `EVENT_MICRO_AP_STA_DEAUTH` 0x2c,
    `EVENT_MICRO_AP_STA_ASSOC` 0x2d, `EVENT_MICRO_AP_BSS_START` 0x2e,
    `BSS_IDLE` 0x43, `BSS_ACTIVE` 0x44;
  - NXP even ships a small **DHCP server** (`dhcpd/`, `incl/dhcp-server.h`) —
    reference for ours, since lwip core has no server app.
* **The blob the board already downloads is the full image**
  (`sduartIW416_wlan_bt.bin`, not the station-only `sdIW416_wlan.bin`), so uAP
  support is *probably* in the firmware already running. Probably is not
  evidence — see Phase 0.
* **M2Radio has zero uAP code**, and one structural assumption that uAP
  breaks: the data path hardcodes `bss_type/bss_num = 0` (the STA interface)
  in the TxPD it builds AND ignores those fields in the RxPD it parses
  (`Iw416.cpp` ~947, ~1468). With a second BSS live, RX frames from AP
  clients would be silently mis-delivered to the STA netif.

## Phase 0 — the decisive cheap probe (do this before designing anything)

Send `SYS_CONFIGURE` (a GET is enough) and/or `BSS_START` to the running
firmware and read the reply's `result` field. `result=0` or a real error code
= uAP present; unknown-command = this blob lacks it and the whole plan changes.
This is the W6 precedent (`queryPmk` settled the embedded-supplicant question
in one afternoon) and it reuses `sendHostCmd`/`waitCmdResp` unchanged.
`STA_LIST` (0x00b3) is worth probing too — it is the future gates'
**un-fakeable oracle**: the CARD's own list of who has joined.

Note the QEMU model answers UNKNOWN commands with `HostCmd_RESULT_ERROR`
(its NOTE 13) — so the probe's "not supported" path can be exercised in
emulation today, and the "supported" path CANNOT until the model grows the
uAP surface. Model the contract first, as W14 did: read `mlan_uap_cmdevent.c`,
model what the FIRMWARE accepts, then write the driver against it.

## Shape of the work (after Phase 0 says yes)

1. **uAP command set** in Iw416: `uapConfigure(ssid, psk, channel)` (the
   SYS_CONFIGURE TLV soup — SSID, channel, auth/cipher, max-STA), `uapStart()`,
   `uapStop()`, `uapStaList()`. Same command-port machinery as `associate()`.
2. **Dual-BSS data path.** Make `bss_type/bss_num` a real parameter on TX and
   a demux key on RX. ★ This touches the hot path that W8/W12/W16 were dug out
   of — smallest possible change, and check the ring contract in NXP's source
   first: mlan runs BOTH interfaces over the SAME rings, tagged per-packet.
   Verify that, don't assume it.
3. **Events**: the five MICRO_AP events into serviceLink's demux + counters
   (`staAssocCount()` etc. — gates need numbers, not prints).
4. **Upstack**: second lwip netif for the AP side; a DHCP server (port NXP's
   `dhcpd` shape onto raw lwip, or static-IP clients first — static is the
   zero-dependency Phase 1); routing/NAT between STA and uAP is explicitly
   OUT of scope until everything else soaks.
5. **QEMU**: uAP command surface + a modelled "station" that associates and
   exchanges frames, so gates can assert join/leave/data without RF. Every
   modelled behaviour anchored to `mlan_uap_cmdevent.c` or a silicon capture,
   with a where-this-could-be-lying list (`iw416-sdio.c`'s header is the
   precedent).

## Invariants and open questions (violate the first set, inherit W8–W16's pain)

* **The ring safety net stays**, and its trigger is quiet SERVICE passes —
  re-derive, don't inherit, after any pass-structure change (W15's lesson).
* **The fresh-evidence interrupt clear stays** (M2Radio `c7d0510`): clearing
  `HOST_INT_UP_LD` on a stale snapshot cost 4.6x and strands on BOTH paths.
* **Register port + interrupt mode are the transport** — uAP work should ride
  them, not bypass them. RX/TX aggregation are DEFAULT OFF; leave them.
* ★ **IEEE PS is an open question in uAP mode.** The W10 erratum (fw idle
  RX-death, worked around by IEEE PS ON) was established in STA mode; IEEE PS
  is a STA mechanism. What the firmware does with PS while a uAP BSS is
  active — and whether the erratum even exists there — is UNKNOWN. Check what
  mlan does (it may refuse or auto-disable PS with uAP up) and say so in the
  design doc before the first soak, not after.
* **Simultaneous STA+uAP** is datasheet-supported but is a later phase: bring
  the AP up alone first. Concurrency multiplies every ring/event question.

## Method (the W16 lessons, condensed — all five were paid for)

* Bisect ONE variable with controls at BOTH ends; every single-pair comparison
  this project has believed was later overturned.
* A MISSING counter must never read as zero (the truncated-`char[160]` UDP
  reply produced a confident, false "the interrupt never fired").
* A reference value read in a DIFFERENT PHASE of the card's life is not a
  constant (reg 0x5C: 0x0D to the boot ROM, 0x40 to running firmware).
* WHERE you sample matters (INT_SIGNAL_EN read inside a service pass is
  always 0 — the ISR masked it; read from loop()).
* Counters that look excellent are necessary, not sufficient — twice a
  bus-cost win was a throughput loss (W11 cache, W16 aggregation-before-fix).
* Every regression gate DEMONSTRATED to fail against a re-broken driver, red
  output quoted in its header. Driver work through implementer + review
  subagent loops; the reviewer has overridden the author correctly many times.

## Bench notes

* ★ **The XIAO ESP32-C6 flips roles: it is the bench STATION.** It currently
  runs `tools/esp32c6-benchap/` (SoftAP); a sibling sketch with `WiFi.begin()`
  joining the board's AP gives a closed-loop join/DHCP/traffic test with NO
  change to the Mac's network — the constraint that shaped all of W16's bench
  work. Toolchain proven: `arduino-cli -b esp32:esp32:XIAO_ESP32C6`, port
  `/dev/cu.usbmodem14534401`, antenna-switch GPIOs 3/14 handled in the sketch.
  The ESP8266 (`/dev/cu.usbserial-0001`) is a second candidate client.
* Keep the ESP8266 UNPLUGGED unless it is the client under test — it still
  broadcasts `ESP8266TEST`.
* New AP credentials: fresh throwaway SSID/PSK, via the standing rule only
  (CMake cache → gitignored generated header; `make-creds.sh` shows the
  refuse-to-write-untracked pattern). This repo has leaked live Wi-Fi
  passwords into pushed history once already.
* Board: MCU-Link VCOM `/dev/cu.usbmodem5DQ2DDHVWO5EI3`, flash VCOM-free
  (`pkill -f rt1170-console.py` first), LinkServer 26.6.137, never
  `rm -rf`/`cmake -B` an existing example build dir (a wiped creds cache is
  indistinguishable from a dead card).
* `m2_throughput_test` has runtime switches over UDP (`TPUT MODE`, `TPUT SET`,
  `TPUT BUS?`) — the pattern to copy for uAP A/Bs: flip at runtime, one
  firmware life, never compare across reflashes.

## Success criteria

1. Phase 0 answer recorded either way, with the reply bytes quoted.
2. AP visible in a scan, C6 client joins, gets an address, moves TCP/UDP both
   ways — each step asserted by counters/`STA_LIST`, not by prints.
3. STA-side regression: all 108 gates green, and a real-AP `m2_lwip_test`
   session unchanged (the STA path must not notice uAP code exists).
4. QEMU gates for join/leave/data, each demonstrated to fail.
5. Health counters clean across a soak: `stranded=0` (it is 0 now — keep it),
   `split=0`, resyncs at their documented baseline.
