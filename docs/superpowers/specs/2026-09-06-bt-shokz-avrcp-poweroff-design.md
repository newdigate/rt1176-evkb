# M2Radio BT: Shokz self-power-off — AVRCP capture + minimal target (NEW-34 piece 3)

**Status:** IMPLEMENTED 2026-09-07 — capture → branch D→C (§8) → the minimal AVRCP target built and verified on the real Shokz (§9: its whole AV/C set is two PDUs, both answered on the wire, M2Radio `fea6d52`) → the `[media]` fake peer now drives that exchange as the headset's controller and asserts our two responses byte-for-byte (three RED demos; no new gate, count stays 132). The 1–2 min self-power-off never reproduced in five arms. Open: the "connected" announcement (not reported from the bench), the 5+ min acceptance run as a formal transcript, and the piece-2 inbound-teardown defect filed from arm 2.
**Issue:** NEW-34 "M2Radio BT: reconnect known devices + soak-test connection
resilience (range loss/recovery)", piece 3 of 5.
**Depends on:** piece 2 (the A2DP source + `L2cap` allow-list). Heavily
bench-dependent — a real Shokz OpenMove is required for both the capture and the
acceptance.

## 1. Goal and the finding that reframes it

A real Shokz OpenMove, connected as our A2DP sink, plays the tone but **never
announces "connected"** and **self-powers-off after ~1–2 minutes** — it behaves
as if the connection is INCOMPLETE and runs its idle-power-off timer. The ESP32
sink and the OneOdio A70 do not; it is Shokz-specific. Prime suspect: **missing
AVRCP** (this stack is A2DP-source only; headsets typically announce "connected"
once an AVRCP control channel is up).

**The finding that sharpens it:** the stack has NO AVRCP/AVCTP at all, and
piece 2's `L2cap::allowPsm` restricts inbound channels to SDP (`0x0001`) and
AVDTP (`0x0019`) only — so when the Shokz opens an **AVCTP channel (PSM
`0x0017`) for AVRCP, we actively REFUSE it** (CONN_RSP result 0x0002, PSM not
supported). A refused AVCTP channel is a very plausible trigger for "connection
incomplete → power off."

**Sequencing (brainstorm decision): capture-first.** The hypothesis is
unconfirmed and a full AVRCP target is a large clean-room build for one headset's
UX quirk. So piece 3 first CONFIRMS the mechanism with a bench capture (does the
Shokz open AVCTP; does our refusal trigger the power-off; does merely accepting
the channel change it), then builds an AVRCP surface **sized to the capture**.

**Build scope (brainstorm decision): minimal-to-satisfy.** Build the smallest
AVRCP surface the capture shows the Shokz needs to consider the link complete —
not a reusable AVRCP 1.3 target. The capture picks the exact PDUs.

## 2. The capture (bench, real Shokz)

Tooling already exists: `M2_BT_ACL_TRACE` dumps every L2CAP PDU decoded and skips
RTP media (`0x80 0x60`) to avoid the observer effect that once throttled the
tone. AVCTP/AVRCP is low-rate signalling, so tracing it is safe.

Two arms, both `bt_tone_test -DM2_BT_TARGET_NAME=<Shokz> -DM2_BT_ACL_TRACE=ON`
streaming the tone to the Shokz:

- **Arm 1, baseline (current firmware).** Trace from connect through the
  power-off (~1–2 min). Look for, in order: (a) does the Shokz **SDP-query us for
  AVRCP** first (a ServiceSearch for AV Remote Control `0x110E` / target
  `0x110C`); (b) does it open an **L2CAP channel for PSM `0x0017` (AVCTP)**; (c)
  does our stack **refuse it** (CONN_RSP 0x0002). Correlate the power-off timing
  against the refusal — a refused AVCTP channel a minute before the power-off is
  the smoking gun.
