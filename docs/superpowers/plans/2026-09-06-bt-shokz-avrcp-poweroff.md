# Shokz self-power-off — AVRCP capture + minimal target (NEW-34 piece 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.
>
> **This plan is CAPTURE-FIRST and BENCH-GATED.** Task 1 is a small software prerequisite; Task 2 is a bench capture on a real Shokz that SELECTS a branch (A/B/C/D); Tasks 3+ are CONTINGENT on that branch and cannot be fully specified until the capture. Do not build the AVRCP module speculatively — execute Task 1, then stop for the bench, then resume on the selected branch.

**Goal:** Stop the Shokz OpenMove self-powering-off ~1–2 min into an A2DP stream, with the least protocol — first confirming the missing-AVRCP hypothesis by bench capture, then building a minimal AVRCP surface sized to what the capture shows.

**Architecture:** Capture-first. The stack currently REFUSES the Shokz's AVCTP (PSM 0x0017) channel (piece-2 `allowPsm`); the capture establishes whether that refusal is the trigger and what AVRCP, if any, the Shokz needs. Any build is minimal-to-satisfy, clean-room, gated against a fake peer; the fix's proof is silicon (a real Shokz, 5+ min, no power-off).

**Tech Stack:** M2Radio `bt/` (C++11, host-compiled), evkb Arduino examples, `M2_BT_ACL_TRACE` capture tooling, the custom QEMU machine + fake HCI peer.

**Spec:** `docs/superpowers/specs/2026-09-06-bt-shokz-avrcp-poweroff-design.md` (read first — §3 is the decision tree that governs Tasks 3+).

---

## Ground rules (same as pieces 1–2 & 4)

- Library = `~/Development/M2Radio` (pushed + pinned separately); firmware/docs = `~/Development/rt1170/evkb` on `master`. evkb resolves M2Radio local-first.
- Commit with an explicit pathspec, never `git add -A`. End messages with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
- Every new host-test pin shown RED by a mutant (scratch copy) before trusted; every gate assertion demonstrated RED by name.
- Run a QEMU gate as `./run_qemu_X.sh`, never `sh`. Licence audit AFTER a sweep, never during.
- **Never per-packet-console-trace a high-rate BT stream** — `M2_BT_ACL_TRACE` already skips RTP media (`0x80 0x60`); keep it that way (the documented observer effect).

---

## Task 1: the `M2_BT_ACCEPT_AVCTP` capture option (software; buildable now)

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/CMakeLists.txt`
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/bt_tone_test.cpp`

**Context:** `A2dpSource` sets the L2CAP allow-list to `{Sdp::PSM, Avdtp::PSM}` (piece 2), so an inbound AVCTP (0x0017) channel is refused. Arm 2 of the capture needs a build that ACCEPTS 0x0017 with no AVRCP logic, to see whether the bare accept changes the Shokz. Expose it as an opt-in so the default build (and every gate) is byte-identical.

- [ ] **Step 1: Add the CMake option.** In `bt_tone_test/CMakeLists.txt`, beside the other `M2_BT_*` options:
```cmake
# NEW-34 piece 3 capture: accept the peer's AVCTP (PSM 0x0017) channel instead of refusing it (piece-2
# allow-list), to see whether the refusal alone triggers the Shokz self-power-off.  No AVRCP logic -- a
# diagnostic build only.  OFF by default so the default image and every gate stay byte-identical.
option(M2_BT_ACCEPT_AVCTP "Bench capture: accept the peer's AVCTP/AVRCP L2CAP channel (0x0017)" OFF)
if(M2_BT_ACCEPT_AVCTP)
    add_definitions(-DM2_BT_ACCEPT_AVCTP=1)
endif()
```

