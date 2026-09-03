# Headset AVDTP DISCOVER — Diagnose & Fix — Design

**Date:** 2026-09-03
**Programme:** M.2 Bluetooth A2DP (NEW-9, BT-3). Follow-up to the phase-4
size-cap fix (tone audible on the ESP32 sink).
**Status:** Design, pending user review.

## Problem

A2DP media now flows to the ESP32 sink, but the ESP32 is throughput-limited
(~128 kbps → choppy) — the path to CLEAN audio is a real headset. On silicon the
Shokz "OpenMove by Shokz" **pairs over SSP end-to-end** (Just Works, IO-cap
NoInputNoOutput, `encryption=on`, `paired_by=ssp`) but **AVDTP hangs at
DISCOVER**: `avdtp_state=1` (DISCOVERING), `avdtp_err=0x00` — the L2CAP
signalling channel opens, we send `DISCOVER [0x10,0x01]`, and the headset never
responds. The same `A2dpSource`/`Avdtp` code reaches STREAMING against the
ESP32 sink.

### What we know

- **The headset path in the extracted `M2Radio/bt` `A2dpSource`/`Avdtp` was
  never tested.** BT-2 reached AVDTP DISCOVER with headsets in the
  `m2_hci_probe` PROTOTYPE; that code was then extracted + reviewed into
  `M2Radio/bt`, and only the ESP32 sink was tested against the extracted
  `A2dpSource` (phase 2). So this is an ESP32-tolerated / headset-strict
  difference, or an extraction regression only headsets exercise.
- **The DISCOVER path** (`Avdtp::start`/`service`/`onSignalling`): requires the
  signalling channel `OPEN`, sends `DISCOVER`, waits for a response with the
  matching transaction label. `state=1` = that response never arrived.
- **Leading hypothesis — the L2CAP config exchange.** Our channel reaches `OPEN`
  once WE have sent our config req and received a rsp, and answered the peer's
  config req. But we send an **empty CONFIG_REQ** (no MTU option) and our
  CONFIG_RSP **echoes the peer's options verbatim**. If the Shokz is stricter
  than the ESP32 about that exchange, its side never fully opens and it
  **silently discards our DISCOVER** — exactly the failure mode of the past
  cfg-rsp SCID bug (BT-2). Other candidates below.

We cannot design the fix until we see the bytes. This is a **diagnosis-first**
sub-project.

## Goal

Reach AVDTP **STREAMING** to the Shokz headset on silicon — DISCOVER →
GET_CAPABILITIES → SET_CONFIGURATION → OPEN → START → media — and hear a clean
1 kHz tone (which also tests the throughput theory: a compliant headset should
sustain the ~328 kbps SBC the ESP32 sink could not).

## Non-goals

- The ESP32 sink's throughput / NCP-freeze (separate follow-up).
- AVRCP, absolute volume, or any A2DP feature beyond a single SBC stream.
- Over-the-air BR/EDR sniffing hardware (not needed — see the reference capture).
- Phase 5 (Acid Box over BT + uAP coexistence).

## Design

Three phases: get visibility, fix what the visibility reveals, verify + guard.

### Phase 1 — Diagnostic instrument + reference capture

The deliverable is **complete, decodable ACL visibility on our link**, compared
against a **known-good reference for this exact headset**.

