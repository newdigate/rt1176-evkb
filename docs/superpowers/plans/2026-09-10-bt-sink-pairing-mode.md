# A2DP Sink Pairing Mode (NEW-46) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the A2DP sink a runtime pairing mode — 2-minute discoverable windows at boot and after every link drop or failed attempt, plus console `pair` / `forget` / `status` — with every window re-issuing PREPARE so NEW-43's stranded-SSP state heals without a reboot.

**Architecture:** The policy lives in `BtSinkSession` (M2Radio) as a deadline beside `LISTENING`, host-tested on its existing fake-controller rig; the sketch owns the command reader (a `serialEvent1()` override), the LED and the prints. Gate 139 is extended in place: the console becomes a `socket + logfile=` chardev in real runs (a file under `GATE_VACUITY`), a Python driver sends the commands, and the fake peer's `source` phase gains a drop and an unbonded re-page.

**Tech Stack:** C++11 (host tests under clang), ARM GCC 10, Python 3 (peer + console driver), POSIX sh gates, LinkServer + iPhone.

**Spec:** `docs/superpowers/specs/2026-09-10-bt-sink-pairing-mode-design.md` (approved 2026-09-10). Five refinements found while writing this plan are recorded in its §4/§5 as "corrected during planning" and are the authority where the two differ.

---

## Conventions

- `$E` = `/Users/nicholasnewdigate/Development/rt1170/evkb`, `$S` = `$E/examples/audio/bt_sink_test`, `$M` = `$HOME/Development/M2Radio`. **Absolute paths in every command** — the harness resets the shell cwd between calls.
- Gates run as `./run_qemu.sh`, never `sh run_qemu.sh`.
- Every commit message ends with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- **RED first.** Where a step says "verify it fails", the expected failure text is given; if it fails differently, stop and understand why before implementing.
- **For every new check, ask: would deleting the line it pins leave the suite green?** That question found nineteen defects in the NEW-42 plan's own test code.
- The sweep, the audit and the vacuity suite never run concurrently with each other or anything else.

Baseline (master `9c91643`): sink gate PASS; `btsinksession_test: 149 checks`; sweep 138/1/0 with the one red a documented load-sensitivity member; vacuity 46/46.

---

### Task 0: Branch and baseline

