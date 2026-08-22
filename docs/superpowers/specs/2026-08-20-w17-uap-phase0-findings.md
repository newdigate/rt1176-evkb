# W17 Phase 0 — the uAP question, answered

**Date:** 2026-08-20  ·  **Answer: YES, the firmware has the uAP command layer.**

The handoff (`docs/superpowers/handoff/2026-08-20-w17-uap-host-mode.md`) made
Phase 0 the gate on everything else: *"Send `SYS_CONFIGURE` (a GET is enough)
and/or `BSS_START` to the running firmware and read the reply's `result` field.
… `result=0` or a real error code = uAP present; unknown-command = this blob
lacks it and the whole plan changes."*

It is answered, and the answer is more useful than a yes — with one new fault
that Phase 1 will hit on its first day.

Evidence, with every reply byte quoted:
`examples/networking/m2_uap_probe/transcript_hw_evkb.txt`.

## What was built

`examples/networking/m2_uap_probe` — a self-contained probe that brings the
card up, downloads the blob, and asks a matrix of commands, printing one
machine-parsable line per cell.

One driver change, additive and the smallest that could work:
`Iw416::sendHostCmdBss(cmd, body, len, bssType, bssNum = 0)`. mlan routes a
command to an interface through `seq_num`'s high byte
(`HostCmd_SET_SEQ_NO_BSS_INFO`: bss_num 11:8, bss_type 15:12), and every
command this driver had ever sent left both at 0. `sendHostCmd()` now delegates
with `(0, 0)`, so no existing caller changes shape or wire bytes — verified by
inspection and by all eleven pre-existing M.2 QEMU gates staying green.

Two gates: `run_qemu.sh` (card-absent) and `run_qemu_wifi.sh` (the IW416 model).
Sweep goes 108 → **110**.

## The answer

| command | id | result | body |
|---|---|---|---|
| `HOST_CMD_APCMD_SYS_INFO`  | 0x00AE | `0x0000` OK | `"w8978o-V0, RF878X, FP92, 16.92.21.p1"` |
| `HOST_CMD_APCMD_STA_LIST`  | 0x00B3 | `0x0000` OK | `sta_count = 0`, in a 10-byte reply (`S_DS_GEN` + 2) |
| reserved `0x7FFE` (control)| —      | `0x0002` NOT_SUPPORT | — |
| `HOST_CMD_APCMD_SYS_CONFIGURE` | 0x00B0 | *no reply, and the port dies* | see below |

Both AP commands answer on **both** `bss_type=0` and `bss_type=1`, and both are
distinct from what this firmware does with an id nothing defines. The
`SYS_INFO` body is the un-fakeable part: that build string exists nowhere in
our image.

`STA_LIST` returning a real, empty station list is the second result worth
having — it is exactly the oracle the future join/leave gates need, and it
works today.

### The card acts on `bss_type`

Replies echo the `seq_num` they were sent, and the uAP nibble comes back:
`bss=0` requests return `0x0003, 0x0007, …`; `bss=1` requests return
`0x1004, 0x1008, …` (bits 15:12 = 1 = `MLAN_BSS_TYPE_UAP`).

Two consequences:

1. `waitCmdResp()`'s low-byte-only seq comparison is now **mandatory**. It was
   written speculatively in W12 ("keeps the comparison honest even if that ever
   changes") — this is that change, and a whole-word compare would reject every
   uAP reply as stale.
2. **QEMU cannot catch a regression there.** The model zeroes the high byte in
   its replies, so masked and unmasked comparisons both pass against it. If a
   uAP reply ever reads as stale on silicon while every gate is green, this is
   the divergence.

## The method, because run 1 was wrong

Run 1 put the positive control first and the negative control last. It read
`SYS_INFO` OK, then `SYS_CONFIGURE`, `STA_LIST` and the reserved-id control all
timing out — and the obvious conclusion, "this firmware answers unknown
commands with silence, so `STA_LIST` is missing", **is false**. One command had
wedged the command port and every later timeout was an echo of that one event.
The control ran after the damage, so the run could not tell the two stories
apart.

The probe now enforces a **bracketing rule**: a cell is evidence only if the
nearest positive control on *each side* of it answered. Unbracketed cells are
printed as `uap_unbracketed` and excluded from the tally, never folded into a
count. The negative control runs first, on a port known healthy, and again at
the far end; positive controls run between every AP command.

This is the tree's own method (bisect one variable, controls at both ends)
applied to a command matrix, and it is the difference between run 1's wrong
answer and run 2's right one — same firmware, same driver, ten minutes apart.

## ★ FAULT 1 — **RESOLVED 2026-08-21**. The handler wants a populated request.

> **The account in this section was written on 2026-08-20 and its conclusion
> had an uncontrolled variable.** Every row that answered in those runs was
> empty-bodied and both rows that wedged carried a body, so "0x00B0 is the
> killer" and "a bodied command is the killer" fit the evidence equally well.
> The follow-up run closed that, and three more besides: see
> **[FAULT 1, investigated](#fault-1-investigated-2026-08-21)** at the end of
> this document and the `W17 FAULT 1, INVESTIGATED` section of
> `examples/networking/m2_uap_probe/transcript_hw_evkb.txt`. The section below
> is kept as written, because the confound is the lesson.

## FAULT 1 as first recorded (2026-08-20): `SYS_CONFIGURE` 0x00B0 kills the command port

Not "unsupported" — the port dies. After it, no packet of any kind arrives
again: `CMD_PORT_UPLD` is never raised, `int_seen` stays `0xC2`,
`seq_mismatches` stays 0, and the port does not recover within the ~15 s of
further commands the matrix issues. The port was demonstrably alive one command
earlier.

Bisected, one variable, controls at both ends. The obvious hypothesis was a
malformed request — action(2) with an **empty** TLV buffer — so a second form
was sent first, carrying the one TLV mlan pairs with a channel GET
(`TLV_TYPE_UAP_CHAN_BAND_CONFIG` 0x012A). **Both wedge.** The empty TLV buffer
is not the cause. And the bare form is not our invention either: mlan's own
`wlan_uap_cmd_sys_configure_ext` emits exactly action(2) and nothing else when
it has neither an ioctl buffer nor a data buffer.

Not established, and not to be assumed:

- whether the firmware **crashed** or merely stopped answering;
- whether some earlier uAP initialisation is required that mlan does and we do
  not — the likeliest remaining explanation, and where Phase 1 should look
  first (`wlan_uap_ioctl.c`, and how `wlan.c` reaches `WLAN_BSS_ROLE_UAP`);
- whether `bss_num` must be non-zero for the uAP interface — everything here
  used `bss_num=0, bss_type=1`;
- whether a power cycle is the only recovery. Every run reset the card at boot,
  so recovery was never tested in isolation.

`SYS_CONFIGURE` sits **last** of the AP rows in the committed matrix for this
reason: it is the known port-killer, and putting it last confines the damage to
the trailing controls instead of costing every other row its reading.

## The blob A/B

Both SDK blobs were built and run on the same matrix:
`sduartIW416_wlan_bt.bin` (Wi-Fi + BT) and `sdIW416_wlan.bin` (Wi-Fi only).
Genuinely different images (text 427364 vs 295908 bytes), and **53 capture
lines each, zero differing** — same `fw_release=0x9B105C15`, same version
string, same everything. The blobs differ by the appended Bluetooth image, not
by the Wi-Fi build.

So the handoff's "probably in the firmware already running" is confirmed, and
strengthened: it is in *both*, and nothing in Phase 1 needs to depend on which
is loaded.

## What this does NOT settle

- **That an AP can be started.** `BSS_START` was never sent — it emits RF with
  whatever the firmware defaults to, and is compiled out unless
  `-DM2_UAP_PROBE_BSS_START=ON`. "The handler exists" is a long way from "the
  BSS comes up".
- **`SYS_CONFIGURE`**, which is how an AP gets configured at all. Fault 1 is
  Phase 1's gate, before any TLV-soup design work.
- **The data path.** `bss_type`/`bss_num` are still hardcoded to 0 in the TxPD
  the driver builds and ignored in the RxPD it parses. W17 touched the
  **command** path only; the handoff's item 2 is untouched and is still the
  change that lands in the hot path W8/W12/W16 were dug out of.
- **IEEE power save under a live uAP BSS** — still the open question the
  handoff flagged, still unasked.

## Recommended next step

Not the uAP command set. **Fault 1 first**: find out why `SYS_CONFIGURE` stops
the command port, because every remaining Phase-1 item goes through that
command. Read `wlan_uap_ioctl.c`'s path into `wlan_ops_uap_prepare_cmd` for the
initialisation mlan performs before it, and check whether the uAP `mlan_private`
carries a `bss_num` this driver is not sending.

## The pin (closed)

`M2Radio` master is pushed (`c7d0510..494f230`) and `evkb.cmake` names
`494f230`. Verified in fresh-user mode rather than assumed: configured and
built `m2_uap_probe` with `-DEVKB_FORCE_FETCH=ON`, which fetched M2Radio at
that SHA into `_deps/m2radio-src/`, and the fetched `Iw416.cpp` carries
`sendHostCmdBss()`. Clean build.

★ A pin bump makes every consuming build dir **reconfigure** on its next build.
Dirs configured before 2026-08-14 fail that reconfigure with a message that
never mentions a toolchain — `COMPILERPATH is UNDEFINED`. Gates are unaffected
(a gate runs the cached `.elf` and never reconfigures), which is also the
reason a green sweep is no evidence that a build dir can still configure.

## Running the 110-gate sweep

The sweep belongs in the full-build checkout, and `m2_uap_probe` arrives there
with **no build directory** — two SKIPs unless it is built first:

```
cd examples/networking/m2_uap_probe
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```

No blob and no credentials: `build/` is deliberately the fresh-clone build, and
both gates require it (the model gate refuses a build that supplies a blob,
because the model cannot do a download). The silicon builds live in
`build-hw/` (combo blob) and `build-hw-wlan/` (Wi-Fi-only blob).


## FAULT 1, investigated (2026-08-21)

Four more runs on the same board and card, three on the default matrix and one
on an `action=SET` variant. **The wedge reproduced in every run.** Full
evidence, with the reply bytes and the raw captures, is in the transcript.

### Settled

- **It is the command, not the body.** Two new controls: the reserved id
  `0x7FFE` *carrying* `action(2)` answers `NOT_SUPPORT` on both interfaces, and
  `HostCmd_CMD_802_11_MAC_ADDRESS` 0x004D (action(2)+mac(6), size 16) —
  **mlan's own** uAP-addressed init command, `wlan_get_mac_addr_uap()` —
  answers `RESULT_OK` on `bss_type=1` with the real MAC in the body. A bodied
  command on the uAP interface is not merely accepted but understood.
- **Action, TLV and size are all ruled out.** Three forms, each sent first in
  its own firmware life: bare GET (size 10), GET + `CHAN_BAND` TLV (16),
  SET + `MAX_STA_CNT` TLV (16, mlan's own SET shape). All three wedge
  identically. `MACADDR.uap` is also 16 bytes and is answered.
- **The firmware does not crash.** `fwstatus` reads `0xFEDC` (FIRMWARE_READY)
  before, at the moment of failure, and after; CMD52 keeps working; the I/O
  port is unchanged. The card is alive and answers nothing.
- **The wedge has a register signature.** `CARD_TO_HOST_EVENT` (0x5C) reads a
  constant `0x0008` across every healthy command — sampled either side of all
  34/38 cells per run, so this is a distribution and not a single reading — and
  shows `0x0088` (bit 7, `DN_LD_CP_RDY`) in exactly the window of the command
  that kills the port, in both runs, on two different forms. What bit 7 *means*
  here is not claimed; that it is a reproducible trigger is.
- **The data port is not involved.** Thirty `serviceLink()` passes after the
  wedge found no frames, no data and no event, and draining did not bring the
  command port back. The "undrained upload holds off the command port" family
  is eliminated.
- **No self-recovery** in ~90 s (30 retries at 1 s, each also costing its 2 s
  timeout). Previously only "not within ~15 s".
- **`MACADDR.uap` is not the missing prerequisite** — it ran immediately
  before, bracketed by controls that answered, and `SYS_CONFIGURE` wedged
  anyway. And `bss_num=0` is what mlan uses for uAP
  (`mlan_glue.c`, `bss_attr[1].bss_num = 0`), so that open question is answered
  from the reference: what we sent is what mlan sends.
- **Incidental, and load-bearing for Phase 1:** the uAP interface reports the
  **same MAC** as the STA interface. No separate uAP address is provisioned.

- **A card reset recovers it — no power cycle, no MCU reboot.** Run 6, after
  the board was repowered: the wedge reproduced for the fifth time, the port
  again never recovered on its own, and then
  `uap_reinit sdio=ok iw416=ok fw=ok hwspec=ok` → **`uap_reinit=recovered`**.
  The M.2 reset GPIO sequence, a fresh SDIO/IW416 bring-up, a fresh blob
  download and `GET_HW_SPEC` all succeed and the command port answers again.
  Two consequences: a uAP driver can **recover in the field** (the wedge is
  terminal for the firmware life, not for the card), and **the bench can
  iterate** — a variant costs a card reset, not a reflash, which makes a
  multi-variant probe in one firmware life the obvious next tool.

### Still open
- **Why the handler dies.** Inside a closed blob, not answerable from outside.
- **Whether a prerequisite other than `MAC_ADDRESS` exists.** Narrowed, not
  closed: mlan's init sends nothing else to `bss_type=1`.

### What Phase 1 inherits

`SYS_CONFIGURE` cannot be used as the configuration path until this is
resolved, and no amount of reshaping the request will do it — three shapes
covering both action directions and both TLV states all fail the same way. The
card-reset question is now settled (it recovers), so the next move is a
**multi-variant probe in one firmware life** — resetting the card between
variants instead of reflashing — hunting a prerequisite outside mlan's init
path: how `wlan.c` reaches `WLAN_BSS_ROLE_UAP`, and whether a `BSS_MODE`/role
command precedes configuration on this firmware.

### One more thing the probe now carries

`M2_UAP_SYSCFG_SET_FIRST` (CMake option, default OFF) puts an `action=SET`
`SYS_CONFIGURE` first in the matrix. It exists because **the first
`SYS_CONFIGURE` of a run is the only one whose reading survives** — it takes
the port with it and every later row is unbracketed — so a variant can only be
tested by being put first, one variant per firmware life. Default OFF so the
gated build keeps the matrix the QEMU gates assert.


## FAULT 1, resolved (2026-08-21)

**A fully populated `SYS_CONFIGURE` is accepted. A minimal one kills the port.**
Both measured in **one firmware life**, on the same card, minutes apart — so
this needs no cross-run comparison:

```
HWSPEC.ctl+.e3   bss=0/1  st=ok                              <- port healthy in
SYSCFG.fullopen  bss=0    st=ok result=0x0000  cs 0x08->0x08
SYSCFG.fullopen  bss=1    st=ok result=0x0000  cs 0x08->0x08
HWSPEC.ctl+.e5   bss=0/1  st=ok                              <- port healthy out
SYSCFG.chantlv   bss=0    st=cmd-timeout       cs 0x08->0x88 <- the wedge
```

`RESULT_OK` on both interfaces, **bracketed** on each side, so it is evidence by
the example's own rule.

### Where the hypothesis came from

Not from guessing at shapes — from asking the reference what a real bring-up
sends. `wifi_uap_start()` runs `wifi_cmd_uap_config()` → (PMF) → (11d) →
`BSS_START`, so the config **is** the first uAP command and there is no missing
prerequisite upstream of it. And its builder refuses to construct the command
without a configuration at all:

```c
if (pioctl_buf == MNULL) { LEAVE(); return MLAN_STATUS_FAILURE; }
```

"How much configuration the request carries" was the one axis the three earlier
shapes never varied, and the only one mlan's own code says is not optional.

### What shipped

`Iw416::uapConfigure(const UapConfig &)` — the nine TLVs mlan emits for an open
AP on a manual 2.4 GHz channel, built by the **driver** so what was tested is
what Phase 1 ships. 82 bytes, verified in QEMU before the bench run. The TLV set
is a **mask** (`UapTlv`), because the boundary between accepted and rejected is
the finding and can only be found by varying the set.

★ Every TLV `len` is the **payload** length. `DTIM` and `BCAST` are **1** byte,
`AUTH` is **3** (auth_type, PWE_derivation, transition_disable). Those are the
easy ones to get wrong, and one wrong length mis-parses everything after it.

### Consequences

- **There is no harmless probe of `SYS_CONFIGURE`.** A GET is exactly the shape
  that wedges; nothing may "just query" the uAP configuration.
- **The minimal rows stay in the matrix.** They are no longer a mystery, they
  are the control that proves the boundary.
- **The probe sends the full configuration by default**, so the QEMU gate covers
  `uapConfigure()`'s construction. The model can't reproduce the silicon
  behaviour, but it can prove the driver still builds the request byte for byte
  — the part that rots silently. Demonstrated RED by moving the DTIM payload
  length from 1 to 2.

### Still open

- ~~Whether **`BSS_START`** brings the BSS up.~~ **Answered 2026-08-21: it does.**
  See below.

- **WPA2** — AKMP / cipher / passphrase TLVs on the same builder, and with them
  the repo's standing SSID/PSK rule.
- Why a partial request *kills the port* rather than being rejected. Inside a
  closed blob, and no longer blocking.


## The AP is on air (2026-08-21, run 8)

`BSS_START` → `result=0x0000`, `BSS_STOP` → `result=0x0000`, a positive control
answering after each, and `STA_LIST` returning a real `sta_count=0` against a
**live** BSS — the oracle the future join/leave gates need, exercised for the
first time against an AP that is actually up.

**The proof is external.** This Mac, which knows nothing about our firmware, was
asked before, during and after:

| when | `RT1176-UAP-TEST` |
|---|---|
| before | absent (4 networks visible) |
| during | **present** — Channel 6 (2GHz, 20MHz), Infrastructure, Security: None, −75 dBm |
| after `BSS_STOP` | absent, on **four** consecutive scans |

Every advertised value is one we put in a TLV — SSID (0x0000), channel/band
(0x012A, manual + 6), `PROTOCOL_NO_SECURITY` (0x0140) and open `AUTH_TYPE`
(0x011F). So this is not "BSS_START returned OK": the configuration
`uapConfigure()` built **reached the air and a foreign radio read it back**,
and it appears only between START and STOP.

★ **Scan trap:** two `system_profiler` invocations disagreed — one listed the
SSID, the next counted zero — because each triggers its own scan and macOS ages
cached entries out between them. Never compare across invocations; capture one
scan to a file and query that file.

### The events question, answered (run 9)

Run 8 saw **no events** in 63 s with the BSS up, leaving three explanations. The
third — *our demux doesn't surface uAP events* — was eliminated first, by
inspection, because it was free and the only one under our control:
`serviceLink()` acts on `CMD_PORT_UPLD`, reads the command port, checks
`pkttype == MLAN_TYPE_EVENT` and records the id. That machinery was already
known to work, since every command reply in every run arrives through the same
bit (`intseen=0xC2`).

The other two are separated by putting a client on it:

```
uap_hold t=5  sta_count=1 lastevent=0x002D ...
uap_hold t=10 sta_count=1 lastevent=0x002D ...  (steady through t=40)
```

`0x002D` is `EVENT_MICRO_AP_STA_ASSOC`. **The firmware does raise uAP events and
our demux does surface them.** Run 8 saw none because there was nothing to raise
one about. Nothing needs enabling, nothing in the driver needed fixing, and
Phase 1 step 3 can be built on this.

## A client joined (run 9)

An ESP8266 (`tools/esp8266-uapclient`) associated. **Both sides name each
other**, and neither could invent the other's MAC:

| | reports | matches |
|---|---|---|
| card `STA_LIST` | station `84:F3:EB:B7:C4:5B` | the ESP8266's own `client_mac` |
| ESP8266 | `bssid=6C:1D:EB:91:0C:45` ch 6 | the card's `GET_HW_SPEC` MAC, and our TLV 0x012B |

`sta_count` stepped 0 → 1 and the station entry carries a MAC **this firmware has
never seen** — not in our image, not in any TLV we sent. It could only have come
off the air.

### Deliberately not claimed

- **No DHCP, no IP.** There is no upstack yet; the client uses a static address
  purely so it doesn't sit in a retry loop and misreport association as failure.
  The sketch prints `assoc=` and `ip=` separately because *associated* and
  *addressed* are different claims and only the first is made here.
- **No traffic.** `frames=0` throughout; the data path over a uAP BSS is
  untested.
- **The leave event was not observed.** `BSS_STOP` returned OK and the port
  stayed healthy, but no `EVENT_MICRO_AP_STA_DEAUTH` (0x2C) was recorded —
  untested rather than absent, and it is what a leave gate would assert.

### Superseded

> The run-8 note here said no events appeared and listed three explanations.
> Run 9 settled it — the first explanation was right. Kept because the *order*
> of elimination is the reusable part: the explanation under our own control was
> checked first, for free, before any bench time was spent.


## Data over the uAP BSS, and the ring contract verified (runs 11/12)

The handoff is explicit here: *"mlan runs BOTH interfaces over the SAME rings,
tagged per-packet. Verify that, don't assume it."* Verified.

```
uap_bss_service tag=hold frames=99 rx_bss0=0 rx_bss1=99 rx_bssX=0
                         last_bss=1/0 rxdata_total=99 dropped=0
```

**99 frames, every one tagged `bss_type=1, bss_num=0`** — the uAP interface
exactly as mlan addresses it. Zero arrived tagged `bss_type=0` while a uAP
client was the only station transmitting, and `rx_bssX` (there to catch a byte
offset off by one) stayed 0, so the bytes being read are the bytes assumed.

One set of rings, tagged per packet, and the tag is a usable demux key. That is
Phase 1 step 2's foundation, now measured rather than inherited from NXP source.

### The frames are the client's, and the bytes say so

A counter climbing in step with the client's is suggestive, not conclusive. The
first frame, dumped whole, is a **gratuitous ARP**:

```
FFFFFFFFFFFF 84F3EBB7C45B 0806          broadcast | ESP8266 MAC | ARP
0001 0800 06 04 0001                    Ethernet/IPv4 request
84F3EBB7C45B C0A82C32                   sender = client, 192.168.44.50
000000000000 C0A82C32                   target IP == sender IP -> gratuitous
```

`len=42 = 14 + 28`, well-formed, and every identifying field — the MAC, the IP —
exists nowhere in this firmware. It came off the air.

### Deliberately not done

- **Nothing is routed on the tag yet.** Recording and routing are separate
  changes on purpose: this is the hot path W8, W12 and W16 were each dug out of,
  and a counter reading the wrong byte is a cheap mistake while a mis-routed
  netif is not. The tag is now proven, so the routing change can be made against
  evidence.
- **No TX over the uAP BSS.** The TxPD still hardcodes `bss_type/bss_num = 0`;
  nothing has been sent *to* the client. RX is proven, TX is the other half of
  step 2.
- **No upstack.** Frames go to a counting sink — no second netif, no DHCP.

### Bench trap

Run 11 showed `frames=1` and an apparently idle client. Not the firmware: an
`arduino-cli upload` had silently not taken and the ESP8266 was still running
the previous, non-transmitting sketch. It was caught only because the client
prints its own `sent=` counter and that field was **absent** rather than zero —
a missing field looks nothing like a field reading 0. That is the whole reason
to print counters at both ends of a link test rather than infer one from the
other.


## TX over the uAP BSS — the round trip closes (run 13)

The other half of step 2. `sendDataFrameBss()` returning OK proves only that the
*card accepted a buffer* — not that the frame left, which interface it left on,
or that anything understood it. So the proof is the **peer answering**, and the
peer's stack answers with no cooperation from its sketch: there is no code on
the client that knows this test exists.

An ARP request, hand-built, claiming to be `192.168.44.1` at the card's MAC,
sent with `bss_type=1`. What came back:

```
6C1DEB910C45  dst = THE CARD   -- addressed back to us, not broadcast
84F3EBB7C45B  src = the ESP8266
0806 / 0002   ARP reply
84F3EBB7C45B  C0A82C32         sender = client, 192.168.44.50
6C1DEB910C45  C0A82C01         target = the card's MAC, 192.168.44.1
```

The last two fields are the ones that matter: `192.168.44.1` and the card's MAC
exist **nowhere except in the request we constructed**. The client could only
have learned them by receiving and parsing that frame.

```
uap_tx arp_sent=26 arp_txfail=0 arp_replies=11 txcount=26
uap_bss_service frames=147 rx_bss0=0 rx_bss1=147 rx_bssX=0 dropped=0
```

### The number that is not 1:1

**Eleven replies to twenty-six requests, and that is not explained.** Recorded
rather than rounded up to "it works", because ~42% is the sort of number that
becomes a bug report later. Three candidates, none tested:

- the ESP8266 is a station with **power save on by default** — it sleeps between
  beacons and the AP must buffer for it and deliver at DTIM (ours is 1). If that
  buffering is imperfect this is exactly how it would look;
- lwip may rate-limit ARP replies to a repeating request;
- plain air loss, though the RX direction shows none.

What *is* established: the loss is **downstream of this driver** —
`arp_txfail=0` and `txcount` tracks `arp_sent` exactly, so every frame was
accepted by the card.

**A/B'd in run 14** — one line changed on the client, nothing on the RT1176 side:

| client power save | replies / sent | |
|---|---|---|
| ON (run 13) | 11 / 26 | 42% |
| OFF (run 14) | 25 / 30 | **83%** |

The ratio roughly doubles, so the leading candidate is substantially right: a
station sleeping between beacons misses a good fraction of what the AP sends,
and the AP-side buffer-and-deliver-at-DTIM path is where that goes.

★ But this is **one A/B pair**, one run each, and the project rule that every
single-pair comparison here has eventually been overturned applies to it too.
And **power save is not the whole story** — 83% is not 100%, and 5 of 30 went
unanswered with the client fully awake. lwip ARP rate-limiting and air loss are
both still live and untested. Do not read "TX works" as "TX is lossless": it is
proven *correct* and not yet proven *reliable*, and a uAP upstack will need to
know which.

### Step 2 status

**Done** — the data path in both directions on a real uAP BSS: RX tagged and
counted per interface, TX addressable per interface, both verified against a
peer that shares no code with us. **Not done** — nothing routes on the tag (no
second netif, no DHCP), mixed-BSS TX aggregation is untested, and the reply
ratio is open.


## The upstack: a second netif, and a packet at a socket (m2_uap_lwip)

Handoff step 4's first half. New example `networking/m2_uap_lwip` (+1 gate,
sweep now **111**) hosts an open AP and puts an lwip netif on it.

```
uap_configure=ok result=0x0
uap_bss_start=ok result=0x0
uap_hosting ssid=RT1176-UAP-TEST chan=6
uap_netif_up addr=192.168.44.1
uap_udp_bound port=5001

uap_udp_first from=192.168.44.50:58394 len=23 ascii=RT1176-UAP-CLIENT-HELLO
hb card=1 bss=1 udp_rx=74 udp_bytes=1702 rx_bss0=0 rx_bss1=75 unrouted=0
```

The payload string appears **nowhere in the RT1176 image**, and it arrived at a
socket this firmware bound — so the frame crossed air → RxPD → bss demux →
netif → lwip → IP → UDP → callback, and every layer had to be right.

**The accounting closes exactly**, which is worth more than a counter that
climbs: `74 × 23 = 1702` bytes; `rx_bss1 = 75 = 74 UDP + 1 gratuitous ARP`;
`rx_bss0 = 0` (nothing mis-tagged to the STA side); `unrouted = 0`.

### Driver/glue shape

`iw416NetifInitUap` + `iw416NetifPollDual(sta, uap)` — one service pass, each
frame routed by its RxPD tag, either netif may be NULL. **The STA path is
untouched**: `iw416NetifInit`/`iw416NetifPoll` are byte-for-byte unchanged (the
diff removes no lines), which is how handoff criterion 3 is met — by not
touching it. A frame whose tag matches no netif is counted (`unroutedFrames()`)
and dropped rather than delivered to the wrong stack.

`iw416NetifInitUap` deliberately does **not** enable TX aggregation, unlike the
STA init: aggregation is driver-wide, a mixed-BSS batch is untested on silicon,
and this is the hot path W8/W12/W16 were each dug out of.

### Limitations, stated rather than discovered later

No DHCP server (clients must be statically configured — the zero-dependency
route), no routing/NAT, no security, and the AP hosts **indefinitely** — there
is no `BSS_STOP`, so the board beacons until reset or reflashed.