- [ ] **Step 2: Allow the PSM when the option is set.** `A2dpSource` owns the allow-list, so expose a hook and call it from the example. In `~/Development/M2Radio/bt/A2dpSource.h`, the `L2cap &l2()` accessor already exists (used by AudioOutputBluetooth). In `bt_tone_test.cpp`'s `setup()`, after `src.setBonds(...)` / before `session.begin(...)`, add:
```cpp
#if defined(M2_BT_ACCEPT_AVCTP)
    src.setAllowAvctp(true);   // NEW-34 piece 3 capture: accept 0x0017 (see M2_BT_ACCEPT_AVCTP)
    CONSOLE.println("avctp=accepted (capture build)");
#endif
```
and in `A2dpSource` add the one-line hook: a `bool m_allowAvctp = false;`, a `void setAllowAvctp(bool v) { m_allowAvctp = v; }`, and in the L2CAP-setup path (where it currently does `m_l2.allowPsm(Avdtp::PSM); m_l2.allowPsm(Sdp::PSM);`) add `if (m_allowAvctp) m_l2.allowPsm(0x0017);`. (This is an M2Radio change — commit it with the example, or as its own M2Radio commit; it is additive and default-off.)

- [ ] **Step 3: Build both images.**
```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake >/tmp/c.log 2>&1 && cmake --build build >/tmp/b.log 2>&1 && ./run_qemu.sh 2>&1 | tail -3   # default: card-absent gate PASS, byte-identical behaviour
rm -rf build-avctp && cmake -B build-avctp -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DM2_BT_ACCEPT_AVCTP=ON -DM2_BT_TARGET_NAME=<Shokz> -DM2_BT_ACL_TRACE=ON >/tmp/ca.log 2>&1 && cmake --build build-avctp >/tmp/ba.log 2>&1 && echo "capture build OK"
```
Expected: default gate PASS; capture build compiles. (Replace `<Shokz>` with the Shokz's inquiry-name substring.)

- [ ] **Step 4: Commit.** M2Radio: `bt/A2dpSource.{h,cpp}` (the `setAllowAvctp` hook, additive). evkb: `bt_tone_test.cpp` + `CMakeLists.txt`. Two commits (M2Radio then evkb), messages naming the capture purpose, `Co-Authored-By` trailer. If M2Radio changed, bump the `evkb.cmake` pin in the evkb commit (fresh-user verify deferred to the close-out).

---

## Task 2: the bench capture (USER, on hardware) — selects the branch  ★ DONE 2026-09-07: branch D → C (spec §8); two silicon fixes landed on the way (L2cap allow-list capacity, M2Radio 0084bc2; multi-record Sdp with the AVRCP Target record, M2Radio c2a4025 = Task 3 done)

**Files (evidence):** `examples/audio/bt_tone_test/transcript_hw_evkb.txt`.

Real Shokz OpenMove required. Flash, free-run, read the console with `tools/rt1170-console.py`. See spec §2/§3.

- [ ] **Arm 1 — baseline.** Flash `build/` (default, current firmware, `-DM2_BT_ACL_TRACE=ON -DM2_BT_TARGET_NAME=<Shokz>` — build a `build-trace` for this). Stream the tone; trace from connect through the power-off (~1–2 min). Record: (a) any SDP query for AVRCP (`0x110E`/`0x110C`); (b) the L2CAP CONN_REQ for PSM `0x0017`; (c) our CONN_RSP (expect refuse 0x0002); (d) the power-off timing vs the refusal.
- [ ] **Arm 2 — cheap accept.** Flash `build-avctp/` (accepts 0x0017). Re-trace. Record: does the power-off stop? If not, the AV/C PDUs the Shokz sends over the accepted channel (and whether it times out waiting for responses).
- [ ] **Select the branch (spec §3):** A (no AVCTP → not AVRCP, re-scope), B (bare-accept stops it → tiny fix), C (needs AV/C → minimal target), or D (SDP record needed first). Paste the decoded capture into the transcript and record the selected branch. **Tasks 3+ execute only the selected branch.**

---

## Task 3 (branches B/C/D): SDP AVRCP-target record + accept AVCTP by default  ★ RECORD DONE (M2Radio c2a4025, bench-verified: the Shokz answers with RegisterNotification). STILL TO DO: make allowPsm(0x0017) the A2dpSource DEFAULT (retire the capture option) — fold into Task 4.

**Files:** `~/Development/M2Radio/bt/SdpServer.{h,cpp}` (or `bt/Sdp.*`), `bt/A2dpSource.cpp`.

**Context:** `SdpServer` already answers the Shokz's reverse AudioSource query. Add a second static record: AV Remote Control Target (service class `0x110C`), AVRemoteControl profile (`0x110E`, version from the capture), L2CAP `0x0017`, AVCTP version, supported-features mask. Make accepting 0x0017 the default in `A2dpSource` (retire the piece-2 refusal for this one PSM — the allow-list keeps refusing everything else).

- [ ] **Step 1:** Read the current `SdpServer` record-serving path and the Shokz's SDP query from the Task-2 capture (its exact ServiceSearchAttribute request for AVRCP). Write the response record to match what the Shokz asks for (host-test the encoder against the captured request/response bytes, the `sdp_test` pattern).
- [ ] **Step 2:** In `A2dpSource`, add `m_l2.allowPsm(0x0017)` to the L2CAP-setup path unconditionally (remove the `M2_BT_ACCEPT_AVCTP` gate — it becomes the default), and route the 0x0017 channel's inbound data: in branch B, ACK/ignore it; in branch C, to the `Avrcp` module (Task 4).
- [ ] **Step 3:** Host test in `sdp_test`: the AVRCP-target record encodes to the bytes the Shokz's query expects (RED-pinned). Build bt_tone_test; the card-absent gate stays PASS; existing `[media]`/`[avdtp]`/`[reconnect]` gates unaffected (0x0017 is only opened by a peer that asks — the fake peers don't, so no gate output moves — confirm by re-running them).
- [ ] **Step 4: Commit** (M2Radio SdpServer + A2dpSource; evkb pin bump). **Branch B ends here** → go to Task 6 (silicon acceptance). Branches C/D continue to Task 4.