- [ ] **Step 1: Branch in place** (the 139 built gate images live here; a worktree has none)

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git status --short && git checkout -b new-46-pairing-mode && git log --oneline -1
```
Expected: clean status, then `Switched to a new branch 'new-46-pairing-mode'`.

- [ ] **Step 2: Confirm both host suites and the gate are green**

```bash
cd /Users/nicholasnewdigate/Development/M2Radio/bt/test && ./run.sh 2>&1 | grep -E "btsinksession_test:|BT-HOST-TESTS"
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./run_qemu.sh 2>&1 | tail -1 | cut -c1-40
```
Expected: `btsinksession_test: 149 checks, 0 failures`, `BT-HOST-TESTS: PASS`, `PASS: A2DP SINK -- a paging source is accepted`.

---

### Task 1: `BtSinkSession` — the pairing window (M2Radio)

**Files:**
- Modify: `~/Development/M2Radio/bt/BtSinkSession.h`
- Modify: `~/Development/M2Radio/bt/BtSinkSession.cpp`
- Modify: `~/Development/M2Radio/bt/test/btsinksession_test.cpp`
- Modify: `evkb.cmake:119` (pin)

- [ ] **Step 1: Write the failing tests**

In `btsinksession_test.cpp`, immediately before the final `printf("btsinksession_test: ...` line, add five cases. `PHONE`, `KEY`, `Rig`, `inboundToStreaming`, `failPairing`, `disconnectionComplete`, `advanceMs`, `runUntil`, `io.count()`, `io.lastParamsOf()` and `OP_SCAN` already exist in the file; `OP_SSP` does not — add `static const uint16_t OP_SSP = 0x0C56;   // Write_Simple_Pairing_Mode: PREPARE writes it once; every pairing window writes it again` beside `OP_SCAN`.

```cpp
    {   // P1. THE BOOT WINDOW (NEW-46).  begin() opens a PAIR_BOOT window: a BONDED sink is discoverable (0x03)
        //     while it is open and stops advertising (0x02) when it expires on the clock.  The bond is a
        //     synthetic upsert here ON PURPOSE -- the case is "bonded AND windowed", which no live pairing
        //     produces on a fresh session.  Both halves are load-bearing: a window that never closes keeps
        //     the first half green and reddens the second; no window at all reddens the first.
        Rig r; r.session.begin(&r.bonds, 8, 0); r.answerPrepare();
        CHECK(r.session.pairingOpen()); CHECK(r.session.pairingReason() == BtSinkSession::PAIR_BOOT);
        CHECK(r.session.pairingRemainingMs(r.io.now) > 110000 && r.session.pairingRemainingMs(r.io.now) <= 120000);
        Bond b{}; memcpy(b.bd, PHONE, 6); memcpy(b.key, KEY, 16); b.keyType = 4; r.bonds.upsert(b);
        r.tick(); r.tick();
        CHECK(r.bonds.count() == 1);
        CHECK(r.io.lastParamsOf(OP_SCAN) == std::vector<uint8_t>{ 0x03 });      // bonded, but the window keeps us discoverable
        r.advanceMs(119000);
        CHECK(r.session.pairingOpen());                                          // 1 s to go
        r.advanceMs(2000);
        CHECK(!r.session.pairingOpen()); CHECK(r.session.pairingEnd() == BtSinkSession::PAIR_END_TIMEOUT);
        CHECK(r.session.pairingRemainingMs(r.io.now) == 0);
        CHECK(r.runUntil([&] { return r.io.lastParamsOf(OP_SCAN) == std::vector<uint8_t>{ 0x02 }; }, 200));   // expired: connectable only
        CHECK(r.io.count(OP_SSP) == 1);                                          // the boot window did NOT double begin()'s PREPARE
    }
    {   // P2. A DROP WINDOW ON LOSS, closed as PAIRED by the next link, and PREPARE RE-ISSUED once per window.
        //     The stream is real (Q1's flow); the window's PREPARE is counted at the fake controller as a second
        //     Write_Simple_Pairing_Mode -- which is the NEW-43 heal, so it is what this case exists to pin.
        Rig r; r.session.begin(&r.bonds, 8, 0); r.answerPrepare();
        r.inboundToStreaming(0x0060);
        CHECK(!r.session.pairingOpen()); CHECK(r.session.pairingEnd() == BtSinkSession::PAIR_END_PAIRED);   // the boot window closed on STREAMING
        CHECK(r.io.count(OP_SSP) == 1);
        r.disconnectionComplete(0x13);
        CHECK(r.runUntil([&] { return r.session.state() == BtSinkSession::LISTENING; }, 500));
        CHECK(r.session.pairingOpen()); CHECK(r.session.pairingReason() == BtSinkSession::PAIR_DROP);
        CHECK(r.runUntil([&] { return r.io.count(OP_SSP) == 2; }, 500));         // PREPARE re-issued for the window
        CHECK(r.bonds.count() == 1);
        CHECK(r.runUntil([&] { return r.io.lastParamsOf(OP_SCAN) == std::vector<uint8_t>{ 0x03 }; }, 500));   // bonded AND discoverable
        r.advanceMs(121000);
        CHECK(!r.session.pairingOpen());
        CHECK(r.runUntil([&] { return r.io.lastParamsOf(OP_SCAN) == std::vector<uint8_t>{ 0x02 }; }, 200));
        CHECK(r.io.count(OP_SSP) == 2);                                          // expiry writes nothing
    }
    {   // P3. A DROP WINDOW ON A FAILED ATTEMPT -- the NEW-43 shape.  A page that fails to pair returns the
        //     session to LISTENING from CONNECTING, not from STREAMING, and it is precisely that path that must
        //     re-issue PREPARE: BtLink's legacy-PIN fallback has just written SSP_Mode=0 on the controller.
        //     RED with the rejects branch not opening a window: pairingOpen() false, OP_SSP count stays 1.
        Rig r; r.session.begin(&r.bonds, 8, 0); r.answerPrepare();
        r.advanceMs(121000); CHECK(!r.session.pairingOpen());                    // let the boot window lapse first
        r.incomingPage(PHONE);
        CHECK(r.runUntil([&] { return r.session.state() == BtSinkSession::CONNECTING; }, 1000));
        r.failPairing();
        CHECK(r.runUntil([&] { return r.session.state() == BtSinkSession::LISTENING; }, 5000));
        CHECK(r.session.stats().rejects == 1);
        CHECK(r.session.pairingOpen()); CHECK(r.session.pairingReason() == BtSinkSession::PAIR_DROP);
        CHECK(r.runUntil([&] { return r.io.count(OP_SSP) == 2; }, 500));
    }
    {   // P4. enterPairing(): EXTENDS an open window (deadline moves, reason becomes the caller's), is REFUSED
        //     while a link is up (no window, no PREPARE), and does not double a PREPARE already in flight.
        Rig r; r.session.begin(&r.bonds, 8, 0);
        CHECK(r.session.enterPairing(r.io.now, BtSinkSession::PAIR_CMD));       // boot PREPARE still in flight ...
        r.answerPrepare();
        CHECK(r.io.count(OP_SSP) == 1);                                          // ... so startPrepare() declined: ONE write, not two
        CHECK(r.session.pairingReason() == BtSinkSession::PAIR_CMD);
        r.advanceMs(60000);
        uint32_t before = r.session.pairingRemainingMs(r.io.now);
        CHECK(before > 55000 && before <= 60000);
        CHECK(r.session.enterPairing(r.io.now, BtSinkSession::PAIR_CMD));
        CHECK(r.session.pairingRemainingMs(r.io.now) > 119000);                  // extended to a full window
        CHECK(r.runUntil([&] { return r.io.count(OP_SSP) == 2; }, 500));         // idle now: PREPARE re-issued
        r.inboundToStreaming(0x0060);
        CHECK(!r.session.enterPairing(r.io.now, BtSinkSession::PAIR_CMD));      // link up: refused
        CHECK(!r.session.pairingOpen()); CHECK(r.io.count(OP_SSP) == 2);
        CHECK(!r.session.canPair());
    }
    {   // P5. setPairingWindowMs(0) turns the AUTOMATIC windows off and leaves the commanded one working at
        //     the default length.  RED with the guard removed: the boot window opens anyway.
        Rig r; r.session.setPairingWindowMs(0); r.session.begin(&r.bonds, 8, 0); r.answerPrepare();
        CHECK(!r.session.pairingOpen());
        r.inboundToStreaming(0x0060); r.disconnectionComplete(0x13);
        CHECK(r.runUntil([&] { return r.session.state() == BtSinkSession::LISTENING; }, 500));
        r.tick(); r.tick();
        CHECK(!r.session.pairingOpen()); CHECK(r.io.count(OP_SSP) == 1);
        CHECK(r.session.enterPairing(r.io.now, BtSinkSession::PAIR_CMD));
        CHECK(r.session.pairingOpen());
        CHECK(r.session.pairingRemainingMs(r.io.now) > 119000);                  // the default 120 s, not 0
    }
```

…plus **P6**, added by the second Task 1 review (see the note at the end of this task): Q7's wire sequence with `disconnect()` called from OUTSIDE the attempt callback, pinning that `pairingEnd()` reads the same either way — that the reason a window ended describes the wire and not the app's callback timing.

- [ ] **Step 2: Run to verify it fails (RED)**

```bash
cd /Users/nicholasnewdigate/Development/M2Radio/bt/test && ./run.sh 2>&1 | grep -m1 "pairingOpen\|PAIR_BOOT"
```
Expected: a compile error naming `pairingOpen` or `PAIR_BOOT` as no member of `BtSinkSession`.

- [ ] **Step 3: The header**

In `BtSinkSession.h`, in `public:` after `void setAlwaysDiscoverable(bool on) { m_alwaysDisc = on; }`, add:

```cpp
    // --- PAIRING MODE (NEW-46) -----------------------------------------------------------------------------
    // A bonded sink is not discoverable by design (a phone that knows us PAGES).  The consequence the bench
    // recorded twice is that a phone which has FORGOTTEN the sink has no way back except a reflash -- and
    // NEW-43 sharpened it: one failed SSP leaves the controller in legacy-PIN mode until PREPARE runs again.
    // A pairing WINDOW answers both, the way a speaker does: discoverable for a while after power-on and after
    // any attempt ends (loss, clean close, OR a failed pairing -- the NEW-43 path is a failed attempt), and on
    // demand.  Every window re-issues PREPARE, so opening one guarantees SSP is on.
    // The window is a DEADLINE beside LISTENING, not a State: MANUAL and LISTENING both compose with it and
    // every `m_state == LISTENING` test in tick() stays as it is.  A window is never open while a link is up --
    // scans are already off then -- so enterPairing() refuses rather than queues.
    enum PairingReason : uint8_t { PAIR_NONE, PAIR_BOOT, PAIR_DROP, PAIR_CMD };
    enum PairingEnd    : uint8_t { PAIR_END_NONE, PAIR_END_TIMEOUT, PAIR_END_PAIRED, PAIR_END_CANCELLED };   // CANCELLED: closed by the app's disconnect(), not by pairing or the clock
    static const uint32_t PAIR_DEFAULT_MS = 120000;
    void          setPairingWindowMs(uint32_t ms) { m_pairMs = ms; }   // auto-window length; 0 = no auto-windows (a commanded window is then PAIR_DEFAULT_MS)
    bool          canPair() const;                                     // LISTENING with no link up -- the one condition every trigger needs
    bool          enterPairing(uint32_t now, PairingReason r = PAIR_CMD);   // open, or extend to now + window; false = refused (see canPair)
    bool          pairingOpen() const { return m_pairOpen; }           // as of the last tick(now): that is where expiry is evaluated, so the poll, the scan line and the heartbeat agree
    uint32_t      pairingRemainingMs(uint32_t now) const { return m_pairOpen && (int32_t)(m_pairUntil - now) > 0 ? m_pairUntil - now : 0; }
    PairingReason pairingReason() const { return m_pairOpen ? m_pairReason : PAIR_NONE; }
    PairingEnd    pairingEnd() const { return m_pairEnd; }             // why the LAST window closed; the sketch prints it on the falling edge
```

and in `private:`, after `bool m_alwaysDisc = false;`, add:

```cpp
    uint32_t m_pairMs = PAIR_DEFAULT_MS, m_pairUntil = 0; bool m_pairOpen = false;
    PairingReason m_pairReason = PAIR_NONE; PairingEnd m_pairEnd = PAIR_END_NONE;
    bool openWindow(uint32_t now, PairingReason r);                    // the ONE path every trigger takes
```

- [ ] **Step 4: The implementation**

In `BtSinkSession.cpp`:

Replace `begin()` with:

```cpp
void BtSinkSession::begin(BondTable *bonds, uint8_t aclNum, uint32_t now) {
    m_bonds = bonds; m_sink.setBonds(bonds);      // the session and the sink share one table (the inbound accept reads it)
    m_sink.begin(now, aclNum);                    // resets the attempt machine and runs PREPARE once per session
    m_stats = Stats{}; m_state = LISTENING;
    m_pairOpen = false; m_pairEnd = PAIR_END_NONE;
    openWindow(now, PAIR_BOOT);                   // begin()'s own PREPARE is in flight, so this one's startPrepare() declines: no double
}
```

After `begin()`, add:

```cpp
bool BtSinkSession::canPair() const {
    bool linkUp = m_sink.link().linkState() == BtLink::LINK_UP || m_sink.link().linkState() == BtLink::LINK_SECURE;
    return m_state == LISTENING && !linkUp;
}
bool BtSinkSession::openWindow(uint32_t now, PairingReason r) {
    if (r != PAIR_CMD && m_pairMs == 0) return false;               // automatic windows switched off
    if (!canPair()) return false;
    m_pairUntil = now + (m_pairMs ? m_pairMs : PAIR_DEFAULT_MS);
    m_pairOpen = true; m_pairReason = r; m_pairEnd = PAIR_END_NONE;
    // Every window guarantees SSP is on.  startPrepare() returns false when an op is already in flight (the
    // boot PREPARE, or a page being accepted), which is exactly the no-double we want; tickPrepare() writes
    // only idempotent things and never touches the scan bookkeeping, so this needs no begin() -- and MUST NOT
    // use one: BtLink::begin() resets that bookkeeping to "off" and would desync host and controller.
    m_sink.link().startPrepare();
    return true;
}
bool BtSinkSession::enterPairing(uint32_t now, PairingReason r) { return openWindow(now, r); }
```

In `tick()`: the `CONNECTING` failure branch — after `m_state = LISTENING;` (the line followed by the attempt callback) insert `openWindow(now, PAIR_DROP);` so the block reads:

```cpp
        m_state = LISTENING;
        openWindow(now, PAIR_DROP);                                         // a FAILED attempt is the NEW-43 path: re-issue PREPARE
        if (m_attemptCb) m_attemptCb(m_attemptCtx, m_sink.result(), m_sink.link().pairedBy());
```

The `STREAMING` loss branch — after `m_state = LISTENING;` insert `openWindow(now, PAIR_DROP);` before the stream callback. The clean-close branch likewise, after its `m_state = LISTENING;`.

Then replace the scanning block at the end of `tick()` with:

```cpp
    // Scanning: page scan (connectable) whenever we are LISTENING with no link up, and inquiry scan
    // (discoverable) on top of it until a bond exists -- a phone that already knows us pages, it does not
    // search -- OR while a pairing window is open.  Computed AFTER the state machine so it reflects THIS
    // tick's state, exactly as BtSession does.
    bool linkUp = m_sink.link().linkState() == BtLink::LINK_UP || m_sink.link().linkState() == BtLink::LINK_SECURE;
    bool listening = (m_state == LISTENING && !linkUp);
    // The window's edges are evaluated HERE, once per tick, so pairingOpen(), the scan line below and the
    // heartbeat all read the same answer for the same pass.  A link coming up closes it as PAIRED; the clock
    // closes it as TIMEOUT.  (A window opened this tick by a drop branch above has now + window as its
    // deadline and cannot expire on the same pass.)
    if (m_pairOpen) {
        if (m_state == STREAMING)                        { m_pairOpen = false; m_pairEnd = PAIR_END_PAIRED; }
        else if ((int32_t)(now - m_pairUntil) >= 0)      { m_pairOpen = false; m_pairEnd = PAIR_END_TIMEOUT; }
    }
    m_sink.link().wantPageScan(listening);
    m_sink.link().wantDiscoverable(listening && (m_alwaysDisc || !m_bonds || m_bonds->count() == 0 || m_pairOpen));
```

- [ ] **Step 5: Run to verify it passes (GREEN)**

```bash
cd /Users/nicholasnewdigate/Development/M2Radio/bt/test && ./run.sh 2>&1 | grep -E "btsinksession_test:|BT-HOST-TESTS|FAIL"
```
Expected: `btsinksession_test: N checks, 0 failures` (N ≈ 200) and `BT-HOST-TESTS: PASS`, all fourteen binaries green.

- [ ] **Step 6: Demonstrate RED by mutation, then restore each**

(a) In `tick()` delete the `else if ((int32_t)(now - m_pairUntil) >= 0)` line → P1's `!pairingOpen()` after 121 s and the `0x02` check fail by name; P1's first half stays green. (b) Delete `openWindow(now, PAIR_DROP);` from the CONNECTING failure branch only → P3 fails (`pairingOpen()` false, `OP_SSP == 2` times out) and P2 stays green — the two drop paths are pinned independently. (c) In `openWindow`, delete `m_sink.link().startPrepare();` → P2/P3/P4's `OP_SSP == 2` checks fail. (d) Delete the `r != PAIR_CMD && m_pairMs == 0` guard → P5's first `!pairingOpen()` fails. (e) Change the STREAMING close to `m_state == CONNECTING` → P2's `PAIR_END_PAIRED` check fails (the window closes too early, before P2 asserts it against STREAMING). Restore after each; the suite must read `0 failures`. **(e) is superseded by the second review below** — the close no longer lives at the end of `tick()`; the mutants that apply to the shipped code are (i)/(ii)/(iii) in that note.

- [ ] **Step 7: Commit, push, bump the pin**

```bash
cd /Users/nicholasnewdigate/Development/M2Radio && git add bt/BtSinkSession.h bt/BtSinkSession.cpp bt/test/btsinksession_test.cpp && git commit -q -F - <<'MSG' && git push origin HEAD:master 2>&1 | tail -1 && git rev-parse HEAD
bt/BtSinkSession: a pairing WINDOW -- discoverable for a while at boot, after every attempt ends, and on demand; every window re-issues PREPARE

A bonded sink is not discoverable by design; the bench recorded twice that a phone which has
forgotten it has no way back except a reflash, and NEW-43 sharpened it -- one failed SSP leaves
the controller in legacy-PIN mode until PREPARE runs again.  The window (default 120 s) opens
in begin(), on every return to LISTENING -- loss, clean close, AND a failed attempt, which is
the NEW-43 path -- and from enterPairing().  It is a deadline beside LISTENING, not a State.
Every opening calls link().startPrepare(), which declines when an op is in flight, so the boot
window cannot double begin()'s PREPARE; tickPrepare() writes only idempotent things and never
touches the scan bookkeeping, so no begin() is needed -- and none may be used, since
BtLink::begin() resets that bookkeeping and would desync host and controller.

btsinksession_test P1-P5: boot window opens and expires on the clock with a bonded sink
discoverable only while it is open; drop windows on loss AND on a failed attempt, each pinned
independently; PREPARE re-issued exactly once per window, counted at the fake controller;
enterPairing() extends, refuses with a link up, and does not double an in-flight PREPARE;
setPairingWindowMs(0) turns the automatic windows off and leaves the commanded one working.
Five mutants RED by name.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
MSG
```

Then in `$E/evkb.cmake` line 119 replace `1b88c1bd21817566bed915e5db751c1ab89f29a2` with the full new SHA and append to that line's comment: ` 2026-09-10 NEW-46: BtSinkSession pairing window (enterPairing/pairingOpen/canPair + PAIR_* enums) -- bt_sink_test's sketch calls them and will not BUILD against anything older.` Verify with `grep -c "$(git -C ~/Development/M2Radio rev-parse HEAD)" /Users/nicholasnewdigate/Development/rt1170/evkb/evkb.cmake` → `1`, then:

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git add evkb.cmake && git commit -q -m "build: bump M2Radio to $(git -C ~/Development/M2Radio rev-parse --short HEAD) (BtSinkSession pairing window -- NEW-46)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" && cd examples/audio/bt_sink_test && cmake --build build 2>&1 | tail -1 && ./run_qemu.sh 2>&1 | tail -1 | cut -c1-40
```
Expected: the sink still builds and the gate still passes (nothing the gate reads has changed; the sketch does not call the new API yet). Also confirm the pin note's claim will be true after Task 2 — it is not yet.

**Task 1 review corrections (2026-09-10).** `PairingEnd` gained `PAIR_END_CANCELLED` and `disconnect()` closes an open window with it, because both callbacks can call `disconnect()` with a window open and `tick()`'s close check saw neither shape: the attempt callback on the success edge runs BEFORE that check and has already left `STREAMING`, so the boot window stayed open through a `LINK_SECURE` teardown (Q7); the stream callback on the loss edge runs one line AFTER the drop branch opened its window (Q6) — either way the window rode through `MANUAL` with the scans off. `resume()` opens NO window (an app command, not the end of an attempt; Q4 pins it by the scan value and the PREPARE count). P3's comment overstated what `failPairing()` reaches — it refuses at `PR_AUTH1_STATUS`, before the legacy-PIN rung, so no mode-0 write happens there; the comment now says so and the case pins the re-issued parameter `0x01`. `BtLink.h`'s two "once per session" comments (`startPrepare()`, `setIdentity()`) were rewritten: PREPARE re-runs per window, so the borrowed name must outlive the whole session.

**Second Task 1 review (2026-09-10) — where the *paired* close belongs.** Answering the success edge with *cancelled* made the end reason a function of CALLBACK TIMING rather than of the wire: the same sequence (stranger paged, SSP completed, link `LINK_SECURE`, attempt `OK`) read *cancelled* with `disconnect()` inside the attempt callback and *paired* with it in `loop()` one tick later, so Task 2's `pairingEndName()` would print `pairing=off reason=cancelled` on a successful pairing. The *paired* close moved INTO the `CONNECTING` → `STREAMING` transition, beside `links++`/`accepts++` and AHEAD of `m_attemptCb`; `tick()`'s check keeps only the clock arm, the `STREAMING` arm being unreachable once nothing but `LISTENING` can open a window. `disconnect()`'s *cancelled* then means a window that never saw `STREAMING` — Q6's drop window, or a boot/commanded one torn down from `LISTENING`/`CONNECTING`. Q7's mid-teardown and post-MANUAL ends became `PAIR_END_PAIRED`, RED at `d906c08` (lines 360/363), the mid-teardown check still asserting `!pairingOpen()`. **New case P6** — Q7's sequence with `disconnect()` called from OUTSIDE the callback, asserting the identical `pairingEnd()` — is the timing-independence pin nothing had; it is green either way ALONE (that is the point: it discriminates only beside Q7) and its trailing checks re-pin the `disconnect()` guard. Mutants by name: (i) the close moved back after `m_attemptCb` → Q7 360/363; (ii) the close deleted → Q7 plus Q4/P2/P4/P6, nine checks; (iii) `disconnect()`'s guard deleted → P6 491 plus Q4/Q7. 267 checks, 0 failures. Comment sweep: `A2dpSink.h`'s `begin()` and `setIdentity()` carried the same stale "once per session" / "set it BEFORE begin()" claims (false for the sink — every window re-runs PREPARE), and `BtLink.h`'s `startPrepare()` said PREPARE runs "at begin()" when `BtLink::begin()` does not run it at all: the OWNER does. `BtSinkSession.h`'s own "a window is never open while a link is up" was measured false during `CONNECTING` (by design) and now states what the code implements. Task 2's `pairingEndName()` is unchanged — *cancelled* is still reachable.

**Third Task 1 review (2026-09-10) -- code quality.**  Nine findings, each measured by the reviewer before it
was written down.  The one that changes behaviour is `enterPairing()`'s parameter: it reached `openWindow()`
whose `r != PAIR_CMD` guard doubled as the automatic/commanded test, so `enterPairing(now, PAIR_NONE)` opened a
window that `pairingReason()` then reported as `PAIR_NONE` (Task 2 would print `pairing=off secs=120` with the
LED blinking) and `enterPairing(now, PAIR_DROP)` after `setPairingWindowMs(0)` was refused.  **Step 4's
`openWindow()`/`enterPairing()` bodies and Step 3's declaration are superseded**: `openWindow(now, r,
commanded)` takes the distinction as an argument of its own, `enterPairing(now, r = PAIR_CMD)` always passes
`commanded` and normalises `PAIR_NONE` to `PAIR_CMD`, and the three ad-hoc `m_pairOpen = false; m_pairEnd = X;`
sites become one guarded `closeWindow(PairingEnd)`.  **New case P7** pins the entry's contract (normalisation,
a labelled command not refused by the off switch, the default argument) and is kept out of P5 so that case
still owns the off switch alone; RED first, five failures by name plus a compile error for the default.
**P2 gained its second `pairingOpen()`**: `runUntil(state == CONNECTING)` returns on the tick that ASSIGNS
`CONNECTING` in the `LISTENING` branch, so `case CONNECTING:` has not run once and a mutant closing the window
at the top of it passed the whole suite -- the check after `peerAuthenticates()` reddens it by name, and with
that line deleted the same mutant is green.  The rest are comment defects corrected against measurement: an
inbound accept is NOT an op (`Accept_Connection_Request` is submitted straight from `BtLink::onEvent`, `m_op`
stays `NONE`, measured `op=0 busy=0 canPair=1` in the `Connection_Request` -> `Connection_Complete` gap); the
harm in `BtLink::begin()` is `m_op = NONE; m_cmdBusy = false` abandoning an in-flight op, NOT the scan
bookkeeping, which self-heals (measured: a `link().begin(now)` in `openWindow()` passes 267/267); "every window
re-issues PREPARE, so opening one guarantees SSP is on" overstates a call whose result `openWindow()` ignores;
`setPairingWindowMs()` affects the NEXT window; `begin()` opens no boot window if `canPair()` is false;
P6's first clause contradicted its second (it is the control -- Q7 and P6 together are the pin); `BtLink.h`'s
`startPrepare()` comment is wrapped.  Found while demonstrating a mutant: **Q5's NEW-46 comment was false** --
its scan `0x03` is the BOOT window, still open because Q5 never reaches `STREAMING`, not the ABORT's drop
window; identical scan history with the failed-attempt `openWindow()` deleted.  `canPair()` KEEPS its name
(spec §4/§5 and Task 3 use it); its gloss now says what it is and what it must not be read as.  282 checks,
0 failures; `BT-HOST-TESTS: PASS`; the sink gate PASSES on the new pin.

---

### Task 2: The sketch — `printHeartbeat()`, the `pairing=` field, edge prints, the LED

**Files:**
- Modify: `examples/audio/bt_sink_test/bt_sink_test.cpp`

- [ ] **Step 1: Lift the heartbeat out of `loop()`**

The heartbeat block is `loop()` lines 438–539 (`static uint32_t last = 0; if (millis() - last >= 1000) { ... }`). Move its BODY (everything inside the `if`, after `last = millis();`) into a new file-scope function placed just above `loop()`:

```cpp
// The once-a-second heartbeat, and also what `status` prints on demand (Task 3): one body, two callers, so
// the console command and the timer can never disagree about what a heartbeat contains.
static void printHeartbeat() {
    const BtSinkSession::Stats &st = session.stats();
    /* ... the existing body, verbatim, through CONSOLE.println(sink.avrcp().volume()); ... */
}
```

and leave in `loop()`:

```cpp
    static uint32_t last = 0;
    if (millis() - last >= 1000) { last = millis(); printHeartbeat(); }