- **Arm 2, cheap experiment.** A bench build that merely **accepts** the AVCTP
  channel (one `allowPsm(0x0017)` behind a new `M2_BT_ACCEPT_AVCTP` option), no
  AVRCP logic behind it. Re-trace. If the Shokz stops powering off on the bare
  accepted channel, the trigger was the refusal alone (branch B). If it now sends
  **AV/C over the channel and times out**, the trace shows exactly which PDUs it
  sends — which sizes the minimal target (branch C).

**Deliverable:** a decoded capture in `examples/audio/bt_tone_test/transcript_hw_evkb.txt`
— the SDP query (if any), the `0x0017` CONN_REQ/RSP, the AV/C command list the
Shokz sends, and the power-off timing under each arm. Everything downstream is
sized from it.

## 3. The decision tree

- **A — the Shokz never opens AVCTP.** AVRCP is NOT the trigger (something else:
  sniff/park mode, a role expectation, an idle keepalive, a firmware quirk).
  Record the refutation, re-scope piece 3 as a fresh investigation, build no
  AVRCP. A surprise, but the capture must be allowed to say it.
- **B — it opens AVCTP, we refuse it, bare-accept (arm 2) STOPS the power-off.**
  Trigger was the refusal alone. Fix is tiny: make `allowPsm(0x0017)` + accept
  the default in `A2dpSource` (retire the piece-2 refusal for this one PSM), plus
  the SDP AVRCP-target record if the Shokz SDP-queried. No AV/C logic.
- **C — it opens AVCTP, bare-accept is NOT enough; it sends AV/C and times out /
  still powers off.** Build the minimal AVRCP target (§4), sized to exactly the
  AV/C PDUs the capture shows.
- **D — it SDP-queries for AVRCP, finds no record, and never opens AVCTP.** The
  SDP AVRCP-target record is a prerequisite to reproduce the channel open. Add
  the record first, re-capture, then land in B or C.

Likely path: B or C, one bench capture apart. Nothing is built before the
capture selects the branch.

## 4. The contingent minimal AVRCP target (built for the selected branch)

Clean-room from the AVRCP/AVCTP specs, MIT, no heap — the `Avdtp` discipline.