---

## Task 4 (branch C): the minimal `Avrcp` target — sized to the capture  ★ BUILT 2026-09-07 (M2Radio 8e22082, spec §9): RegisterNotification → INTERIM PLAYING, NOT IMPLEMENTED for the rest, 0x0017 accepted by default; found + fixed L2cap's uninitialised-before-begin() state on the way. Re-capture (arm 4) pending; the fake-peer gate (Task 5) after it

**Files:** Create `~/Development/M2Radio/bt/Avrcp.{h,cpp}`, `bt/test/avrcp_test.cpp`; modify `bt/A2dpSource.{h,cpp}`.

**Context:** Clean-room from the AVRCP/AVCTP specs, MIT, no heap — the `Avdtp` discipline. Build ONLY the AV/C PDUs the Task-2 capture shows the Shokz send. The skeleton below is the LIKELY set; the capture is the authority — add nothing it does not show, and if it shows a PDU not listed here, add that one.

- [ ] **Step 1: AVCTP transport.** Parse the AVCTP header (transaction label + packet type in byte 0; PID = AVRCP `0x110E` in bytes 1–2) from the 0x0017 channel's inbound L2CAP data; a `respond(tl, pdu, len)` that frames an AVCTP response (same tl, response bit) and sends via `L2cap::send` on the AVCTP channel. Host-test the header parse + framing (RED-pinned).
- [ ] **Step 2: the AV/C responder, per the capture.** The ANCHOR (build regardless if the Shokz registers it): `RegisterNotification(PLAYBACK_STATUS_CHANGED)` → an AVCTP AV/C INTERIM response carrying `PLAYING`. Around it, ONLY what the capture shows — likely: AV/C `UNIT INFO` / `SUBUNIT INFO` (mandatory: report a panel subunit), `GetCapabilities(EVENTS_SUPPORTED)` (report PLAYBACK_STATUS_CHANGED, plus TRACK_CHANGED if the capture asks), possibly `GetPlayStatus` and `PASS THROUGH` ACKs. Each response host-tested against the captured request bytes; each RED-pinned.
- [ ] **Step 3: wire into `A2dpSource`.** Instantiate `Avrcp`, route the 0x0017 channel's data to `m_avrcp.onData(...)`, and drive it from `service()` (the same tick shape as `m_avdtp`). Reset it on teardown (like `m_avdtp.reset()`).
- [ ] **Step 4:** Run the full host suite (`./bt/test/run.sh`) → `avrcp_test` + all others green. Build bt_tone_test.
- [ ] **Step 5: Commit** (M2Radio Avrcp + A2dpSource; evkb pin bump).