```

- [ ] **Step 2: The `pairing=` field on the `bt_link` line**

In `printHeartbeat()`, the `bt_link` line currently ends `CONSOLE.print(" state="); CONSOLE.println(BtSinkSession::stateName(session.state()));`. Change it to:

```cpp
    CONSOLE.print(" state="); CONSOLE.print(BtSinkSession::stateName(session.state()));
    // NEW-46: the pairing window, so a 1-in-30 sampled transcript still shows it.  reason names the OPEN window
    // (boot|drop|cmd); off when closed; secs the time left on this heartbeat's clock.
    CONSOLE.print(" pairing="); CONSOLE.print(pairingReasonName(session.pairingReason()));
    CONSOLE.print(" secs="); CONSOLE.println(session.pairingRemainingMs(millis()) / 1000u);
```

with, above `printHeartbeat()`:

```cpp
static const char *pairingReasonName(BtSinkSession::PairingReason r) {
    switch (r) { case BtSinkSession::PAIR_BOOT: return "boot"; case BtSinkSession::PAIR_DROP: return "drop";
                 case BtSinkSession::PAIR_CMD: return "cmd"; default: return "off"; }
}
static const char *pairingEndName(BtSinkSession::PairingEnd e) {
    switch (e) { case BtSinkSession::PAIR_END_PAIRED: return "paired"; case BtSinkSession::PAIR_END_TIMEOUT: return "timeout";
                 case BtSinkSession::PAIR_END_CANCELLED: return "cancelled"; default: return "none"; }
}
```

- [ ] **Step 3: Edge prints and the LED, in `loop()`**

Above `loop()`, add:

```cpp
// The green User LED (LED_BUILTIN = pin 3 = GPIO_AD_04, D3) blinks at 1 Hz while a pairing window is open.
// ** POLARITY: the RevC3 header audit names the pad but not its active level, and the only "active low" note
// in the core is the 1060's D8 -- a different board.  BT_SINK_LED_ON is VERIFIED AT FIRST LIGHT on the bench
// (plan Task 6) and corrected there if this default is wrong.  QEMU cannot see it: a silicon-only witness. **
#ifndef BT_SINK_LED_ON
#define BT_SINK_LED_ON HIGH
#endif
static void pairingIndicator() {
    // Edges are printed here, for the AUTOMATIC windows and for every close.  A commanded window prints its own
    // `pairing=on reason=cmd` from the command (Task 3) -- an extension of an open window is not an edge, and
    // the person who typed it deserves an answer either way -- so the rising edge is skipped for PAIR_CMD.
    static bool wasOpen = false;
    bool open = session.pairingOpen();
    if (open && !wasOpen && session.pairingReason() != BtSinkSession::PAIR_CMD) {
        CONSOLE.print("pairing=on reason="); CONSOLE.print(pairingReasonName(session.pairingReason()));
        CONSOLE.print(" secs="); CONSOLE.println(session.pairingRemainingMs(millis()) / 1000u);
    }
    if (!open && wasOpen) { CONSOLE.print("pairing=off reason="); CONSOLE.println(pairingEndName(session.pairingEnd())); }
    wasOpen = open;
    bool lit = open && ((millis() / 500u) & 1u);
    digitalWrite(LED_BUILTIN, lit ? BT_SINK_LED_ON : !BT_SINK_LED_ON);
}
```

In `loop()`, after `btin.hold(sink.suspended());`, add `pairingIndicator();`. In `setup()`, before `session.begin(...)` (inside the `if (s_hciSt == Hci::OK)` block is fine, but put it before the `if` so the LED is driven even card-absent — off): `pinMode(LED_BUILTIN, OUTPUT); digitalWrite(LED_BUILTIN, !BT_SINK_LED_ON);`.

- [ ] **Step 4: Build, run the gate, read the new lines**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./run_qemu.sh 2>&1 | tail -1 | cut -c1-40; grep -E "^pairing=|^bt_link " build/sink.uart | head -4; grep -c "^hb " build/sink.uart
```
Expected: `PASS: A2DP SINK ...`; a `pairing=on reason=boot secs=119` line right after `sink=listening`; `bt_link ... state=listening pairing=boot secs=1NN` on early heartbeats; then `pairing=off reason=paired` once the peer's link streams and `pairing=off secs=0` after. The `hb` count is unchanged from before this task (the refactor added no heartbeat).