**1a. `M2_BT_ACL_TRACE` — every ACL packet on our link, as text over VCOM.**
- Add an optional trace callback to `L2cap`:
  `void L2cap::onAclTrace(void (*fn)(void *ctx, bool out, const uint8_t *acl, uint16_t len), void *ctx)`.
  `L2cap` invokes it for **every inbound ACL** (in `onAcl()`, BEFORE demux — so
  it catches traffic on CIDs/PSMs we do not route, e.g. a reverse SDP/AVDTP the
  Shokz opens) and **every outbound ACL** (in `service()`'s write loop). `L2cap`
  stays Arduino-free — the callback is the same pattern as `BtLink::LogFn`.
- The example (`bt_tone_test`, under `M2_BT_ACL_TRACE`) wires it to a hex dumper
  over `CONSOLE`, one line per packet:
  `acl_trace dir=<in|out> t=<micros> hex=<02 01 00 ...>`.
  (Text hex over the VCOM, not binary — avoids binary-over-serial hazards.)
- OFF by default: no gate or default-build behaviour changes.

**1b. `tools/acl-trace-to-btsnoop.py` — hex log → standard btsnoop file.**
- Parses the `acl_trace` lines and writes a **btsnoop** file (the standard HCI
  log format): the 16-byte header (datalink type HCI-UART/H4 = 1001), then one
  record per packet — original/included length, the direction flag (0 = sent /
  host→controller for `out`, 1 = received for `in`), the microsecond timestamp,
  and the packet bytes prefixed with the H4 ACL type byte `0x02`.
- Output opens directly in **Apple PacketLogger** (or Wireshark) — fully decoded
  L2CAP + AVDTP, no custom decoder needed.

**1c. Reference capture — Mac → Shokz, in Apple PacketLogger.**
- Pair the Shokz to the Mac, play audio (the Mac is a compliant, reference A2DP
  SOURCE that streams to this exact headset), capture the session in PacketLogger
  (Apple's "Additional Tools for Xcode"; logs the Mac's local controller HCI,
  fully decoded — no extra hardware).
- This is the **golden template** for THIS headset: the exact L2CAP CONFIG_REQ
  options (MTU etc.), config sequence, and AVDTP DISCOVER/GET_CAP/SET_CONFIG it
  accepts. Better than the ESP32 reference (another non-standard sink).

**1d. Compare.** Diff our EVKB btsnoop (1a+1b) against the Mac's PacketLogger
capture (1c). The discrepancy — which config option we omit, which response we
malform, which reverse flow we ignore, or whether our DISCOVER even leaves — is
the bug. (An ESP32-→-Shokz-EVKB comparison is an optional secondary reference.)

### Phase 2 — Fix (contingent on Phase 1)

The comparison resolves the cause to exactly one of these; the fix lands in
`M2Radio/bt` (`L2cap` or `Avdtp`):

- **(a) DISCOVER never leaves** (stuck in the txq / send returns false) → send /
  ordering fix in `Avdtp::service()` or `L2cap`.
- **(b) DISCOVER leaves, Shokz silent** → our L2CAP config exchange is wrong for
  the Shokz (empty CONFIG_REQ, echo-only CONFIG_RSP, or an option it requires) →
  send a proper CONFIG_REQ (with the MTU the Mac uses) and handle the Shokz's
  CONFIG correctly so its side actually opens. **(Leading hypothesis.)**
- **(c) Shokz answers on a CID/PSM we do not route** → routing fix in
  `L2cap::onAcl`/`A2dpSource::onData`.
- **(d) Shokz drives a reverse flow** (its own DISCOVER/SDP of us) that blocks it
  → handle that flow (`A2dpSource`/`Avdtp` already answer a peer DISCOVER; extend
  as the capture requires).

The exact change is written once Phase 1 names the cause. It MUST preserve the
ESP32 path (Phase 3).

### Phase 3 — Verification + regression guard

- **Silicon (the acceptance):** with the Shokz freshly in pairing mode, the
  bench build reaches `avdtp` STREAMING and a **clean, continuous 1 kHz tone**
  plays on the headset — sustained ≥ 60 s, no dropouts. Capture the
  DISCOVER→…→START sequence + a streaming heartbeat window into
  `transcript_hw_evkb.txt`.
- **No ESP32 regression:** the ESP32 sink still reaches STREAMING (the phase-4
  audible path); `bt_tone_test` (`run_qemu.sh`, `run_qemu_media.sh`) and
  `m2_hci_probe` (`[avdtp]`, others) QEMU gates green; BT host tests green;
  full sweep unchanged count; `LICENSE-AUDIT: PASS`.
- **Regression guard (demonstrated-red):** whatever the fix, add a check that
  fails against the old code —
  - if it is an L2CAP config-exchange correctness fix: an `l2cap_test` host case
    asserting the CONFIG_REQ/RSP bytes, AND/OR tighten the `m2_hci_probe`
    `[avdtp]` gate's fake peer (`hci_peer.py`) to REQUIRE the correct config
    exchange before it answers DISCOVER — so a driver that regresses to the old
    exchange fails the gate;
  - if it is an `Avdtp` change: an `avdtp_test` host case.
  The guard must be shown RED against the pre-fix code, then GREEN after — a
  regression gate never shown to fail is decoration.

## Testing

`m2_hci_probe`'s `[avdtp]` QEMU gate already exercises AVDTP DISCOVER→…→START
against `hci_peer.py`'s fake AVDTP peer — that fake peer is where a
config-strictness guard is added, so the fix is provable without the headset.
The headset itself is silicon-only (the acceptance). Host tests
(`l2cap_test`/`avdtp_test`) guard the byte-level fix deterministically. This is
the same two-gate discipline as the rest of the tree: a deterministic
automated guard + silicon as the real proof.

## Acceptance criteria

- [ ] `M2_BT_ACL_TRACE` (opt-in) emits in/out ACL over VCOM;
      `tools/acl-trace-to-btsnoop.py` produces a btsnoop that opens decoded in
      PacketLogger/Wireshark. Gates/default build unchanged.
- [ ] Mac→Shokz reference captured in PacketLogger; EVKB→Shokz captured via the
      trace; the discrepancy identified and written down.
- [ ] Fix in `M2Radio/bt` reaches AVDTP **STREAMING** with the Shokz on silicon;
      **clean tone ≥ 60 s** on the headset. `transcript_hw_evkb.txt` captures it.
- [ ] ESP32 path still STREAMING (no regression); `bt_tone_test` + `m2_hci_probe`
      QEMU gates green; BT host tests green; sweep count unchanged;
      `LICENSE-AUDIT: PASS`.
- [ ] Demonstrated-red regression guard (host test and/or tightened `[avdtp]`
      fake peer) added and shown red-then-green.
- [ ] Pins bumped (`M2Radio`) + fresh-user `-DEVKB_FORCE_FETCH=ON` verified;
      pushed. Memory + NEW-9 updated.

## Risks & open questions

- **The capture may reveal (a)/(c)/(d) rather than the (b) config hypothesis.**
  That is the point of diagnosis-first; the fix in Phase 2 is written to the
  cause the capture names, not assumed.
- **PacketLogger availability:** ships with Apple's "Additional Tools for Xcode"
  (free). If not installed, a one-time download; Wireshark decodes the same
  btsnoop as a fallback.
- **Headset bonding across EVKB resets:** the EVKB loses its stored link key on
  reflash while the Shokz keeps its bond, and the Shokz EXITS discoverable after
  pairing — put it in explicit PAIRING mode per attempt (power-cycle only gives
  reconnect mode). Captured in bench mechanics.
- **Throughput on the headset:** reaching STREAMING is necessary; CLEAN audio
  also depends on the headset sustaining the SBC bitrate. Expected (a compliant
  headset does), but if it is choppy too, that folds into the separate throughput
  thread rather than blocking this one — STREAMING + the DISCOVER fix is this
  sub-project's acceptance.

## File-by-file changes

**`M2Radio` (sibling repo — pin bump after push):**
- `bt/L2cap.h`, `bt/L2cap.cpp` — `onAclTrace()` hook + the trace calls; the
  Phase-2 config fix (contingent).
- `bt/Avdtp.cpp` (+ `.h`) — the Phase-2 fix if the cause is AVDTP-side.
- `bt/test/l2cap_test.cpp` / `avdtp_test.cpp` — the demonstrated-red host guard.

**`rt1176-evkb` (this repo):**
- `examples/audio/bt_tone_test/CMakeLists.txt` — `option(M2_BT_ACL_TRACE ...)`.
- `examples/audio/bt_tone_test/bt_tone_test.cpp` — wire the trace to a VCOM hex
  dumper under `M2_BT_ACL_TRACE`.
- `examples/audio/bt_tone_test/transcript_hw_evkb.txt` — silicon evidence.
- `tools/acl-trace-to-btsnoop.py` — the hex-log → btsnoop converter.
- `examples/networking/m2_hci_probe/hci_peer.py` — tighten the `[avdtp]` fake
  peer's config-exchange requirement (regression guard), if that is the fix.
- `evkb.cmake` — bump the `M2Radio` pin after push.
- Memory (`m2-bluetooth-a2dp-programme.md`) + NEW-9.

## Bench mechanics (carried from the phase-4 session)

- Real BT firmware: `-DM2RADIO_IW416_BT_FW=…/uartIW416_bt.bin.inc` (gate builds
  use a 1 KB SYNTHETIC image → HCI dead → `streaming=0`).
- `-DM2_BT_CONNECT_RETRY=ON` (retry the flaky one-shot connect from `loop()`),
  `-DM2_BT_LEGACY_PIN=OFF` (SSP for the headset), `-DM2_BT_TARGET_NAME=Shokz`.
- Headset in **explicit PAIRING mode per attempt** (power-cycle → reconnect mode,
  not discoverable). LinkServer `run` is the only reliable reset+VCOM (physical
  reset halts on a DEMCR vector catch when attached; drops the VCOM CDC when not).
- Clean-room discipline: L2CAP/AVDTP facts from the specs and our own captures;
  no third-party stack source transcribed. Stage specific files, never
  `git add -A`.
