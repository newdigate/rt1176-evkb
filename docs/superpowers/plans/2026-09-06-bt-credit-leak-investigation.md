# BT A2DP credit-leak soak investigation (NEW-34 piece 4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Instrument the A2DP L2CAP credit path (host-side) so a long silicon soak can confirm or deny a residual lost-NCP credit leak, and prove the instrument fires on a modeled leak — building no fix unless the soak shows one.

**Architecture:** Investigation-first. Extend `L2cap` with cumulative credit counters beside the existing `creditsMin()`; log them on the examples' heartbeats; prove the accounting (and its leak detection) in `l2cap_test`; run the soak on the bench. No new QEMU gate — QEMU's media drop is a timing artifact, so the instrument's *reading* is a silicon claim while its *correctness* is a host-test claim.

**Tech Stack:** C++11 (M2Radio `bt/`, host-compiled `-Wall -Wextra -Werror`), Arduino (evkb examples), the custom QEMU machine + fake HCI peer, `tools/gate-lib.sh`.

**Spec:** `docs/superpowers/specs/2026-09-06-bt-credit-leak-investigation-design.md` (read it first).

---

## Ground rules (same as pieces 1–2)

- Library = `~/Development/M2Radio` (pushed + pinned separately); firmware/docs = `~/Development/rt1170/evkb` (on `master`, per the established piece-1/2 pattern). evkb resolves M2Radio local-first, so builds pick up local changes with no pin bump until the close-out.
- Host suites run FROM `~/Development/M2Radio` (`./bt/test/run.sh`); the runner GLOBs `bt/*.cpp`, so no new file needs registering here (this piece adds no new `bt/` source).
- Commit with an explicit pathspec, never `git add -A`. End every commit message with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
- Every new host-test pin must be shown RED by a mutant on a SCRATCH COPY before it is trusted.
- Run a QEMU gate as `./run_qemu_X.sh`, never `sh`. One QEMU user at a time; the licence audit runs AFTER a sweep, never during.
- The plan's given code may contain slips (every prior task had at least one real one). The wire bytes and existing behaviour are the spec; the fallible parts are newly-written bodies and expected test values. Verify with a diagnostic, fix provable errors, note them; STOP only if a fix is genuinely ambiguous.

---

## Task 1: L2cap credit-leak instrument + host test

**Files:**
- Modify: `~/Development/M2Radio/bt/L2cap.h`
- Modify: `~/Development/M2Radio/bt/L2cap.cpp`
- Test: `~/Development/M2Radio/bt/test/l2cap_test.cpp`

**Context:** Read `bt/L2cap.h`/`.cpp` (the credit path: `begin`, `send`, `service`'s credit-paced write loop, `onEvent`'s 0x13 NCP handler, and the piece-2 `creditsMin`/`resetCreditsMin`). The instrument adds four cumulative counters and a millisecond-clock hook used only to measure the zero-with-work stretch. It changes no wire byte.

- [ ] **Step 1: Write the failing tests**

Add three scenarios to `bt/test/l2cap_test.cpp` before the final `printf`. They use the existing `CapIo`/`l2()`/`CHECK` scaffolding. `CapIo` already has a `now` field (used as the clock).