- [ ] **Step 5: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git add examples/audio/bt_sink_test/bt_sink_test.cpp && git commit -q -F - <<'MSG' && git log --oneline -1
audio/bt_sink_test: pairing-window feedback -- edge prints, a pairing= heartbeat field, the User LED at 1 Hz; heartbeat lifted into printHeartbeat()

pairing=on reason=boot|drop secs=N on each automatic opening (a commanded window prints its own),
pairing=off reason=timeout|paired on every close, and ` pairing=<reason|off> secs=N` on the
bt_link line so a sampled transcript still shows the window.  LED_BUILTIN (pin 3, GPIO_AD_04)
blinks while open; its active level is a constant the bench verifies at first light, because
the header audit does not state it.  The heartbeat body moves to printHeartbeat() so the
`status` command (next) and the timer share one body.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
MSG
```

**Task 2 review corrections (2026-09-10).**  The heartbeat move was verified VERBATIM rather than by capture
diff -- the old body dedented one level against the new `printHeartbeat()` differs in exactly one hunk, the
`println`->`print` on `state=` plus the two new fields -- which is the stronger check, since a capture diff
cannot distinguish a dropped print from a run-to-run counter.  Three things the run measured that later tasks
need.  **`secs=119`, not 120, for the AUTOMATIC windows**: `begin()`/`tick()` open the window and
`pairingIndicator()` prints on a LATER loop pass, so the remainder is ~119,9xx ms and `/1000u` truncates; the
COMMANDED print is immune because Task 3's handler passes one `now` to both `enterPairing()` and
`pairingRemainingMs()`, so `pair` really does say `secs=120`.  The literals in Step 4, spec 5/6 and the bench
steps are corrected; Task 4's greps already use `secs=1[0-9][0-9]$` and need no change.  **`#ifndef
BT_SINK_LED_ON` moved up beside the sink objects** -- Step 3 drew it with `pairingIndicator()`, but `setup()`
uses it and sits ~130 lines earlier, so the plan's placement does not compile; the polarity paragraph is
unchanged.  **`transcript_qemu.txt` is stale in FOUR classes, only two of them this task's** -- the two added
`pairing=` lines and the `bt_link` field (Task 2), plus the seven `dry*` fields `5434d26` added to `bt_jit`
and the `fillmin` sentinel reading 32 where the RING-40 firmware prints 40 (both PRE-EXISTING on master, and
therefore already stale when the NEW-42 close-out recorded vacuity 46/46: the silent-staleness class, surviving
because no assertion reads those fields).  Task 4 Step 8's re-capture fixes all four and its commit should say
so.  Two smaller ones for Tasks 3 and 4: `bonds_forgotten=` exists today only inside
`#if defined(M2_BT_FORGET_BONDS)` in `setup()`, so Task 3's handler EMITS that line rather than reusing one;
and `PEER-SCAN-ENABLE` already exists in `hci_peer.py`'s `source` phase (Step 5 need not add it) -- its
assertion is sound, but the comment claiming a windowless bonded sink writes `0x02` at link-up is wrong, the
value there is `0x00` (`BtLink.cpp:170` composes page|inquiry and both go off with a link up).

---

### Task 3: The sketch — `pair` / `forget` / `status`, and the console tool's write path

**Files:**
- Modify: `examples/audio/bt_sink_test/bt_sink_test.cpp`
- Modify: `tools/rt1170-console.py`

**Two requirements added by the Task 2 reviews, both about not making a later reader wrong.**
(a) **`status` must mark its block.** `printHeartbeat()` opens with the literal `hb `, so an on-demand
block is byte-indistinguishable from a timed one -- and NEW-42's bench derived its headline
`over 13.09/min` / `under 16.08/min` by treating heartbeat blocks as a clock, which an untagged `status`
would silently inflate.  The handler prints `cmd=status` on its own line FIRST, which also gives Task 4's
driver a token proving the command landed.  (`last` is a `loop()` local static, so `status` does not
re-phase the timer; that is fine and deliberate -- the timed cadence stays a clock.)
(b) **One owner for the `pairing=on` format.** Spec 5 keeps the split deliberately -- the edge detector
skips `PAIR_CMD` because extending an open window is not an edge, and the command answers either way --
but the two printers must not drift apart.  Factor the line into one helper
(`printPairingOn(BtSinkSession::PairingReason r, uint32_t now)`) and call it from BOTH
`pairingIndicator()` and `runCommand()`, so the format has a single definition even though it has two
callers.


- [ ] **Step 1: The command reader**

Above `loop()` (after `pairingIndicator()`), add:

```cpp
// --- console commands (NEW-46) -----------------------------------------------------------------------------
// Read from serialEvent1(), which the core's yield() calls whenever Serial1.available() -- and loop() calls
// yield() every pass -- so there is no loop() change and no polling.  A line is `\n` or `\r` terminated,
// case-insensitive, EXACT match: a NUL (the VCOM capture starts with one), a control byte or a stray char is
// never a command byte, which is what makes `forget` -- the one destructive command -- safe against line noise.
static char    s_cmd[16];
static uint8_t s_cmdLen = 0;
static bool    s_cmdOver = false;
static void runCommand(const char *c) {
    uint32_t now = millis();
    if (!strcmp(c, "pair")) {
        if (session.enterPairing(now, BtSinkSession::PAIR_CMD)) {
            CONSOLE.print("pairing=on reason=cmd secs="); CONSOLE.println(session.pairingRemainingMs(now) / 1000u);
        } else CONSOLE.println("pairing=refused reason=link_up");
    } else if (!strcmp(c, "forget")) {
        // canPair() first, and wipe ONLY if it holds: a refused forget must leave the table exactly as it was.
        if (!session.canPair()) { CONSOLE.println("pairing=refused reason=link_up"); return; }
        uint8_t before = bonds.count(); (void)BondStoreEeprom::wipe(bonds);
        CONSOLE.print("bonds_forgotten="); CONSOLE.println(before);
        if (session.enterPairing(now, BtSinkSession::PAIR_CMD)) {
            CONSOLE.print("pairing=on reason=cmd secs="); CONSOLE.println(session.pairingRemainingMs(now) / 1000u);
        }
    } else if (!strcmp(c, "status")) {
        printHeartbeat();
    } else {
        CONSOLE.print("cmd=? \""); CONSOLE.print(c); CONSOLE.println("\"");
    }
}
void serialEvent1() {
    while (CONSOLE.available()) {
        int ch = CONSOLE.read(); if (ch < 0) break;
        if (ch == '\n' || ch == '\r') {
            if (s_cmdOver) { s_cmd[s_cmdLen] = 0; CONSOLE.print("cmd=? \""); CONSOLE.print(s_cmd); CONSOLE.println("...\" (too long)"); }
            else if (s_cmdLen) { s_cmd[s_cmdLen] = 0; runCommand(s_cmd); }
            s_cmdLen = 0; s_cmdOver = false;
            continue;
        }
        if (ch < 0x20 || ch > 0x7E) continue;                    // NUL, control bytes, non-ASCII: dropped, never buffered
        if (s_cmdLen < sizeof s_cmd - 1) s_cmd[s_cmdLen++] = (char)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch);
        else s_cmdOver = true;
    }
}
```

`CONSOLE` is `Serial1` on this board; `serialEvent1` is the weak hook `HardwareSerial1.cpp:59` declares. Add `#include <string.h>` if not already present.

- [ ] **Step 2: The console tool gains a stdin→port thread**

In `tools/rt1170-console.py`, change `import sys, time` to `import sys, time, threading`, and replace the body from `try:` (the outer loop) to the end with:

```python
_ser = None                       # the OPEN port, shared with the stdin pump; None while reconnecting
_lock = threading.Lock()

def _stdin_pump():
    """stdin -> port, one line at a time.  Under `nohup ... &` stdin is at EOF at once and this thread simply
    ends; reading is untouched.  Interactively, type `pair`, `forget`, `status` and press return."""
    for line in sys.stdin:
        with _lock:
            s = _ser
        if s is None:
            print("[console] port not open, dropped: %r" % line.rstrip(), file=sys.stderr); continue
        try:
            s.write((line.rstrip("\r\n") + "\n").encode("ascii", "replace"))
        except (OSError, serial.SerialException) as e:
            print("[console] write failed: %s" % e, file=sys.stderr)

threading.Thread(target=_stdin_pump, daemon=True).start()
try:
    while True:
        try:
            ser = serial.Serial(PORT, BAUD, timeout=0.2)
        except (OSError, serial.SerialException):
            time.sleep(0.5)          # port not present (e.g. mid power-cycle); retry
            continue
        with _lock:
            _ser = ser
        try:
            while True:
                data = ser.read(256)
                if data:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
        except (OSError, serial.SerialException):
            with _lock:
                _ser = None
            try: ser.close()
            except Exception: pass
            print("\n[console] port dropped, reconnecting…", file=sys.stderr)
            time.sleep(0.5)
except KeyboardInterrupt:
    print("\n[console] bye", file=sys.stderr)
```