- **SDP AVRCP-target record** (B, C, D). A static record advertising AV Remote
  Control Target (service class `0x110C`), the AVRemoteControl profile
  (`0x110E`), L2CAP PSM `0x0017` for AVCTP, an AVCTP version, and a
  supported-features mask — data, not logic — served by the existing `SdpServer`
  (which already answers the Shokz's reverse AudioSource query) by adding this
  second record. In D it is the prerequisite that makes the Shokz open the
  channel.
- **AVCTP accept + transport** (C). `allowPsm(0x0017)`, parse the AVCTP header
  (transaction label, packet type, PID = AVRCP `0x110E`), frame responses back on
  that channel. A thin layer under AV/C.
- **AV/C responder, sized to the capture** (C). The anchor is
  **`RegisterNotification(PLAYBACK_STATUS_CHANGED)` → an INTERIM response of
  PLAYING** — how a headset learns the stream is live, the most likely thing that
  stops the idle timer. Around it, ONLY the PDUs the capture shows: almost
  certainly the mandatory AV/C **UNIT INFO / SUBUNIT INFO**, probably
  **GetCapabilities (EVENTS_SUPPORTED)**, possibly **GetPlayStatus** and
  **PASS THROUGH** button ACKs. Anything the capture does not show, we do not
  build.
- **Module.** A new `bt/Avrcp.{h,cpp}` (AVCTP + the AV/C target), wired by
  `A2dpSource` (allow `0x0017`, route that channel's data to `Avrcp`). Branch B
  needs only the SDP record + the accept, not this module.

## 5. Verification and acceptance

- **Gateable proof (if a build happens).** Host tests for `Avrcp` — the AVCTP
  header parse, each AV/C response, the `RegisterNotification → PLAYING` anchor —
  each RED-pinned by a mutant. Then a QEMU gate (a new `bt_tone_test` variant, or
  an extension of the `[media]` fake peer) in which the peer opens AVCTP and
  replays the capture's exact AV/C sequence and asserts our target answers (SDP
  AVRCP record served, AVCTP accepted, UNIT INFO answered, notification →
  PLAYING). Proves the protocol; cannot prove the Shokz's reaction.
- **Silicon acceptance — the un-fakeable claim.** The real Shokz streams the tone
  **well past the 1–2 min power-off (target 5+ minutes)** without powering off,
  ideally announcing "connected." That, in `transcript_hw_evkb.txt`, closes the
  piece. Plus an **ESP32 + OneOdio A70 no-regression** pass — accepting AVCTP and
  serving the AVRCP SDP record must not disturb the sinks that never needed it.

## 6. Non-goals

No AVRCP CONTROLLER role (target only); no volume / metadata / browsing /
play-pause transport control; no AVRCP for sinks that do not require it. The
single objective is stopping the Shokz power-off with the least protocol. A full
AVRCP target, if ever wanted for volume or metadata, is a separate piece this
minimal target would seed. Pieces 4 (credit-leak instrument, done) and 5 (soak
automation) are separate. If the capture lands in branch A, the AVRCP work is not
built and piece 3 re-scopes.

## 7. Files (contingent on the branch)

evkb: `examples/audio/bt_tone_test/{bt_tone_test.cpp, CMakeLists.txt}` (the
`M2_BT_ACCEPT_AVCTP` capture option; later the AVRCP wiring), transcripts.
M2Radio (branch B): `bt/A2dpSource.cpp` (allow `0x0017`), `bt/SdpServer.*` (the
AVRCP record). M2Radio (branch C): NEW `bt/Avrcp.{h,cpp}` + `bt/test/avrcp_test.cpp`,
`bt/A2dpSource.*` wiring. evkb gate + fake peer (`hci_peer.py`) if branch C.
`evkb.cmake` pin, `CLAUDE.md`, memory. The captures and the acceptance are bench
artifacts.

## 8. Capture outcome (bench, 2026-09-07, real Shokz OpenMove `C0:86:B3:31:29:2F`)

Three arms of `bt_tone_test` (bench config: BT-only UART firmware, RXRTSE flow,
3 Mbaud, `M2_BT_ACL_TRACE=ON`, target `Shokz`), console via the MCU-Link VCOM.

- **Arm 1 (baseline, channel refused).** SSP pair by inquiry, `streaming by=inquiry`.
  1.8 s after AVDTP START the Shokz opens **PSM 0x0017 (AVCTP)**; we refuse it
  `CONN_RSP result=0x0002`; it retries 0.5 s later, refused again, then gives up.
  Its only SDP query is the known AudioSource one (0x110A). **It streamed ≈699 s
  with `drops=0` and did NOT power off**; it never announced "connected" (by ear).
  Piece-4 instrument over the run: `sent=30160 returned=30160 starves=0
  starve_max_ms=0 clamp=0` — no credit leak at close range.
- **Arm 2 (`M2_BT_ACCEPT_AVCTP`) — first a SILICON BUG in the capture build
  itself:** the channel was STILL refused (0x0002) with `avctp=accepted` printed,
  because `L2cap`'s allow-list held only TWO PSMs and `A2dpSource` fills them with
  SDP + AVDTP, so the AVCTP entry was silently dropped. No gate could see it (none
  opens three PSMs). Fixed: `MAX_ALLOW=4`, `allowedCount()`, host test A5 (three
  accepted, an unlisted fourth refused) RED-pinned by the capacity-2 mutant
  (M2Radio `0084bc2`). Rebuilt: the channel is **accepted and configured** (our
  dcid 0x0081, MTU 1004/672). The Shokz then opens a fresh SDP channel and asks
  **{0x110C AV Remote Control Target}** with attributes {0x0009, 0x0311}, gets an
  empty list, then asks **{0x110E}** the same way, gets an empty list — and sends
  **no AV/C at all** (0 frames in ≈333 s). So the record is the prerequisite for
  the AV/C to START, not for the channel open (the design's branch D, in a
  variant: it opens AVCTP first, then asks). Also on this arm, piece 1/2 silicon
  evidence: after the abrupt reflash the Shokz would not accept our page (page
  timeout ×2, status 0x04) but **paged US**; we accepted as slave with the stored
  key and it drove AVDTP as initiator (`streaming by=incoming`, `role=s`). ★ A
  piece-2 defect surfaced: the boot walk's failed OUTBOUND attempt (`connect=fail
  reason=no_inquiry_hit`) tore down the freshly-authenticated INBOUND link
  (`disconnect reason=0x16`); the Shokz paged again and the second attempt
  streamed — filed for piece 2's follow-up.
- **Arm 3 (AVRCP Target record served).** `Sdp` is now multi-record: record 2
  (handle 0x00010001) = ServiceClassIDList {0x110C}, ProtocolDescriptorList
  {L2CAP psm 0x0017, AVCTP 0x0104}, BluetoothProfileDescriptorList {0x110E,
  0x0104}, SupportedFeatures 0x0001 (Category 1); a search for 0x110E matches it
  too; ServiceSearch lists both handles (M2Radio `c2a4025`, `sdp_test` 39 checks,
  the Shokz's two queries answered byte-for-byte, RED-pinned by `N_RECORDS=1`).
  Paged with the stored key (`by=paged`). The Shokz then sent its first AV/C
  frame on the AVCTP channel:
  `20 11 0E | 03 48 00 | 00 19 58 | 31 00 | 00 05 | 01 00 00 00 00` — AVCTP
  tl=2 command PID=AVRCP; AV/C ctype NOTIFY, panel subunit, VENDOR DEPENDENT,
  Bluetooth SIG; PDU 0x31 **RegisterNotification**, event 0x01
  **PLAYBACK_STATUS_CHANGED**, interval 0. We sent no response (no responder yet)
  and it sent nothing further; it kept streaming (≈471 s, `drops=0`).

**Branch:** D → C. The prerequisite record is built; the responder's first PDU is
known. **Task 4 (the minimal `Avrcp` responder) is sized to:** an INTERIM response
to RegisterNotification(PLAYBACK_STATUS_CHANGED) carrying PLAYING (`0F 48 00 00 19
58 31 00 00 02 01 01`, same AVCTP transaction label, C/R = response), and whatever
the Shokz sends AFTER it receives that (a re-capture with the responder decides).
**The self-power-off did not reproduce in any of the three arms** (≈699 / ≈333 /
≈471 s); the original observation came from the acid_box build on the pre-piece-2
stack. "Connected" was never announced (arm 1 by ear; arms 2/3 not reported).

## 9. Task 4 built (2026-09-07, same bench session)

- **`bt/Avrcp.{h,cpp}`** (M2Radio `8e22082`): AVCTP framing (transaction label / packet
  type / C-R / IPID, PID 0x110E) + the AV/C target sized to §8's capture.
  `RegisterNotification(PLAYBACK_STATUS_CHANGED)` → INTERIM (`0F`) + PLAYING on the
  same transaction label (`22 11 0E 0F 48 00 00 19 58 31 00 00 02 01 01`); every other
  AV/C command → NOT IMPLEMENTED (`08`, operands echoed) so a peer never hangs on an
  unanswered transaction; a foreign PID → the AVCTP IPID reply; fragments and
  response frames ignored. RX records one command slot, TX from `service()` (B6).
  `A2dpSource` now allows PSM 0x0017 UNCONDITIONALLY (the `setAllowAvctp` capture hook
  and the example's `M2_BT_ACCEPT_AVCTP` option are gone), routes the channel to
  `Avrcp`, services it beside the SDP server (logging `avrcp: register_notification
  playback_status -> interim playing` the first time each attempt) and resets it with
  `Avdtp`. `avrcp_test` 15 checks, RED-pinned by the PLAYING→STOPPED and
  INTERIM→ACCEPTED mutants.
- ★ **A latent library bug surfaced when the member was added**: `a2dpsource_test`
  SEGV'd in `L2cap::service()` with `m_txHead` read as 155. `L2cap` initialised its
  state only in `begin()`, and `A2dpSource::service()` ticks it from boot before any
  attempt has called `begin()` — a stack-allocated host object therefore serviced a
  garbage TX queue, harmless while the garbage happened to be zero and fatal once
  `A2dpSource` grew by 84 bytes. Static firmware objects are zero-filled, which is why
  silicon never saw it. Fixed with in-class initialisers matching `begin()`; `l2cap_test`
  A6 pins `service()`-before-`begin()` as inert. Bisected by shifting the member
  (layout-independent), by stack size (64 MB still crashed) and by `-O2` (passed) —
  the crash report's registers named the instruction.
- **Arm 4 (responder, GetCapabilities still NOT IMPLEMENTED):** the Shokz sent
  `GetCapabilities(EVENTS_SUPPORTED)` FIRST (`10 11 0E 01 48 00 00 19 58 10 00 00 01
  03`), took our NOT IMPLEMENTED, then `RegisterNotification` and took our INTERIM
  PLAYING; nothing further; streamed ≈18 k packets, `drops=0`, no power-off.
  GetCapabilities was then built (M2Radio `fea6d52`: STABLE `0C … 10 00 00 03 03 01
  01` for EVENTS_SUPPORTED, the SIG id for COMPANY_ID; RED-pinned by an empty event
  list).
- **Arm 5 (both PDUs answered) — the complete exchange, on the wire:** AVCTP opened
  (this time during the INBOUND bring-up, before `a2dp=ok`), ONE SDP query (0x110C —
  the Target record satisfied it, no 0x110E follow-up), then
  `GetCapabilities(EVENTS_SUPPORTED)` → our STABLE `{PLAYBACK_STATUS_CHANGED}`, then
  `RegisterNotification(PLAYBACK_STATUS_CHANGED)` → our INTERIM PLAYING, then nothing.
  **The Shokz's AV/C set is two PDUs, both now answered.** Streaming `by=incoming`,
  `drops=0`. Six A2DP-path gates re-run PASS against the library (`[avdtp]`
  `[reconnect]` `[media]` `[lifecycle]` `[soak]` card-absent).
- **Still open:** whether the Shokz announces "connected" now (a bench observation),
  the fake-peer gate for the exchange (plan Task 5), the 5+ min acceptance run.


## 10. The gate (2026-09-07)

No new gate: the `[media]` fake peer (`hci_peer.py`, media phase) is now also the
headset's AVRCP controller, Shokz-shaped — 1.8 s after AVDTP START it opens AVCTP
at us, sends GetCapabilities(EVENTS_SUPPORTED) then
RegisterNotification(PLAYBACK_STATUS_CHANGED) verbatim from the capture, and
requires our STABLE {PLAYBACK_STATUS_CHANGED} and INTERIM PLAYING byte for byte.
`run_qemu_media.sh` asserts `PEER-AVRCP state=done caps=1 notif=1 bad=0` and the
firmware's `avrcp: register_notification playback_status -> interim playing` line,
with tripwires for a wrong response, a refused channel and unexpected AV/C.
**Demonstrated RED three ways by name:** INTERIM→ACCEPTED, an empty event list,
and the pre-piece-3 allow-list (channel refused 0x0002). Fixture re-captured;
vacuity gains `noavrcp_fixture_fails_media_gate` (the peer tally is unreachable
under the fake QEMU, so the UART-side assertion carries that replay).
