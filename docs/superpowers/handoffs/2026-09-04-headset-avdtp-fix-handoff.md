# Handoff — Headset AVDTP DISCOVER fix (BT-3 / NEW-9), software DONE, silicon BLOCKED on bench access

**Date:** 2026-09-04. **Resumes:** `2026-09-03-headset-avdtp-discover-handoff.md`.
**Status:** the three diagnosed AVDTP gaps + the two driver/bench blockers are
IMPLEMENTED, host-tested (each shown RED first), and every affected QEMU gate is
green with the fake peer tightened to model the Shokz. **Not pushed, pin NOT
bumped** — deliberately: this A2DP change is silicon-affecting and the tree's
rule is *silicon wins*, and the silicon acceptance is BLOCKED (below). Local-first
resolution means the bench builds already use the working-tree M2Radio.

## What was implemented (all from the Mac→Shokz PacketLogger reference, `~/Documents/BT-shokz-capture.pklg`)

M2Radio (`~/Development/M2Radio`, branch `master`, committed, NOT pushed):
1. **`9e804d2` L2CAP** — Config Request now carries an **MTU option** (`RX_MTU=1004`,
   the exact value the Mac negotiates with the Shokz; `01 02 EC 03`). `MAX_CHANNELS`
   3→5 (both headsets open reverse SDP channels on AVDTP contact; with 3 slots the
   media `connect()` found none → AVDTP `0xFD`). `l2cap_test` 32→45, both shown RED.
2. **`c218280` Avdtp** — `GetAllCapabilities` (0x0C) replaces `GetCapabilities`
   (0x02); the initiator walks the SEP list to the SBC one (the Shokz lists its
   **MPEG SEP first** — "first audio sink" picked wrong and failed at caps); configures
   **Delay Reporting** (cat 0x08) only when the SEP advertises it; accepts the peer's
   **DelayReport** command after OPEN; General-Rejects any other peer command. `avdtp_test`
   11→55, the whole state-machine walk shown RED (19 failures) first.
3. **`03483d9` + `eb1c8b5` SDP server** — `Sdp::serve()` + `SdpServer` answer the
   peer's SDP query of **our** AudioSource record (the Shokz asks the source's A2DP
   profile version and answers DISCOVER only after that completes; this stack answered
   nothing). Chunked continuation under MaxAttributeByteCount and the peer's L2CAP MTU.
   `A2dpSource` routes peer SDP to it and drives it from a single `service()`.
   `sdp_test` 25→37.
4. **`1dd4fb0` BtLink paging** — the two bench blockers: `Write_Page_Timeout 0x2000`
   written explicitly; Create_Connection with **role_switch=0** (the Mac's params);
   the page is retried up to `PAGE_ATTEMPTS=3` without a fresh inquiry; a **silent
   page is cancelled** (`Create_Connection_Cancel`) after **reclaiming the leaked HCI
   command credit** (`Hci::reclaimCredit`, the `ncmd_starved` wedge). `disconnect()`
   added; `A2dpSource` disconnects on every post-connect failure so a retry starts
   clean. `btlink_test` (new, 23 checks) reproduces the wedge on the host, shown RED.

evkb (`~/Development/rt1170/evkb`, branch `master`, committed, NOT pushed):
- **`2ab703b`** — `hci_peer.py`'s AVDTP acceptor now **models the Shokz** (reverse
  SDP query of our source, MPEG-SEP-first list, GetAllCapabilities with delay
  reporting, DelayReport-after-OPEN holding back the OPEN accept, an MTU tripwire on
  the host's Config Request, plus Write_Page_Timeout / Create_Connection_Cancel /
  Disconnect). The `[avdtp]` gate asserts all of it (order `1,12,12,3,6,7`,
  `sdp_served=1 delay_report=2000`) and was **DEMONSTRATED RED five ways** (SDP server
  silent = the bench symptom; option-less CONFIG_REQ; wrong-SEP config; 0x02 vs 0x0C;
  DelayReport ignored). `[media]` carries the same tripwires. `m2_hci_probe` wires
  `SdpServer`; `bt_tone_test`'s loop calls `A2dpSource::service()`.

## Verification done (autonomous, no bench)
- BT host suite: `l2cap 45 / avdtp 55 / sdp 37 / btlink 23 / sbc 228 / rtp 9 / mediapacketizer 22`, `BT-HOST-TESTS: PASS`; SBC SNR 63.9 dB.
- HCI host suite PASS; `acl-trace-to-btsnoop.test.sh` PASS.
- Every affected QEMU gate green: `bt_tone_test` `run_qemu.sh` + `run_qemu_media.sh`;
  `m2_hci_probe` `run_qemu.sh` / `run_qemu_hci.sh` / `run_qemu_avdtp.sh` / `run_qemu_baud.sh`.
- Vacuity suite **32/0/0**. Blast radius is only these two examples (nothing else links `bt/` or `hci/`).
- Full 128-gate sweep + license audit: see the session's closing note.

## BLOCKED — silicon acceptance (needs a human at the bench)
Two physical dependencies I cannot perform:
1. **The MCU-Link DAP is WEDGED at connect.** Two clean `LinkServer run` attempts sat
   at "Selected probe #1" with a 0-byte flash output for 4½ minutes each. The VCOM
   still enumerates, so the board is fine — this is the documented DAP-connect wedge
   whose ONLY fix is **replugging the DEBUG USB** (a board power cycle does NOT clear it;
   see `mcu-link-dap-wedge` memory). Until that is done, nothing can be flashed.
2. **The Shokz needs physical setup per attempt** — factory-reset, explicit PAIRING
   mode (a power-cycle only gives reconnect mode), and **Mac Bluetooth OFF** (the Mac
   currently has `OpenMove by Shokz` C0:86:B3:31:29:2F bonded and will grab it).

## Resume recipe (bench, after replugging the DEBUG USB)
Two prebuilt images are ready in `examples/audio/bt_tone_test/`:
- `build-bench/bt_tone_test.elf` — **ESP32 no-regression** (legacy PIN, `EVKB-SINK`,
  3 Mbaud, RXRTSE, real fw). Flash it FIRST (the ESP32 sink is always discoverable,
  no human steps): confirm `a2dp_try=ok`, `streaming`, `hb streaming=1 ... drops` low,
  and audible tone — proves the MTU/GetAllCapabilities/SDP-server/paging changes did
  not break the working ESP32 path.
- `build-trace/bt_tone_test.elf` — **Shokz** (SSP, target `Shokz`, `M2_BT_ACL_TRACE=ON`).
  Flash with the Shokz freshly in pairing mode + Mac BT off; read the VCOM with a
  reopen-on-drop reader (`scratchpad/reader.py <port> <log>`); expect
  connect→SSP pair→encrypt→SDP→**DISCOVER→GetAllCapabilities→SetConfiguration→OPEN→
  (accept DelayReport)→START→STREAMING** and a clean 1 kHz tone ≥60 s. Grab the
  `acl_trace` lines → `tools/acl-trace-to-btsnoop.py out.btsnoop` and diff against the
  Mac reference to confirm which gap mattered (the caveat still stands: an earlier run
  reached `avdtp_state=1`, so let silicon say whether the MTU alone was the blocker).

## Then close out (plan Task 8)
Only AFTER silicon STREAMING on the Shokz (and ESP32 no-regression): push M2Radio,
bump the `M2Radio` pin in `evkb.cmake`, fresh-user `-DEVKB_FORCE_FETCH=ON` verify,
full sweep + license audit (never concurrent), commit + push evkb, update memory + NEW-9.