Update the docstring's Usage block with one line: `Type pair / forget / status + return to send a console command to the sink (NEW-46).`

- [ ] **Step 3: Build; smoke the reader in QEMU by hand**

Build the gate image, then drive it manually with a socket console (the same mechanism Task 4 makes
permanent).  **The send must land AFTER `setup()`**: card-absent, `setup()` runs ~20 s (the firmware
download's attempts plus ten 500 ms HCI Resets), so a `sleep 6` lands inside it, before any timed heartbeat
exists.  That earlier draft's "two `hb` lines close together" was therefore unreachable, and the diagnosis
that `loop()` never ran at `QRUN_TIMEOUT=25` was also wrong -- it does, eight timed heartbeats by t=25.
Sleep past `setup()` and give the sends room:

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && cmake --build build 2>&1 | tail -1 && OUT=/tmp/pm-smoke.uart && rm -f $OUT && (QRUN_TIMEOUT=45 ../../../tools/qrun -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel build/bt_sink_test.elf -display none -chardev socket,id=c0,host=127.0.0.1,port=45495,server=on,wait=off,logfile=$OUT -serial chardev:c0 -serial null >/dev/null 2>&1 &) ; sleep 22; python3 - <<'PY'
import socket, time
s = socket.create_connection(("127.0.0.1", 45495), timeout=3)
def send(b, gap=0.6): s.send(b); time.sleep(gap)
send(b"STATUS\r\n")                              # case fold + CR LF -> ONE dispatch
send(b"pair\n"); send(b"bogus\n")
send(b"x"*40 + b"\n")                            # over-length
send(b"for\x00get\n")                            # a NUL INSIDE forget must NOT fire it
send(b"forget\n")
send(b"pair\nstatus\nbogus\n", 1.5)              # three commands in ONE write: order, nothing lost
send(b"status\nstatus\nstatus\nstatus\n", 2.0)   # four heartbeat blocks back to back: no livelock
s.close()
PY
sleep 2; pkill -f "port=45495"; tr -d '\r\000' < $OUT | grep -nE "^(cmd=|pairing=|bonds_|hb )"
```
Expected: the boot `pairing=on reason=boot`; `cmd=status` then an `hb` block for the `STATUS` line (case
folded, and CR LF costs two invocations, the second on an empty line, so there is exactly ONE dispatch);
`pairing=refused reason=not_listening` for `pair` **and** for `forget` -- card-absent, `a2dp_sink=deferred`
means the session never began, so `canPair()` is false for the `not_listening` reason and no
`bonds_forgotten` line is printed; `cmd=? "bogus"`; `cmd=? "xxxxxxxxxxxxxxx..." (too long)`;
`cmd=? "forget" (bad byte)` -- the destructive command visibly did not fire; then the burst answered in
order (refusal, `cmd=status` + `hb`, `cmd=? "bogus"`) and four `cmd=status` blocks from the second burst.
★ **A run at `sleep 6` is worth doing once, and not as the test**: the commands ARE answered from inside
`setup()`, because every `delay()` there calls `yield()` and `yield()` dispatches this handler.  That is
the cleanest proof of how the reader is driven -- and exactly why there is no heartbeat to pair it with.

★ **The 2 s partial-line timeout needs its own run, and a WALL-clock gap will not do**: QEMU's guest clock
ran ~0.67x wall here, so `sleep 2.5` between `for` and `get` was ~1.7 guest seconds, the timeout did not
fire, the two fragments spliced into `forget` and it RAN (NEW-42's "time magnitudes are a fiction" wearing
a new face).  Count HEARTBEATS, which are the guest's own second: send `for`, wait for six `hb` lines, then
`get\n` -> `cmd=? "get"`.  The short-gap run is the control and should be kept: it shows the splice this
timeout exists to prevent, live.

- [ ] **Step 4: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git add examples/audio/bt_sink_test/bt_sink_test.cpp tools/rt1170-console.py && git commit -q -F - <<'MSG' && git log --oneline -1
audio/bt_sink_test: console commands pair / forget / status; rt1170-console.py sends stdin lines

A serialEvent1() override (the core's yield() dispatches it; loop() already yields every pass):
newline-terminated, case-folded, exact-match, 16-byte lines; NUL, control bytes and non-ASCII are
never buffered, and an over-long line is reported, not executed -- which is what makes `forget`,
the one destructive command, safe against the NUL this VCOM capture always starts with.  `pair`
opens or extends a window (refused with a link up); `forget` wipes the bond store ONLY if the
window could open, then opens one; `status` prints the heartbeat now.  The console tool gains a
stdin-to-port thread that ends harmlessly at EOF under nohup.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
MSG
```

**Task 3 review corrections (2026-09-10).**  Step 1's code block above is the PRE-REVIEW draft; the file is
authoritative where they differ.  Five changes, each demonstrated by a QEMU smoke run (there is no host suite
for the sketch), and each stated by the harm it prevents rather than by the rule it adds.  **A LIVELOCK
BOUND**: the draft's `while (CONSOLE.available())` dispatched EVERY queued line per invocation, and
`yield()` holds its `running` flag across the call -- so a blocking print's nested `yield()` is a no-op and
`EventResponder::runFromYield()`, the HciPump and therefore the audio path, does not run until the handler
RETURNS.  `status` is 7 bytes in and ~550 out, 80x, against an 11.5 KB/s console; a held Enter key (~30/s)
already asks 16.5 KB/s, the 4 KB TX ring stays full and `HardwareSerialIMXRT::write()` spins on a `yield()`
that cannot pump -- a board that looks alive and is dead on Bluetooth, NEW-41's observer effect with nothing
bounding it.  It now dispatches ONE command per invocation and returns; the rest waits in the 64-byte RX
ring.  Measured: three commands in one write are answered in order, and four `status` blocks back to back
all land.  **A PARTIAL LINE EXPIRES (2 s)**: `s_cmdLen` is static and nothing in `loop()` cleared it, so
`for` + a Ctrl-C + a restarted console + `get` ran `forget` and wiped the bond store.  Demonstrated BOTH
ways in QEMU -- over the timeout, `cmd=? "get"`; under it, the splice fires -- which is also how the guest
clock was caught running ~0.67x wall (see Step 3).  **A BOND-STORE READINESS FLAG**: `forget` was guarded
only by an accident of `setup()`'s ordering, and this handler is reachable from the FIRST `delay(10)` in
`m2ReleaseWifiReset()`, hundreds of lines before `BondStoreEeprom::load()`.  Move `session.begin()` earlier
for any reason and `forget` goes live over an UNREAD table: `wipe()` is `t.clear(); save(t)` and
`BondTable::clear()` dirties UNCONDITIONALLY, so the canonical empty image lands on top of real persisted
bonds, the next `load()` returns empty, and `bonds_boot=0` reads perfectly normal -- silent and
destructive.  `s_bondsLoaded` is set at each `load()` call site and required before the wipe.  **THE
REFUSAL SPLIT** (spec §5): `reason=link_up` was printed for IDLE, MANUAL, CONNECTING and DISCONNECTING too,
so the card-absent image printed a flatly untrue sentence twice under a heartbeat reading `state=idle
links=0`.  `reason=not_listening` covers those; Task 4's two `pairing=refused reason=link_up` matches are
the while-STREAMING case, a genuine link up, and are unaffected byte for byte.  **STEP 3's TIMING**: the
send landed inside `setup()`, which the note there now explains along with what the t=6 s run is actually
good for.  Three smaller ones: a non-printable byte now REFUSES the line (`cmd=? "..." (bad byte)`) instead
of being deleted from it, because deleting turns `for<NUL>get` back into a firing `forget` -- the old
comment's "safe against line noise" was true only of over-length; the NUL justification cited the wrong
direction (CLAUDE.md's leading NUL is board->host, this filter is for a BREAK or DTR transition on RX); and
`BondStoreEeprom::wipe()`'s return is now checked and reported on its own line rather than cast to void two
lines under a comment insisting `enterPairing()`'s must not be dropped.

---

### Task 4: Gate 139 — socket console, the command driver, the peer's drop-and-re-page, assertions, vacuity

**Files:**
- Create: `examples/audio/bt_sink_test/sink_console.py`
- Modify: `examples/audio/bt_sink_test/run_qemu.sh`
- Modify: `examples/networking/m2_hci_probe/hci_peer.py`
- Modify: `examples/audio/bt_sink_test/transcript_qemu.txt` (re-captured)
- Modify: `tools/gate-vacuity.test.sh`

- [ ] **Step 1: Write the gate assertions FIRST (RED against the unmodified peer and driver)**

In `run_qemu.sh`, after the `[jit]` block and before the final `echo "PASS: ...`, add:

```sh
# --- the pairing window (NEW-46), judged against what the PEER and the DRIVER know -----------------------------
# The console driver sends `pair` while STREAMING (must be refused: a window is never open with a link up), and
# after the peer's injected drop sends `pair`, `forget` and `status`.  What only the peer can see: its own
# Write_Scan_Enable log -- the sink turning INQUIRY scan on after the drop is the window, and the firmware
# cannot invent a controller write.  What only the driver can see: that it sent the commands at all.
# ★ `forget` is exercised here ONLY because QEMU has no NVM behind the FlexSPI window (CLAUDE.md): the bond
# store this wipes is a within-boot table.  On silicon it is destructive, by design.
grep -q "^pairing=on reason=boot secs=1[0-9][0-9]$" "$OUT"          || fail "[pair] no boot window: $(grep -m1 '^pairing=' "$OUT")"
grep -q "^pairing=off reason=paired$" "$OUT"                          || fail "[pair] the boot window did not close as PAIRED when the link came up"
grep -q "^DRIVER-SENT pair while-streaming" "$CON"                    || fail "[pair] the driver never sent pair while streaming: $(tail -3 "$CON")"
# `link_up` and not `not_listening`: Task 3's review split the refusal in two (spec 5), and THIS one is a
# genuine link up, so the text is unchanged.  Anchored, so the other reason can never satisfy it.
grep -q "^pairing=refused reason=link_up$" "$OUT"                     || fail "[pair] pair while streaming was not REFUSED"
grep -q "^PEER-SOURCE-DROP " "$RES"                                   || fail "[pair] the peer did not inject its drop: $(grep -m1 PEER-SOURCE-DROP "$RES")"
grep -q "^pairing=on reason=drop secs=1[0-9][0-9]$" "$OUT"            || fail "[pair] no drop window after the peer's disconnect"
# The un-fakeable half: after the drop, the peer must see inquiry scan turned ON (0x03) -- a bonded sink
# with no window writes 0x02 there.  Counted AFTER the PEER-SOURCE-DROP line, not anywhere in the log.
awk '/^PEER-SOURCE-DROP /{d=1} d && /^PEER-SCAN-ENABLE 0x03/{n++} END{exit !(n>=1)}' "$RES" \
    || fail "[pair] the peer never saw inquiry scan re-enabled after its drop: $(grep PEER-SCAN-ENABLE "$RES" | tail -3)"
[ "$(grep -c '^ssp_mode: st=ok status=0x00 mode=1' "$OUT")" -ge 2 ]   || fail "[pair] PREPARE was not re-issued for the drop window (want a second ssp_mode line): $(grep -c '^ssp_mode' "$OUT")"
grep -q "^pairing=on reason=cmd secs=1[0-9][0-9]$" "$OUT"             || fail "[pair] the commanded window never opened"
grep -q "^bonds_forgotten=1$" "$OUT"                                  || fail "[pair] forget did not wipe the one bond"
grep -q "^PEER-SOURCE-REPAGE " "$RES"                                 || fail "[pair] the peer did not re-page after forget"
[ "$(grep -c '^conn_req: bd=AA:BB:CC:DD:EE:01 -> accept(slave, unbonded)' "$OUT")" -ge 2 ] \
    || fail "[pair] the re-page after forget was not accepted as UNBONDED (the bond survived, or no re-page)"
[ "$(grep -c '^link_key_req: .* -> neg_reply (no stored key)' "$OUT")" -ge 2 ] \
    || fail "[pair] the re-page after forget did not restart pairing from scratch"
[ "$(grep -c '^pairing_complete: status=0x00' "$OUT")" -ge 2 ]        || fail "[pair] the re-page after forget did not pair Just Works again"
# `status` prints a heartbeat NOW: two hb lines inside one second, which the 1 s timer alone cannot produce.
grep -q "^DRIVER-STATUS-OK" "$CON"                                     || fail "[pair] status did not produce an immediate heartbeat: $(grep DRIVER-STATUS "$CON")"
```

and extend the final `PASS:` line's text with: `; the pairing window opens at boot and closes PAIRED on link-up, pair is REFUSED while streaming, the peer's injected drop opens a drop window it can SEE (inquiry scan 0x03 re-enabled) with PREPARE re-issued, and after forget the peer's re-page is accepted UNBONDED and pairs Just Works again -- the NEW-43 recovery without a reboot`.

Near the top, after `OUT=...; DBG=...; RES=...`, add `CON="$BUILD_DIR/sink.console"` and `rm -f "$CON"`.

- [ ] **Step 2: Run the gate to verify it fails (RED)**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./run_qemu.sh 2>&1 | tail -1
```
Expected: `FAIL: [pair] the driver never sent pair while streaming:` (the console log does not exist yet). If it fails EARLIER on `no boot window`, Task 2 is not built into `build/` — rebuild and retry.

- [ ] **Step 3: The console driver**

Create `examples/audio/bt_sink_test/sink_console.py`:

```python
#!/usr/bin/env python3
"""The sink gate's CONSOLE driver (NEW-46).  Connects to QEMU's LPUART1 socket -- the same bytes the gate's
capture file receives, because the chardev carries logfile= -- waits for UART milestones, and sends the
console commands at the moments the assertions need: `pair` while STREAMING (to be refused), then after the
peer's injected drop `pair`, `forget` and `status`.  It writes DRIVER-* lines to stdout; the gate greps them.
It never judges the firmware: the gate does that from the capture and the peer's tally."""
import socket, sys, time, re
HOST = "127.0.0.1"; PORT = int(sys.argv[1]); DEADLINE = time.time() + float(sys.argv[2]) if len(sys.argv) > 2 else time.time() + 50
def connect():
    end = time.time() + 10
    while time.time() < end:
        try:
            s = socket.create_connection((HOST, PORT), timeout=2.0); s.settimeout(0.25); return s
        except OSError: time.sleep(0.2)
    print("DRIVER-FAIL could not connect to the console socket"); sys.exit(1)
buf = b""
def wait_for(s, pat, what):
    """Block until a line matching pat has been RECEIVED (bytes already in buf count).  Returns the match."""
    global buf
    while time.time() < DEADLINE:
        m = re.search(pat, buf.decode("latin-1"), re.M)
        if m: return m
        try: b = s.recv(4096)
        except socket.timeout: continue
        if not b: break
        buf += b
    print("DRIVER-FAIL timed out waiting for %s" % what); sys.exit(1)
def send(s, line, note):
    s.send((line + "\n").encode()); print("DRIVER-SENT %s %s" % (line, note)); sys.stdout.flush()
s = connect(); print("DRIVER-CONNECTED"); sys.stdout.flush()
wait_for(s, r"^streaming by=incoming", "STREAMING")
time.sleep(0.5)
send(s, "pair", "while-streaming")
wait_for(s, r"^pairing=refused reason=link_up", "the refusal")
wait_for(s, r"^pairing=on reason=drop", "the drop window")            # the peer injects the drop after its volume set
send(s, "pair", "after-drop")
wait_for(s, r"^pairing=on reason=cmd", "the commanded window")
send(s, "forget", "after-drop")
wait_for(s, r"^bonds_forgotten=1", "the wipe")
hb_before = len(re.findall(r"^hb ", buf.decode("latin-1"), re.M)); t0 = time.time()
send(s, "status", "after-forget")
wait_for(s, r"^bt_avrcp ", "a heartbeat block")                        # the LAST line of a block: the whole block is in
hb_after = len(re.findall(r"^hb ", buf.decode("latin-1"), re.M))
print("DRIVER-STATUS-%s hb %d -> %d in %.2fs" % ("OK" if hb_after > hb_before and time.time() - t0 < 0.9 else "SLOW", hb_before, hb_after, time.time() - t0))
print("DRIVER-DONE"); sys.stdout.flush()
```

`DRIVER-STATUS-OK` requires the heartbeat to arrive within 0.9 s of `status` — the timer alone fires once a second, so a block that lands that fast came from the command. (Under heavy sweep load the timer's own block could coincide; the driver counts `hb` lines, and a coincidence would give `hb_after - hb_before == 2` — still OK. A SLOW result is the finding this check exists for.)

- [ ] **Step 4: The gate's console chardev and the driver's launch**

In `run_qemu.sh`, the real-run branch currently launches QEMU with `$(gate_console "$OUT")`. Replace that branch with:

```sh
else
    SOCK="/tmp/m2sink_$$.sock"; rm -f "$SOCK"; gate_tmp "$SOCK"
    # NEW-46: the console is a SOCKET so the driver can type at it, with logfile= so every byte the sink
    # prints still lands in $OUT for the assertions below (smoke-tested: the banner lands in the file).  The
    # vacuity harness's fake QEMU recognises only `-serial file:`, which is why the GATE_VACUITY branch above
    # keeps gate_console.  The console is the FIRST -serial (LPUART1); the HCI socket stays the second.
    CPORT=$((45500 + ($$ % 400)))
    "$QEMU" $(gate_qemu_machine) -kernel "$ELF" -display none \
        -chardev socket,id=con,host=127.0.0.1,port=$CPORT,server=on,wait=off,logfile="$OUT" -serial chardev:con \
        -serial unix:"$SOCK",server -d guest_errors -D "$DBG" &
    P=$!; gate_pid $P
    python3 "$DIR/sink_console.py" $CPORT 55 > "$CON" 2>&1 &
    CPID=$!
    PEER_RC=0
    python3 "$EVKB/examples/networking/m2_hci_probe/hci_peer.py" source "$SOCK" "$DIR/sine.sbc" > "$RES" 2>&1 || PEER_RC=$?
    wait $CPID 2>/dev/null || true
fi
```

Under `GATE_VACUITY=1` the driver does not run, so also give the vacuity replay a stand-in console log: in the vacuity branch, after `cp "$GATE_PEER_FIXTURE" "$RES"`, add `[ -n "${GATE_CONSOLE_FIXTURE:-}" ] && cp "$GATE_CONSOLE_FIXTURE" "$CON" || : > "$CON"`.

- [ ] **Step 5: The peer — drop after the volume set, then re-page**

In `hci_peer.py`, in the `source` phase's `src` dict add `"dropped": False, "drop_at": 0.0, "repaged": False,` after `"paged": False,`. In `src_avrcp_pump()` (or wherever `set_ok` is assigned to 1), record the time: `s["drop_at"] = time.time() + 1.0` right where `s["set_ok"] = 1` is set. In the `source` run-loop block, replace

```python
        if phase == "source":
            if peer.src["started"]: peer.src_pump()
            peer.src_avrcp_pump()
```
with
```python
        if phase == "source":
            if peer.src["started"]: peer.src_pump()
            peer.src_avrcp_pump()
            s = peer.src
            # NEW-46: after the volume set, DROP the link (as lifecycle does), then re-page the sink ~8 s later.
            # The gate's console driver has `forget` in by then, so this page finds NO bond and must restart
            # pairing from scratch -- the un-fakeable proof the wipe happened and SSP is still on.
            if s["set_ok"] == 1 and not s["dropped"] and time.time() >= s["drop_at"]:
                peer.send(event(0x05, b"\x00" + struct.pack("<H", s["handle"]) + b"\x13"))       # Disconnection_Complete, reason 0x13
                s["dropped"] = True; s["drop_at"] = time.time() + 8.0
                peer.log.append("PEER-SOURCE-DROP handle=0x%04x reason=0x13" % s["handle"])
                peer.src_reset_link()                                                              # fresh handle, fresh SSP state, media closed
            elif s["dropped"] and not s["repaged"] and time.time() >= s["drop_at"]:
                peer.src_page()                                                                     # Connection_Request -> the sink accepts as slave
                s["repaged"] = True; peer.log.append("PEER-SOURCE-REPAGE handle=0x%04x" % s["handle"])
```

`src_page()` is the function the phase already uses for its first page (find it by the `PEER-SOURCE-PAGING` log line); `src_reset_link()` must be added beside it: it clears the per-link state the first page set up (`sig_*`, `media_*`, `psm`, `cfg`, `avctp_open`, `state = "idle"`, `started = False`) and picks a new handle (`s["handle"] += 1`) exactly as `lifecycle` does between legs — read that phase's leg-2 setup and mirror it. Do NOT reset `pkts`, `set_ok`, `delay_reports` or `sink_record_ok`: the tally lines and the done-clause read them.

Extend `phase_done("source")` to require the drop and the re-page's SSP to have completed:

```python
        return (s["started"] and s["pkts"] >= SOURCE_PKTS and s["errors"] == 0 and not peer.avdtp["error"]
                and s["avctp_open"] and s["set_ok"] == 1
                and s["dropped"] and s["repaged"] and peer.ssp_complete_count() >= 2)
```

where `ssp_complete_count()` returns how many `Simple_Pairing_Complete` events the peer has SENT (add a counter where it sends them). Raise `DEADLINE["source"]` from 50 to 65 (the drop-and-re-page adds ~10 s).

- [ ] **Step 6: Run the gate (GREEN)**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./run_qemu.sh 2>&1 | tail -1 | cut -c1-60; echo "--- driver ---"; cat build/sink.console; echo "--- the window lines ---"; grep -E "^pairing=|^bonds_forgotten|^ssp_mode|^conn_req|^link_key_req|^pairing_complete" build/sink.uart; echo "--- peer ---"; grep -E "PEER-SOURCE-DROP|PEER-SOURCE-REPAGE|PEER-SCAN-ENABLE" build/sink.peer
```
Expected: `PASS: A2DP SINK ...`; the driver log ending `DRIVER-DONE` with `DRIVER-STATUS-OK`; the UART showing boot window → `pairing=off reason=paired` → `pairing=refused reason=link_up` → `pairing=on reason=drop` → a second `ssp_mode` → `pairing=on reason=cmd` (twice, for `pair` and `forget`) → `bonds_forgotten=1` → a second `conn_req ... accept(slave, unbonded)`, `link_key_req ... neg_reply`, `pairing_complete: status=0x00`; the peer showing `PEER-SOURCE-DROP`, then `PEER-SCAN-ENABLE 0x03`, then `PEER-SOURCE-REPAGE`.

- [ ] **Step 7: Demonstrate RED by name, then restore**

(a) In `BtSinkSession.cpp` `openWindow`, remove `m_sink.link().startPrepare();` (rebuild M2Radio-linked `build/`) → `[pair] PREPARE was not re-issued`. (b) In the sketch's `runCommand`, make `forget` skip the wipe → `[pair] forget did not wipe the one bond`, and separately the `accept(slave, unbonded)` count check. (c) In the peer, set `s["dropped"] = True` without sending the event → `[pair] no drop window after the peer's disconnect`. (d) In `openWindow`, drop the `canPair()` guard → `[pair] pair while streaming was not REFUSED`. Restore all; gate green; **M2Radio must be clean at the pinned SHA afterwards** (`git -C ~/Development/M2Radio status --short` empty).

- [ ] **Step 8: Re-capture the transcript; vacuity**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./run_qemu.sh >/dev/null 2>&1 && cp build/sink.uart transcript_qemu.txt && cp build/sink.console transcript_console.txt && grep -c "^pairing=" transcript_qemu.txt
```

In `tools/gate-vacuity.test.sh` section 13: add the new peer lines to the `PEERGREEN` heredoc (`PEER-SOURCE-DROP handle=0x0001 reason=0x13`, `PEER-SCAN-ENABLE 0x03`, `PEER-SOURCE-REPAGE handle=0x0002` — copy the exact lines from `build/sink.peer`, and note the accepted-page line will now appear twice); export `GATE_CONSOLE_FIXTURE="$EVKB/$sink_rel/transcript_console.txt"` beside `GATE_PEER_FIXTURE`; and add case (d): the committed transcript with every `pairing=on reason=drop` line deleted must fail by name `[pair] no drop window`. Run the suite alone and idle (~10 min; background it):

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && ./tools/gate-vacuity.test.sh > /tmp/new46-vacuity.log 2>&1; grep -c "^PASS:" /tmp/new46-vacuity.log; grep "^FAIL:" /tmp/new46-vacuity.log
```
Expected **47** PASS (46 + `no_drop_window_fails_sink_gate`), no FAIL — re-derived from the run.

- [ ] **Step 9: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git add examples/audio/bt_sink_test/sink_console.py examples/audio/bt_sink_test/run_qemu.sh examples/audio/bt_sink_test/transcript_qemu.txt examples/audio/bt_sink_test/transcript_console.txt examples/networking/m2_hci_probe/hci_peer.py tools/gate-vacuity.test.sh && git commit -q -F - <<'MSG' && git log --oneline -1
audio/bt_sink_test gate: the pairing window, driven over a socket console and judged by the peer

The console becomes a socket chardev with logfile= (every byte still lands in the capture; the
vacuity branch keeps -serial file: because its fake QEMU knows only that form).  sink_console.py
sends `pair` while STREAMING (refused), then after the peer's injected drop `pair`, `forget`
and `status`.  The peer's source phase drops the link after the volume set and re-pages ~8 s
later -- by then forget is in, so the page finds NO bond and must pair Just Works again: a
second accept(slave, unbonded), a second neg_reply, a second pairing_complete.  What only the
peer can see is asserted from its log: inquiry scan re-enabled (0x03) AFTER its drop.  PREPARE's
re-issue is a second ssp_mode line.  RED four ways by name.  Gate count unchanged at 139;
vacuity 47/47 with a no-drop-window negative.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
MSG
```

**Task 4 review corrections (2026-09-10).**  Two reviews, both passing on substance -- the spec review
verified every claim independently and found no defect, the quality review found five Important issues and
none of them makes the gate pass when it should not.  Everything below is diagnosis quality, recorded
evidence and precision.  **THE FIVE RED DEMONSTRATIONS WERE RECORDED NOWHERE**: the header still described a
NEW-41 gate with no pairing window and its `DEMONSTRATED RED` list stopped at NEW-42's (h), leaving eleven new
assertions outside the tree's own rule that a regression gate never shown to fail is decoration.  A
`DEMONSTRATED RED (2026-09-10)` block now carries one entry per mutation with its exact failure line -- five,
not the four this task planned, because `forget`-that-reports-but-does-not-wipe is a separate and sharper arm
than `forget`-that-does-neither: on it `bonds_forgotten=1` STILL PRINTS (the count is read before the wipe, so
the one line a reader would trust is the one that lies) and the only visible change is on the far side of the
link, the re-page degrading from `accept(slave, unbonded)` + `neg_reply` to `accept(slave)` +
`reply(stored type=4)`.  **THE STATUS VERDICT'S STRUCTURAL HALF WAS WEAKER THAN ITS OWN DOCSTRING**: the lazy
`(?:[^\n]*\n)+?` did not require the closing `bt_avrcp` to belong to the commanded block -- measured on a
synthetic capture, a block truncated after its `hb ` line still matched by running through the five
HCI-settings lines and borrowing the NEXT block's closer, 13 lines instead of 7.  Pinned to the block's exact
shape (`hb` plus exactly four lines), which `printHeartbeat()` emits unconditionally.  **NOTHING GREPPED
`DRIVER-FAIL` AND THE DRIVER'S EXIT CODE WAS DISCARDED**, so a driver that died mid-run was diagnosed by the
firmware-shaped assertion it made unreachable -- kill it after its second command and the first failure reads
`[pair] the commanded windows never opened`, blaming a sink nothing was ever typed at, which is the shape this
file's header says the gate exists to prevent.  Both are now read at the TOP of the `[pair]` block, and a peer
that failed fast reaps the driver instead of waiting out its 45 s budget.  **`transcript_console.txt` WAS
COMMITTED AND READ BY NOTHING** while the vacuity suite carried a byte-identical heredoc copy: two copies that
will drift, and the version-controlled one -- the run's evidence -- was the one nobody executed.
`GATE_CONSOLE_FIXTURE` now replays the committed file, as case (b) already does for `transcript_qemu.txt`.
**ONE `DRIVER-SENT` LINE OF FOUR WAS ASSERTED**, so the header's claim to rule out self-opening `reason=cmd`
windows was false for two of them, and the "reason=cmd twice" count could not tell "pair opened one and forget
opened one" from "forget opened two" -- which is exactly what its comment claimed.  All three post-drop sends
are now read, and the capture's ORDER is pinned (`reason=cmd` -> `bonds_forgotten=1` -> `reason=cmd`) with the
sequence idiom `m2_uap_lwip[uap]` uses for configure-before-BSS_START.

**Two gaps are now NAMED rather than left to be read into the `PASS:` line** (the `[lifecycle]` `btout.end()`
precedent), in the gate header, at the peer's `Encryption_Change`-only branch, and in spec §6.2: the peer's
minimal re-page does no SDP/AVDTP/media, so the gate proves the sink re-**pairable** and never re-**usable**;
and because that link never reaches `STREAMING`, `pairing=off reason=paired` is exercised on the BOOT window
only.  Widening the peer was rejected -- the run is 39 s against qrun's 60 s cap and QEMU cannot carry A2DP at
the audio rate (CLAUDE.md) -- so Task 6's bench owns both.

**One judgement call went the strong way and cost a driver change.**  The review offered "say *a* window" or
"move the assertion ahead of the driver's first post-drop send" for the `PEER-SCAN-ENABLE 0x03` check.
`reconcileScan()` writes only on a CHANGE, so all three post-drop windows coalesce into one controller write
and whichever opened first owns it -- and the pre-review capture had the COMMANDED window first
(`pairing=on reason=cmd` twice, THEN `scan_enable=0x03`).  Rather than trim the claim, `sink_console.py` now
waits for the sink's own `scan_enable=0x03` after the drop and logs `DRIVER-SAW` before typing; the gate
asserts that line and that it precedes every post-drop `DRIVER-SENT`.  The capture inverted as intended
(`reason=drop` -> `scan_enable=0x03` -> the two `reason=cmd`), so `transcript_qemu.txt` and
`transcript_console.txt` were RE-CAPTURED -- the only non-counter change in the UART is those two scan lines
moving.  The `[pair]` message for PREPARE was reworded in the same pass to stop claiming a localisation
`grep -c ... -ge 2` cannot give (in the real capture the second `ssp_mode` lands after the FORGET window);
mutants (i) and (iii) were re-run against the reworded file so both quoted lines are verbatim.

Smaller, all taken: the `[jit]` message said "streaming heartbeat" of a variable that is now the final one; a
`secs=1[0-9][0-9]` comment at first use; `BtLink::reconcileScan()` cited by FUNCTION rather than by a line
number in a sibling repo at a moving pin; `tail -3` where `grep DRIVER-STATUS` prints nothing in the commonest
failure; the wipe check moved AHEAD of the window-count check so a broken `forget` stops failing as "the
commanded windows never opened"; the port probe's TOCTOU window stated (only EXHAUSTION is reported by name, a
lost race still surfaces as `no UART capture`); `sendall()` with a guard, an argc guard, EOF distinguished
from timeout, the unused `wait_for` return dropped, the one wait that re-used an older mark given a fresh one,
`mark()`'s docstring told what the region includes, `time.sleep(0.5)` justified as a margin and not a
synchronisation (`m_state = STREAMING` is assigned before the callback that prints the line), and
`STATUS_BOUND = 0.9` recorded as an IDLE-only measurement with NEW-42's `[jit]` floor as the precedent for
saying so.  In `hci_peer.py`: `DEADLINE["source"]` 65 -> 55, a DIAGNOSTIC change rather than a budgetary one
(65 is above qrun's 60 s cap, so every unhealthy run printed `PEER-EOF` -- which cannot separate "we outran
the cap" from "QEMU crashed" -- and never `PEER-DEADLINE`, the line that names the class; healthy is ~30 s);
`not peer.avdtp["error"]` KEPT in `phase_done("source")` but documented, because dropping it is not free --
the phase's own writer also bumps `s["errors"]`, but `handle_acl`'s and `handle_sdp`'s bump `rc["errors"]`,
which that clause is the only reader of, and `reset_link()` at the re-page clears it; and the drop/re-page
chain given a `repage_at` of its own so its three stages read instead of having to be proved from the
latches.  DECLINED: a `gate_console_socket()` helper in `gate-lib.sh` -- one caller, noted in a comment
citing `gate_console()`'s own warning instead.

---

### Task 5: Rebuild, freshness, fresh-user, sweep, audit, vacuity

Identical in shape to the NEW-42 close-out (`docs/superpowers/plans/2026-09-09-bt-sink-jitter-absorption.md` Task 8), with the M2Radio pin now at Task 1's SHA. In order, none concurrent:

- [ ] **Step 1:** rebuild every M2Radio-linking gate build dir (skip bench dirs whose `CMakeCache.txt` carries a real `M2RADIO_IW416*_FW` path, and `build-fetch`); expect 0 failures apart from the pre-existing `acid_box/build-bt` ITCM overflow (NEW-45 — it is a bench dir with an EMPTY blob path, so it slips the filter; skip it by name).
- [ ] **Step 2:** mtime freshness of those ELFs against `~/Development/M2Radio/bt/BtSinkSession.cpp`, restricted to dirs that compile it; expect 0 stale.
- [ ] **Step 3:** fresh-user: `rm -rf build-fetch`, configure with `-DEVKB_FORCE_FETCH=ON`, confirm the log fetches M2Radio at Task 1's SHA, build, swap `build`→`build-fetch` and run the gate with `GATE_VACUITY=1` (real peer, fetched ELF), restore. Expect PASS.
- [ ] **Step 4:** the full sweep alone, output to `/tmp/new46-sweep.log`; target `gates: 139 passed`; disposition any red with an idle re-run.
- [ ] **Step 5:** `./tools/license-audit.sh` AFTER the sweep → `LICENSE-AUDIT: PASS` (no new build dir; the new `sink_console.py` and `transcript_console.txt` are not compiled).
- [ ] **Step 6:** vacuity alone → **47** PASS, re-derived.
- [ ] **Step 7:** both host suites green.


**Task 5 MEASURED 2026-09-10 (recorded here so the numbers are not carried from a report).**
Sweep **139 discovered, 139 passed, 0 failed, 0 SKIP**, `-l` reports 139, 23m29s wall, alone.
Every historically load-sensitive gate green IN THE SWEEP ITSELF -- including
`display/synthui_slide_toggle_test`, the ONE red of the NEW-42 close-out hours earlier, which nothing in
this branch touches: the third independent confirmation that the load-sensitive set is a property of
machine load and not of those gates.  `LICENSE-AUDIT: PASS`, 118 manifests, `bt_sink_test` at 1177 dep
paths -- identical to NEW-41's, consistent with no new COMPILED file (`sink_console.py` and
`transcript_console.txt` need no `GATES` entry, as predicted).  Vacuity **47**, re-derived.  Host suites:
M2Radio 14 binaries `BT-HOST-TESTS: PASS` with `btsinksession_test: 282 checks`; the sink example
`servo_test: 113`, `node_test: 365` (default) and `337` (CONTROL arm).  Rebuild: 23 M2Radio-linking gate
dirs built, 19 bench dirs skipped by their real blob path, `acid_box/build-bt` skipped BY NAME, 0
failures, 0 dirs hitting the cached-toolchain trap.  Freshness: 12 dirs compile `BtSinkSession.cpp.obj`,
12 checked by mtime, 0 stale.  Fresh-user: the configure log shows M2Radio cloned at
`b90d52b8c5ca20c4e98658fec85181e584596a32` and the gate PASSED on that ELF under `GATE_VACUITY=1` with no
fixture -- which is the invocation that skips the gate's own rebuild (it would otherwise recompile the
fetched ELF from local-first sources and destroy the point) while still running the real peer and the
real console driver.
★ **`acid_box/build-bt` overflows ITCM by 140 bytes -- BIT-FOR-BIT the figure NEW-45 recorded against the
OLD pin**, so this branch's library change provably did not move it, and no gate reads that directory.
★ **A freshness check that measured NOTHING reported green.** The first attempt globbed
`BtSinkSession.cpp.o` and printed `checked: 0 stale: 0`; the macros emit `.obj`.  A zero-denominator
check is indistinguishable from a clean one in its own output -- print the denominator, and read it.

