# Continue M.2 Wi-Fi (W10: PS workaround for the fw idle erratum; stack polish)

u-blox **M2-MAYA-W161** (IW416/SD8978) on **MIMXRT1170-EVKB RevC3**, repo
`~/Development/rt1176-evkb-m2-maya-w161`, branch **`m2-phase0-serial2`**.
Driver sibling `~/Development/M2Radio` (master, pushed, pinned at
**`eeabe28`**).

**Read first:** `examples/networking/m2_lwip_test/transcript_hw_evkb.txt`
(the W9 proof) and the erratum record below.

## What W9 shipped (all reviewed, committed, silicon-proven)

* **lwip runs over the Wi-Fi link.** New driver surface: sink-based
  `Iw416::serviceLink()` (every RX frame per pass; `pollLink()` now wraps
  it), `Iw416::connectStation()` (one-call scan→PMK→associate→watch with
  reason-15 short-circuit), and `M2Radio/lwip/Iw416Netif.{h,cpp}` (the
  netif glue, ethernetif.c's Wi-Fi sibling).
* **`examples/networking/m2_lwip_test`**: lwip DHCP + raw-API TCP echo
  client vs the ESP oracle. Silicon: **86/86 byte-exact echoes, 0 fails,
  3-minute window** (transcript committed). QEMU gate asserts the
  device-absent fallback; sweep is now **96 gates**.
* **Board-preamble lesson (transcript note): the M.2 reset/1.8 V preamble
  lives in EXAMPLES, not the driver** — a new example that omits it falls
  to the fallback on silicon while QEMU stays green (no card there either
  way). Copy the preamble block from m2_lwip_test.cpp / m2_sdio_probe.cpp.
* **licence-audit**: `LICENSE_AUDIT_EVKB=$(pwd)` is REQUIRED from this
  checkout (the default names ~/Development/rt1170/evkb — trap documented
  in the script). The NXP fw_bin root (LA_OPT binary licence) is now a
  declared REPOS root. Audit measured PASS 2026-08-19.

## The firmware idle RX-death erratum — CHARACTERIZED, W10's job to fix

All silicon-measured 2026-08-19, ESP8266 WPA2 AP, dual capture:

| Arm | PS | Traffic | Outcome |
|---|---|---|---|
| M2Radio probe (overnight) | off | ~1.5 pkt/s | dead at ~44 min / 3972 frames |
| M2Radio probe (soak 2) | off | ~1.5 pkt/s | dead at ~19.7 min (clean cliff, 0 loss before) |
| M2Radio probe (stress) | off | ~11 pkt/s | CLEAN 10 min / 6331 frames |
| NXP wifi_cli | **off** (`wlan-ieee-ps 0`) | ~1.5 pkt/s | dead at ~14.4 min (identically) |
| NXP wifi_cli | **on** (default) | ~1.5 pkt/s | **CLEAN 60 min, 172/172 cycles** |

The death is FIRMWARE-level (its own EAPOL reception dies too — reason-15
loops on a fresh AP; only a card reset revives it; AP reset does not).
Sparse traffic + IEEE PS off + stochastic ~14-44 min. NXP never sees it
because their default is PS on.

**W10 step 1: implement the PS-enable workaround in M2Radio** —
`HostCmd_CMD_802_11_PS_MODE_ENH` (0x00E4; see NXP's `wlan_ieeeps_on` /
`wlan_cmd_enh_power_mode` in mcuxsdk middleware/wifi_nxp) sent after
connect; then an overnight probe/lwip soak to confirm the workaround holds
for OUR stack (wifi_cli PS-on already holds 60 min).

**Optional discriminator (user-gated): the house-AP arm.** The ESP softAP
beacons `WMM: NO` — exotic. A PS-off sparse soak against the user's normal
WMM AP (OnestreamQJN7) separates "fw dies against any AP" from "fw dies in
non-WMM associations". Needs the house PSK: the user drops it in session
scratch (e.g. `house_psk.txt`) — NEVER into the repo. Ask before running.

## W10 also worth doing

* Promote the erratum record into `m2_sdio_probe/transcript_hw_evkb.txt`
  as a dated section (currently the full A/B table lives only here and in
  session scratch logs `soak_run2.txt` / `soak_wifi_cli_psoff.txt` /
  `soak_wifi_cli_pson.txt` / `stress_run1.txt`).
* Interrupt-driven SDIO service (DAT1) to replace CMD52 status polling.
* Stale comment cleanup: `m2_sdio_probe.cpp` ~lines 123-126 still says
  R404 "is DNP... does not reach J54 pin 56" — predates the hand-bridging;
  the code below it (and m2_lwip_test's preamble comment) has it right.
* The bench PSK appeared in plaintext in the committed W9 plan doc until
  redacted at HEAD (still in git history, commit b80ecdc). It is a
  throwaway for the bench-only ESP AP; rotate it (new value in the
  gitignored caches + ap_creds.h, reflash ESP + both examples) if that
  history bothers anyone.
* Throughput: iperf-style UDP/TCP blast; the ring code has seen ~5 pkt/s.
* lwip netif follow-ups noted in review: none blocking.

## The test rig (state as left)

* ESP8266 (`/dev/cu.usbserial-0001`): WPA2 AP "ESP8266TEST" ch6, PSK =
  probe/lwip build's `M2RADIO_WIFI_PSK` (gitignored caches), 1 Hz UDP
  broadcast :4711, **TCP echo server :4712**, logging on serial. Sketch in
  session scratch `esp_ap/` (recreate: see W9 handoff's description or the
  transcript).
* EVKB: left running `m2_lwip_test` (bound at 192.168.4.100, ~0.5 Hz TCP
  echoes + broadcasts ≈ 5 pkt/s — ABOVE the sparse-death regime, so it
  should hold; its `lwip:` line is the soak instrument. If `rx=` freezes
  overnight anyway, that's new data: the erratum bites at 5 pkt/s too.)
* Flash with `LinkServer flash … load` only (plain `run` = ~2 kB/s silent).
* Sweep from `/tmp/ev` (sun_path); audit with `LICENSE_AUDIT_EVKB=$(pwd)`.