```cpp
    {   // C1. Normal flow: N packets sent, all N credits returned -> sent==returned==N, credits back to max,
        //     credmin correct, no starve, no clamp.
        CapIo io; L2cap l(io); l.begin(0x0001, 4); l.resetCreditStats();
        for (int i = 0; i < 4; i++) { const uint8_t d[4] = {0,1,2,3}; l.send(0x0040, d, 4); }
        l.tickClock(io.now); l.service();                                  // 4 sent -> credits 0
        CHECK(l.pktsSent() == 4 && l.credits() == 0 && l.creditsMin() == 0);
        uint8_t ncp[] = { 0x01, 0x01, 0x00, 0x04, 0x00 }; l.onEvent(0x13, ncp, sizeof ncp);  // return all 4
        CHECK(l.creditsReturned() == 4 && l.credits() == 4);
        CHECK(l.pktsSent() - l.creditsReturned() == 0);                    // outstanding == 0
        CHECK(l.clampHits() == 0);
    }
    {   // C2. Withheld NCP (the load-bearing negative): a credit that never comes back keeps the pool down,
        //     and once the pool sits at 0 with work pending the starve fingerprint appears and grows with the clock.
        CapIo io; L2cap l(io); l.begin(0x0001, 2); l.resetCreditStats();
        const uint8_t d[4] = {0,1,2,3};
        io.now = 1000; l.send(0x0040, d, 4); l.send(0x0040, d, 4); l.tickClock(io.now); l.service();   // 2 sent -> credits 0
        CHECK(l.credits() == 0 && l.pktsSent() == 2);
        uint8_t ncp1[] = { 0x01, 0x01, 0x00, 0x01, 0x00 }; l.onEvent(0x13, ncp1, sizeof ncp1);          // return only ONE
        CHECK(l.creditsReturned() == 1 && l.credits() == 1 && l.pktsSent() - l.creditsReturned() == 1); // one outstanding (the withheld one)
        // keep the TXQ fed and the second NCP WITHHELD: credits drain to 0 and stay there across ticks
        l.send(0x0040, d, 4); l.tickClock(io.now); l.service();                                         // credits 1 -> 0, TXQ still has work
        CHECK(l.credits() == 0);
        io.now = 1300; l.tickClock(io.now); l.service();                                                // 300 ms later, still 0-with-work
        CHECK(l.starves() >= 1 && l.starveMaxMs() >= 300);
    }
    {   // C3. Over-return (clamp): the controller returns more credits than were outstanding -> credits caps at
        //     maxCredits and clampHits records the discard (a double-count -- the opposite failure).
        CapIo io; L2cap l(io); l.begin(0x0001, 3); l.resetCreditStats();
        const uint8_t d[4] = {0,1,2,3};
        l.send(0x0040, d, 4); l.tickClock(io.now); l.service();            // 1 sent -> credits 2, one outstanding
        uint8_t ncp[] = { 0x01, 0x01, 0x00, 0x05, 0x00 }; l.onEvent(0x13, ncp, sizeof ncp);  // return FIVE (only 1 outstanding)
        CHECK(l.credits() == 3 && l.clampHits() >= 1);                     // capped at maxCredits=3, clamp recorded
    }
```

- [ ] **Step 2: Run to verify they fail to compile** — `cd ~/Development/M2Radio && ./bt/test/run.sh 2>&1 | head -20` → `pktsSent`/`creditsReturned`/`starves`/`starveMaxMs`/`clampHits`/`tickClock`/`resetCreditStats` are not members.

- [ ] **Step 3: Add the declarations to `bt/L2cap.h`**

In `public:`, after the piece-2 `creditsMin()`/`resetCreditsMin()`:
```cpp
    // Credit-leak instrument (NEW-34 piece 4).  pktsSent == ACL packets written (one per credit consumed);
    // creditsReturned == credits summed from Number_Of_Completed_Packets; by construction
    // credits == maxCredits - (pktsSent - creditsReturned), so pktsSent-creditsReturned is the outstanding count.
    // starves counts entries into "credits==0 with the TXQ non-empty" and starveMaxMs is the longest such stretch
    // (a lost NCP never recovers -> starveMaxMs grows without bound, the host-visible fingerprint vs backpressure).
    // clampHits counts when the NCP handler discards credit above maxCredits (a controller double-count).
    // tickClock(nowMs) supplies the millisecond reference for starveMaxMs; call it once per service() pass.
    uint32_t pktsSent()        const { return m_pktsSent; }
    uint32_t creditsReturned() const { return m_creditsReturned; }
    uint32_t starves()         const { return m_starves; }
    uint32_t starveMaxMs()     const { return m_starveMaxMs; }
    uint32_t clampHits()       const { return m_clampHits; }
    void     tickClock(uint32_t nowMs) { m_nowMs = nowMs; }
    void     resetCreditStats() { m_creditsMin = m_credits; m_pktsSent = 0; m_creditsReturned = 0;
                                  m_starves = 0; m_starveMaxMs = 0; m_clampHits = 0; m_starveSince = 0; m_starving = false; }
```
In `private:`, beside the piece-2 `m_creditsMin`:
```cpp
    uint32_t m_pktsSent = 0, m_creditsReturned = 0, m_starves = 0, m_starveMaxMs = 0, m_clampHits = 0;
    uint32_t m_nowMs = 0, m_starveSince = 0; bool m_starving = false;
```

- [ ] **Step 4: Implement in `bt/L2cap.cpp`**