---

### Task 6: The bench

**Files:**
- Modify: `examples/audio/bt_sink_test/bt_sink_test.cpp` (only if the LED constant is wrong)
- Modify: `examples/audio/bt_sink_test/transcript_hw_evkb.txt` (RUN 9 appended)

- [ ] **Step 1: Build the bench image** — same recipe as NEW-42 RUN 8 (`build-bench-change`, the real blob, `M2_BT_FORGET_BONDS=OFF`), rebuilt from this branch. Flash `.hex`, verify, kill every LinkServer daemon, then run the console tool **in the foreground** so commands can be typed: `python3 tools/rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 | tee /tmp/sink-bench-run9.log`.

- [ ] **Step 2: First light — the LED.** Press SW4. Within the first two minutes the LED must blink at ~1 Hz. If it is *solid* or *off* while `pairing=on reason=boot` shows on the console, the polarity is inverted: change `BT_SINK_LED_ON` to `LOW`, rebuild, reflash, repeat. Record the verified level in the transcript header and in the sketch comment.

- [ ] **Step 3: The boot window.** LED blinking; `pairing=on reason=boot secs=119` on the console; the phone lists EVKB-SINK. Wait two minutes without connecting: LED stops, `pairing=off reason=timeout`, the phone no longer lists it (bonded sinks are invisible outside a window).