---

## Task 5 (branch C): the QEMU gate — fake peer replays the capture's AVCTP/AV/C  ★ DONE 2026-09-07 as an extension of the [media] peer (spec §10): PEER-AVRCP tally + UART line, RED three ways, fixture re-captured, vacuity negative added; gate count unchanged at 132

**Files:** `examples/networking/m2_hci_probe/hci_peer.py` (or the `bt_tone_test` peer), a new `run_qemu_avrcp.sh` or a `[media]`-peer extension, transcript.

- [ ] **Step 1:** Extend the fake peer to, after AVDTP START, open an AVCTP channel (CONN_REQ PSM 0x0017) and send the exact AV/C sequence the Task-2 capture recorded. Assert our target answers: the SDP AVRCP record was served, AVCTP accepted, UNIT INFO answered, `RegisterNotification → PLAYING`. Tripwires for each.
- [ ] **Step 2:** New gate (`bt_tone_test[avrcp]` or the `[media]` peer grown an AVRCP phase). Demonstrate RED (SDP record absent; AVCTP refused; notification not PLAYING). Capture the transcript. Sweep count 131 → 132 (or unchanged if folded into `[media]`).
- [ ] **Step 3:** Vacuity negatives for the new gate; commit.

---

## Task 6 (all branches): silicon acceptance (USER) + close-out

**Files (evidence):** `examples/audio/bt_tone_test/transcript_hw_evkb.txt`, `examples/display/acid_box/transcript_hw_evkb_bt.txt`.

- [ ] **Silicon acceptance.** The real Shokz streams the tone **5+ minutes without powering off** (well past the 1–2 min timer), ideally announcing "connected." Record in the transcript.
- [ ] **No-regression.** ESP32 `EVKB-SINK` and OneOdio A70 still reach STREAMING and play — accepting AVCTP + serving the AVRCP record must not disturb them.
- [ ] **Close-out (software side).** If a build happened: push M2Radio, bump the pin, fresh-user verify (run a bt gate against the fetched ELF), rebuild the bt gate dirs, sweep (131 or 132), `LICENSE-AUDIT: PASS`, CLAUDE.md + spec status + memory, commit, finish per `superpowers:finishing-a-development-branch` (push decision). If branch A: record the refutation in CLAUDE.md/memory and re-scope piece 3.

---

## Self-review (completed by the plan author)

- **Spec coverage:** §2 capture → Tasks 1–2; §3 decision tree → Task 2 selects, Tasks 3–5 branch on it; §4 minimal target → Tasks 3–4; §5 verification → Tasks 4–6; §6 non-goals honoured (target only, no volume/metadata, minimal set). Covered.
- **Capture-first honoured:** no AVRCP module is built before Task 2 selects the branch; Task 1 is only the diagnostic accept option; the AV/C set is explicitly "sized to the capture," not pre-listed as final.
- **Type consistency:** `setAllowAvctp` (Task 1) and `m_allowAvctp`/`allowPsm(0x0017)` (Tasks 1, 3) are the same hook; `Avrcp` (Tasks 4, 5) is one module; the anchor `RegisterNotification → PLAYING` is named identically in the spec and Tasks 4/5.
- **Bench-gated, stated up front:** the header and Task 2 make clear the plan pauses for hardware and resumes on a branch.
- **No placeholders** in the specifiable parts (Task 1 full code; Task 3 SDP record method; Task 4 skeleton is deliberately capture-sized, not a placeholder — the capture is the spec).