In the credit-paced write loop in `service()`, count each packet written — after the existing `m_credits--;`/`creditsMin` update, add:
```cpp
        m_pktsSent++;
```
At the END of `service()` (after the write loop), evaluate the starve fingerprint (credits exhausted while work remains):
```cpp
    // starve fingerprint: credits==0 with the TXQ non-empty.  Enter -> count + mark the start; while starving,
    // extend starveMaxMs by how long we have been at zero-with-work; exit when credit returns or the TXQ empties.
    bool starving = (m_credits == 0 && m_txCount > 0);
    if (starving) {
        if (!m_starving) { m_starving = true; m_starveSince = m_nowMs; m_starves++; }
        uint32_t dur = m_nowMs - m_starveSince;
        if (dur > m_starveMaxMs) m_starveMaxMs = dur;
    } else {
        m_starving = false;
    }
```
In `onEvent()`'s 0x13 handler, count returned credits and the clamp. Replace the credit-add line:
```cpp
        if (h == m_handle) { uint32_t v = (uint32_t)m_credits + c; m_credits = v > m_maxCredits ? m_maxCredits : (uint8_t)v; }
```
with:
```cpp
        if (h == m_handle) { m_creditsReturned += c; uint32_t v = (uint32_t)m_credits + c;
            if (v > m_maxCredits) { m_clampHits++; m_credits = m_maxCredits; } else m_credits = (uint8_t)v; }
```
(The `creditsMin` guard line below it stays.) Leave `begin()` to init the new members via their in-class initialisers; do NOT reset them in `begin()` (the app calls `resetCreditStats()` once streaming starts, same as `resetCreditsMin()`).

> **Note on `starveMaxMs` accuracy:** it is only as fine-grained as how often `tickClock()`+`service()` run. bt_tone_test's `poll()`/loop runs every pass (sub-ms), so on silicon the resolution is fine; the host test drives `io.now` explicitly. This is a diagnostic, not a hard real-time measure — coarse resolution only *under*-reports a stall, never invents one.

- [ ] **Step 5: Run the suite** — `./bt/test/run.sh 2>&1 | tail -14` → `l2cap_test: N checks, 0 failures`, `BT-HOST-TESTS: PASS`, every other suite still 0 failures.

- [ ] **Step 6: Mutation-check** (scratch copy under the scratchpad `/private/tmp/claude-501/-Users-nicholasnewdigate-Development-rt1170-evkb/8e94b9bd-b541-41da-87f4-0f7c76be8fca/scratchpad`, never the working tree): (a) drop `m_pktsSent++` → C1 `pktsSent==4` fails; (b) drop the starve block → C2 `starves>=1 && starveMaxMs>=300` fails; (c) drop the `m_clampHits++` → C3 `clampHits>=1` fails; (d) don't accumulate `m_creditsReturned` → C1 `creditsReturned==4` fails. Compile+run just l2cap_test for each; confirm the named failure; discard the copy.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/M2Radio && git commit -m "bt: L2cap credit-leak instrument -- sent/returned/starve/clamp counters (NEW-34 piece 4)

Beside the piece-2 creditsMin(): pktsSent/creditsReturned (outstanding == their difference), starves + starveMaxMs (the zero-with-work stretch that separates a lost NCP -- never recovers -- from RF backpressure), and clampHits (a controller double-count). tickClock(now) supplies the ms reference; changes no wire byte. l2cap_test pins normal flow, a withheld NCP (the leak fingerprint) and over-return, each RED against a mutant.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>" -- bt/L2cap.h bt/L2cap.cpp bt/test/l2cap_test.cpp && git log --oneline -1
```

---

## Task 2: bt_tone_test heartbeat fields + service() clock tick

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/bt_tone_test.cpp`

**Context:** Read bt_tone_test's `loop()` — the `session.tick(millis()); src.service(); btout.poll();` line and the per-second heartbeat that already prints `credmin` on the `bt_hci` line (piece 2). The instrument needs (a) `src.l2().tickClock(millis())` each pass so `starveMaxMs` has a clock, (b) `src.l2().resetCreditStats()` once when streaming begins, (c) the five new fields on the heartbeat.

- [ ] **Step 1: Tick the L2cap clock every pass.** In `loop()`, immediately after `src.service();`, add:
```cpp
    src.l2().tickClock(millis());     // NEW-34 piece 4: ms reference for the credit-starve fingerprint
```

- [ ] **Step 2: Reset the credit stats when streaming starts.** In `onStreamCb`, in the `streaming` (true) branch after `btout.begin(src);`, add:
```cpp
        src.l2().resetCreditStats();  // zero the credit-leak counters at each STREAMING entry
```