- [ ] **Step 4: Type `pair`.** LED resumes, `pairing=on reason=cmd secs=120`, the phone lists it again; connect; `pairing=off reason=paired`, LED off, music streams.

- [ ] **Step 5: `pair` while streaming** → `pairing=refused reason=link_up`; nothing else changes; music continues.

- [ ] **Step 6: Range loss.** Walk out of range → `stream_lost` → `pairing=on reason=drop`, LED blinks; walk back; iOS does not re-page after a range loss (NEW-41), so re-select the speaker on the phone → stored-key reconnect → `pairing=off reason=paired`.

- [ ] **Step 7: `forget` — the NEW-43 recovery, the whole reason for this feature.** Stop the music, disconnect from the phone (so `canPair()` holds), type `forget` → `bonds_forgotten=1`, `pairing=on reason=cmd`. On the phone: Forget This Device. Pair fresh: **Just Works, no passcode prompt, no SW4.** `a2dp_sink=ok bonds=1 paired_by=ssp` (MEASURED RUN 9).  ★ An earlier correction here claimed `peer` "is not a value this firmware prints" -- WRONG, withdrawn: `BtLink.h:101` lists five (`none|ssp|pin|stored|peer`) and RUN 9 measured BOTH.  `ssp` is set where WE drive the exchange to completion (`BtLink.cpp:704`, our `user_conf_req` accept -> `pairing_complete` -> `bond=saved`); `peer` at `BtLink.cpp:373`, where encryption came up and we never offered a key.  Step 7 is the first; a phone that has forgotten us and re-pairs over a bond we still hold is the second -- it issues NO `link_key_req` at all, goes straight to IO-cap, and the bond reads `bond=updated`.

