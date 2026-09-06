# M2Radio BT: Shokz self-power-off — AVRCP capture + minimal target (NEW-34 piece 3)

**Status:** design APPROVED 2026-09-06 (brainstorm in session); plan follows.
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