- [ ] **Step 3: Add the five fields to the heartbeat.** Extend the `bt_hci` line (or add a `bt_cred` line) in the once-a-second block. Beside the existing `credmin=`:
```cpp
        CONSOLE.print(" sent="); CONSOLE.print(src.l2().pktsSent());
        CONSOLE.print(" returned="); CONSOLE.print(src.l2().creditsReturned());
        CONSOLE.print(" starves="); CONSOLE.print(src.l2().starves());
        CONSOLE.print(" starve_max_ms="); CONSOLE.print(src.l2().starveMaxMs());
        CONSOLE.print(" clamp="); CONSOLE.println(src.l2().clampHits());
```
(Match the existing print style; end the line with `println`. If `credmin=` was the line's terminator, change it to `print` and let `clamp=` terminate.)

- [ ] **Step 4: Build + run the card-absent gate**
```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake >/tmp/c.log 2>&1 && cmake --build build >/tmp/b.log 2>&1 && ./run_qemu.sh 2>&1 | tail -6
```
Expected: `PASS`. The card-absent path never streams, so the new fields print zeros on the vacuous heartbeat; if `run_qemu.sh` greps the `bt_hci`/heartbeat line exactly and the new fields break the match, update that assertion to the new format (it currently matches a prefix, so it should be unaffected — verify).

- [ ] **Step 5: Commit**
```bash
cd ~/Development/rt1170/evkb && git commit -m "audio: bt_tone_test logs the credit-leak instrument on the heartbeat (NEW-34 piece 4)

tickClock(millis()) each loop pass; resetCreditStats() at each STREAMING entry; sent/returned/starves/starve_max_ms/clamp on the per-second heartbeat -- the soak record. Card-absent gate unaffected (fields print zeros on the vacuous heartbeat).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>" -- examples/audio/bt_tone_test/bt_tone_test.cpp && git log --oneline -1
```

---

## Task 3: acid_box heartbeat fields (secondary witness)

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/display/acid_box/acid_box.cpp`

**Context:** Read acid_box's `M2_BT_OUT` `bt_hb` block and its loop (`session.tick(millis()); src.service(); if (s_btBegun) btout.poll();`). Same three edits as Task 2, guarded by `#if defined(M2_BT_OUT)`. The default (BT-OFF) image is untouched.

- [ ] **Step 1:** After `src.service();` in the loop's `M2_BT_OUT` block, add `src.l2().tickClock(millis());`.
- [ ] **Step 2:** In `onStreamCb`'s streaming branch after `btout.begin(src);`, add `src.l2().resetCreditStats();`.
- [ ] **Step 3:** Add the five fields to the `bt_hb` block (mirroring Task 2).
- [ ] **Step 4: Build both images**
```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake >/tmp/ab.log 2>&1 && cmake --build build >/tmp/abb.log 2>&1 && echo "default OK" && rm -rf build-bt && cmake -B build-bt -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DM2_BT_OUT=ON -DM2_BT_TARGET_NAME=Shokz >/tmp/abt.log 2>&1 && cmake --build build-bt >/tmp/abtb.log 2>&1 && echo "BT OK" && grep -i "overflowed\|will not fit" /tmp/abtb.log || echo "no ITCM overflow"
```
Expected: `default OK`, `BT OK`, `no ITCM overflow`. The default build does not compile the `M2_BT_OUT` code, so the acid_box gate golden is unaffected.
- [ ] **Step 5: Commit** (`examples/display/acid_box/acid_box.cpp`), message analogous to Task 2.

---

## Task 4: Close-out — pin, sweep, audit, docs

**Files:**
- Modify: `~/Development/rt1170/evkb/evkb.cmake` (M2Radio pin), `CLAUDE.md`, `docs/superpowers/specs/2026-09-06-bt-credit-leak-investigation-design.md` (status), memory.

- [ ] **Step 1: Push M2Radio, bump the pin.**
```bash
cd ~/Development/M2Radio && git push origin master && git rev-parse HEAD
```
Then in `evkb.cmake` replace the M2Radio SHA with the new one and append a piece-4 note to the trailing comment (credit-leak instrument, additive, no new gate).

- [ ] **Step 2: Fresh-user verify.** Build bt_tone_test with `-DEVKB_FORCE_FETCH=ON` (clones M2Radio at the new pin), confirm the configure log shows `fetching … @ <new sha>`, and run the `[media]` gate against the fetched ELF (swap it into `build-media` with `GATE_VACUITY=1`, the piece-2 pattern) — its heartbeat now carries the new fields; confirm it still PASSES.

- [ ] **Step 3: Rebuild the bt-linking gate dirs fresh + sweep.** The instrument is additive but the heartbeat changed, so rebuild bt_tone_test `build`/`build-media`/`build-lifecycle` and m2_hci_probe `build-avdtp`/`build-reconnect` (the piece-2 freshness set) against the new M2Radio, then read `docs/KNOWN-BROKEN-GATES.md` and run the sweep:
```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh -l | tail -1     # expect 131
./tools/run-all-qemu-gates.sh 2>&1 | tail -6                                    # expect gates: 131 passed
```
No new gate — the instrument adds none. Disposition any red per the standing exceptions (re-run `cm4_audio_test` / load-sensitive gates idle). If a heartbeat-greppping gate moved, recapture its transcript and note it.

- [ ] **Step 4: Licence audit** (AFTER the sweep): `LICENSE_AUDIT_EVKB="$PWD" ./tools/license-audit.sh 2>&1 | tail -3` → `LICENSE-AUDIT: PASS`.

- [ ] **Step 5: Docs + memory.** CLAUDE.md: a short note that piece 4 added the credit-leak instrument (no gate; sweep unchanged at 131) and that the soak is a pending bench claim. Spec status → IMPLEMENTED (instrument), soak PENDING. Update the `new34-bt-reconnect` memory + MEMORY.md index (piece 4 instrument done; soak + escalation pending).

- [ ] **Step 6: Commit** (`evkb.cmake CLAUDE.md docs/superpowers/specs/2026-09-06-bt-credit-leak-investigation-design.md`), then finish per `superpowers:finishing-a-development-branch` (present the push decision; the work is on master like pieces 1–2).

---

## Task 5: Silicon soak (user, on hardware)

**Files (evidence):** `examples/audio/bt_tone_test/transcript_hw_evkb.txt`, `examples/display/acid_box/transcript_hw_evkb_bt.txt`.

The soak is the piece's actual question and is bench work. Flash bt_tone_test (tone → ESP32 `EVKB-SINK`), free-run, read the heartbeat with `tools/rt1170-console.py`.

- [ ] **A — good RF, 30+ min.** Close range. Record the heartbeat once a second. Verdict CLEAN: `credmin` ≥ a small floor, `starve_max_ms` in the low hundreds, `drops` flat, `sent − returned` ≤ `maxCredits`, `clamp` == 0.
- [ ] **B — marginal RF, 30+ min.** Distance or an attenuator (the provocation). Same verdict. A LEAK shows here first: `credmin` pinned at 0, `starve_max_ms` growing, `drops` climbing.
- [ ] **C — acid_box secondary witness** (`-DM2_BT_OUT=ON` → ESP32): the same fields under real synth + UI load, one 30-min run.
- [ ] **On CLEAN:** record the negative finding in the transcripts + CLAUDE.md/memory ("no credit leak over 30 min on either RF arm; batching suffices; lost-NCP hypothesis refuted for these conditions"); piece 4 closes.
- [ ] **On LEAK:** escalate per spec §6 — add the ESP32 received-packet counter (`tools/esp32-a2dp-sink`), reconcile against the host `returned`; if sink-received > host-returned, implement `Write_Automatic_Flush_Timeout` (0x0C28) + re-verify. That escalation is its own follow-up, not part of this plan's software tasks.

---

## Self-review (completed by the plan author)

- **Spec coverage:** §2 instrument → Task 1; §3 health line + soak → Tasks 2/3 + Task 5; §4 host test → Task 1 (C1–C3); §6 outcomes/escalation → Task 5; §7 non-goals honoured (no flush-timeout, no ESP32 counter, no accounting change — all contingent). Covered.
- **Type consistency:** the five accessors (`pktsSent`/`creditsReturned`/`starves`/`starveMaxMs`/`clampHits`) + `tickClock`/`resetCreditStats` are named identically in the header, the .cpp, the tests (Task 1) and both examples (Tasks 2/3).
- **Invariant checked:** `credits == maxCredits − (pktsSent − creditsReturned)` holds because both sides update `m_credits` and the counters in the same events; the clamp is the one place they can diverge, and `clampHits` records exactly that.
- **No new gate:** the sweep stays 131; the instrument's correctness is the host test, its reading the silicon soak — no placeholder QEMU assertion invented.
- **No placeholders:** every code step shows the code or the exact edit; every command has an expected result.