- [ ] **Step 8: NEW-43, reproduced on purpose.** Disconnect; on the phone do NOT forget, on the board type `forget` (so the phone holds a stale key and the board none — the exact RUN 4 setup); connect from the phone → SSP fails (`pairing_complete: status=0x05`) → the attempt fails → **`pairing=on reason=drop`** with a fresh `ssp_mode: ... mode=1` → now Forget This Device on the phone and pair again: it must succeed **without SW4**. Before this branch that needed a reboot.

- [ ] **Step 9: Append RUN 9** to `transcript_hw_evkb.txt` in the established format (header with the verified LED level and each step's verdict; events complete; heartbeats every 30th), and commit.

---

### Task 7: Close-out

- [ ] **Step 1: Spec §7** written from RUN 9; **Step 2: CLAUDE.md** — a ✅ sweep entry and a ★ NEW-46 paragraph in the house style (the pairing window, the socket-console gate mechanism and its vacuity caveat, the NEW-43 heal path being the FAILED-attempt branch, the LED polarity as measured); **Step 3: memory** — `new46-pairing-mode.md` + the `MEMORY.md` line; **Step 4: Linear** — NEW-46 Done with a close-out comment; NEW-43 commented "mitigated by NEW-46's window — the fallback still writes mode 0 and should stop"; **Step 5:** ff-merge to master and push, re-running the sink gate and vacuity on the merged tree first if master moved.

---

## Self-review (done while writing)

**Spec coverage.** §3 decisions → T1 (policy), T2 (feedback), T3 (commands, tool), T4 (gate), T6 (bench). §4 API → T1 step 3 (with the five planning refinements: `canPair()`, DROP on the failed-attempt branch, close on STREAMING, `setPairingWindowMs(0)` semantics, the cmd-prints-its-own-edge rule). §5 sketch → T2/T3. §6.1 → T1 P1–P5 with mutants. §6.2's six assertions → T4 step 1 (assertion 5, close-as-paired, is asserted on the FIRST link, since the post-forget re-page reaches SSP but not STREAMING — recorded in the gate comment). §6.3 → T6. §6.4 → T5/T7. §8 deferred items untouched.

**Placeholders.** T3 step 3's smoke expectation deliberately admits two valid readings and says to record which; T7's docs are written from the bench and cannot be pre-written.

**Type consistency.** `PairingReason {PAIR_NONE, PAIR_BOOT, PAIR_DROP, PAIR_CMD}`, `PairingEnd {PAIR_END_NONE, PAIR_END_TIMEOUT, PAIR_END_PAIRED, PAIR_END_CANCELLED}`, `enterPairing(now, r)`, `canPair()`, `pairingOpen()`, `pairingRemainingMs(now)`, `pairingReason()`, `pairingEnd()`, `setPairingWindowMs(ms)`, `PAIR_DEFAULT_MS` are declared in T1 and used by those names in T1's tests, T2 and T3. `OP_SSP = 0x0C56` matches `BtLink.cpp`'s `OP_WRITE_SSP_MODE`. `PEER-SOURCE-DROP`, `PEER-SOURCE-REPAGE`, `DRIVER-SENT`, `DRIVER-STATUS-OK` are spelled identically in the peer, the driver and the gate. `$CON` is defined before use.
