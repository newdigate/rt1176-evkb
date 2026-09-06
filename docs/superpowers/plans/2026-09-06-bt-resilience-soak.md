# BT connection-resilience soak (NEW-34 piece 5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An unattended BT connection-resilience soak — the firmware forces a link drop every period, `BtSession` auto-reconnects, and a health signature proves no leak over hundreds of cycles — gated in QEMU (structural leak invariants over 10 rapid cycles) and run for 2–4 h on silicon against the ESP32 sink.

**Architecture:** An opt-in `M2_BT_SOAK` driver (~40 lines of example code on top of `BtSession`) issues a RAW `HCI_Disconnect` on the live handle — never the MANUAL `disconnect()` hook — so the loss takes the AUTOMATIC reconnect path (`BtLink` → `LINK_LOST` → `A2dpSource::tick()` teardown → `BtSession` WAITING → `retryNow()` → page with the stored key → STREAMING). A `bt_soak` line every 2 s carries the cycle counters and the structural baselines (`l2_free`, `handle`, `bonds`, heap, stack floor). The fake peer gains a `soak` phase (the `reconnect` phase's flow: host-sent Disconnect, fresh handle per page, stored-key auth — with no decoy, no key rejection, N cycles, per-link media validation). One new gate, `bt_tone_test[soak]`, sweep 131 → 132.

**Tech Stack:** M2Radio `bt/` (C++11, host-compiled tests under `bt/test/run.sh`), evkb Arduino example `examples/audio/bt_tone_test`, `hci_peer.py` (Python 3 fake controller), `tools/gate-lib.sh` / `tools/qrun`, `tools/gate-vacuity.test.sh`.

**Spec:** `docs/superpowers/specs/2026-09-06-bt-resilience-soak-design.md`. **Two corrections found while planning (apply to the spec in Task 6):** (a) `BondTable::count()` ALREADY EXISTS — only `L2cap::freeSlots()` is new; (b) the QEMU gate asserts that the credit STATS RESET per cycle (structural) plus `clamp=0` and HCI `starved=0` (both deterministic against the fake peer), NOT that `credmin` recovers — piece 4 established that QEMU credit DYNAMICS are timing noise, so credit recovery stays a silicon claim.

---

## Ground rules (same as pieces 1–4)

- Library = `~/Development/M2Radio` (pushed + pinned separately); firmware/docs = `~/Development/rt1170/evkb` on `master`. evkb resolves M2Radio local-first, so an M2Radio edit is live in the next evkb build.
- Commit with an explicit pathspec, never `git add -A`. End messages with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Every host-test pin shown RED by a mutant ON A SCRATCH COPY (`cp` the source, mutate, run, delete — never mutate the committed file), every gate assertion demonstrated RED by name and recorded in the gate header.
- Run a gate as `./run_qemu_X.sh`, never `sh`. One QEMU user at a time. Licence audit AFTER a sweep, never during.
- **Rebuild + freshness-check every bt-linking gate ELF before the sweep** (NEW-36 discipline): a gate does not build, and an ELF from the old library passes vacuously.

---

## Task 1: `L2cap::freeSlots()` — the slot-leak baseline (M2Radio)

**Files:**
- Modify: `~/Development/M2Radio/bt/L2cap.h` (one accessor, after `nextInbound`)
- Modify: `~/Development/M2Radio/bt/test/l2cap_test.cpp` (scenario A4, before the final `printf`)

**Context:** `L2cap` keeps `Channel m_ch[MAX_CHANNELS]` (5). A slot is REUSABLE when `state == FREE || state == CLOSED` (that is what `connect()` and the inbound accept path scan for, `L2cap.cpp:26,102`). `reset()` puts every slot back to FREE. The soak's "no slot leak" baseline is the reusable-slot count at each STREAMING entry.

- [ ] **Step 1: Write the failing test.** In `bt/test/l2cap_test.cpp`, immediately BEFORE the final `printf("l2cap_test: ...")` line, add:
```cpp
    {   // A4 (NEW-34 piece 5). freeSlots(): reusable (FREE or CLOSED) slots -- the soak's slot-leak baseline.
        // 5 at begin(); a connect() takes one; a CLOSED channel is reusable again; reset() restores all 5.
        CapIo io; L2cap l(io); l.begin(0x0001, 7);
        CHECK(l.freeSlots() == L2cap::MAX_CHANNELS);
        L2cap::Channel *ch = l.connect(0x0019, 0x0041);
        CHECK(ch != nullptr && l.freeSlots() == L2cap::MAX_CHANNELS - 1);
        ch->state = L2cap::CLOSED;                                              // a torn-down channel is reusable
        CHECK(l.freeSlots() == L2cap::MAX_CHANNELS);
        CHECK(l.connect(0x0019, 0x0042) != nullptr && l.freeSlots() == L2cap::MAX_CHANNELS - 1);
        l.reset();
        CHECK(l.freeSlots() == L2cap::MAX_CHANNELS);
    }
```

- [ ] **Step 2: Run it to verify it fails to COMPILE** (no `freeSlots`):
```bash
cd ~/Development/M2Radio && ./bt/test/run.sh 2>&1 | grep -E "l2cap_test|freeSlots|error" | head -3
```
Expected: a compile error naming `freeSlots`.

- [ ] **Step 3: Implement.** In `bt/L2cap.h`, after the `nextInbound(...)` declaration, add:
```cpp
    // Reusable channel slots (FREE or CLOSED -- what connect() and the inbound accept scan for).  NEW-34 piece 5's
    // slot-leak baseline: sampled at every STREAMING entry it must return to the same value cycle after cycle; a
    // channel never freed makes it decline.  reset() restores MAX_CHANNELS.
    uint8_t  freeSlots() const { uint8_t n = 0; for (const auto &ch : m_ch) if (ch.state == FREE || ch.state == CLOSED) n++; return n; }
```

- [ ] **Step 4: Run the suite** → `l2cap_test: N checks, 0 failures` (N = 73 + 6), `BT-HOST-TESTS: PASS`.

- [ ] **Step 5: Mutant (scratch copy).** `cp bt/L2cap.h /tmp/L2cap.h.orig`; change the body to `return MAX_CHANNELS;`; run → the `MAX_CHANNELS - 1` checks FAIL by line; `cp /tmp/L2cap.h.orig bt/L2cap.h`; run → green.

- [ ] **Step 6: Commit** (M2Radio): `git add bt/L2cap.h bt/test/l2cap_test.cpp && git commit -m "bt: L2cap::freeSlots() -- reusable-slot count, the NEW-34 piece 5 slot-leak baseline (host-tested, RED-pinned)"` + trailer.

---

## Task 2: the `M2_BT_SOAK` driver + `bt_soak` health line (evkb bt_tone_test)

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/CMakeLists.txt` (options, after the `M2_BT_ACCEPT_AVCTP` block)
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/bt_tone_test.cpp` (driver block after the globals at line ~255; `onStreamCb` hook; `loop()` tick + line)

**Context:** globals are `hci`, `src` (A2dpSource), `session` (BtSession), `bonds`, `btout`. `loop()` already calls `session.tick(millis()); src.service(); src.l2().tickClock(millis()); btout.poll();` then a 1 s heartbeat. `onStreamCb(streaming=true)` does `btout.begin(src); src.l2().resetCreditStats();`. `BtSession` exposes `state()`, `retryNow()`, `stats()`; `src.link().handle()` is the live ACL handle; `hci.submit(opcode, params, plen, done, ctx)` is the non-blocking command path.

- [ ] **Step 1: CMake options.** After the `M2_BT_ACCEPT_AVCTP` block add:
```cmake
# NEW-34 piece 5: the unattended connection-resilience soak driver (spec 2026-09-06-bt-resilience-soak-design.md).
# Every M2_BT_SOAK_PERIOD_MS while STREAMING the firmware forces a drop with a RAW HCI_Disconnect and lets BtSession
# auto-reconnect; M2_BT_SOAK_CYCLES bounds the run (0 = unlimited, the silicon soak; the [soak] gate sets 10).
# OFF by default so the default image and every existing gate stay byte-identical.
option(M2_BT_SOAK "Resilience soak: force a drop every period and count the auto-reconnects" OFF)
set(M2_BT_SOAK_PERIOD_MS "15000" CACHE STRING "Soak: ms of streaming between forced drops")
set(M2_BT_SOAK_CYCLES "0" CACHE STRING "Soak: stop after this many forced drops (0 = unlimited)")
set(M2_BT_SOAK_RECONNECT_BOUND_MS "30000" CACHE STRING "Soak: a cycle not re-streaming within this many ms is a failure")
if(M2_BT_SOAK)
    add_definitions(-DM2_BT_SOAK=1 -DM2_BT_SOAK_PERIOD_MS=${M2_BT_SOAK_PERIOD_MS}
                    -DM2_BT_SOAK_CYCLES=${M2_BT_SOAK_CYCLES} -DM2_BT_SOAK_RECONNECT_BOUND_MS=${M2_BT_SOAK_RECONNECT_BOUND_MS})
endif()
```

- [ ] **Step 2: the driver block.** In `bt_tone_test.cpp`, after `static AudioOutputBluetooth btout;` (line ~255) and BEFORE `onStreamCb`, add:
```cpp
#if defined(M2_BT_SOAK)
// --- NEW-34 piece 5: the unattended resilience soak driver ---------------------------------------
// Every M2_BT_SOAK_PERIOD_MS while STREAMING, force a drop with a RAW HCI_Disconnect (0x0406, reason 0x13)
// on the live handle -- deliberately NOT session.disconnect(): that is the MANUAL hook and BtSession would
// treat the loss as intended.  A raw disconnect arrives as Disconnection_Complete with BtLink outside its
// DISCONNECT op, so BtLink goes LINK_LOST, A2dpSource::tick() tears the media path down (m_avdtp.reset();
// m_l2.reset()), BtSession records the loss and goes WAITING -- the AUTOMATIC path a range loss triggers.
// retryNow() then fires the reconnect at once: the retry TIMER is [lifecycle]'s claim; this soak measures
// the reconnect MACHINERY (and the QEMU gate lives inside a 60 s budget).  A cycle that does not re-stream
// within M2_BT_SOAK_RECONNECT_BOUND_MS is COUNTED as a failure and the driver moves on -- a soak records
// failures, it does not stop on the first.
#include <malloc.h>
extern "C" char _ebss;
static uint32_t soakHeapUsed()     { struct mallinfo mi = mallinfo(); return (uint32_t)mi.uordblks; }   // newlib live total; the arena is OCRAM (.bss.dma), clear of the DTCM stack
static uint32_t soakStackFreeMin() { static uint32_t fl = 0xFFFFFFFF; register uint32_t sp __asm__("sp");
                                     uint32_t f = sp - (uint32_t)&_ebss; if (f < fl) fl = f; return fl; }   // running floor: stack grows down from DTCM top
static const uint16_t OP_DISCONNECT = 0x0406;
enum SoakState : uint8_t { SOAK_STREAM, SOAK_DROPPING, SOAK_RECONNECTING, SOAK_DONE };
static SoakState s_soakSt = SOAK_STREAM;
static uint32_t s_soakAt = 0;                    // period reference while streaming; bound reference while dropping/reconnecting
static uint32_t s_soakCycles = 0, s_soakReconnects = 0, s_soakFails = 0, s_soakReconnectMsMax = 0;
static bool     s_soakBaseSet = false;
static uint8_t  s_soakL2FreeBase = 0, s_soakL2FreeRestreamMin = 0xFF;   // the slot-leak baseline and the floor seen at re-stream entries
static void soakOnStream() {                     // from onStreamCb(streaming=true): sample the structural baseline
    uint8_t f = src.l2().freeSlots();
    if (!s_soakBaseSet) { s_soakL2FreeBase = f; s_soakBaseSet = true; }
    else if (f < s_soakL2FreeRestreamMin) s_soakL2FreeRestreamMin = f;
}
static void soakPrintDone() {
    uint8_t rm = s_soakBaseSet && s_soakL2FreeRestreamMin != 0xFF ? s_soakL2FreeRestreamMin : s_soakL2FreeBase;
    CONSOLE.print("soak_done cycles="); CONSOLE.print(s_soakCycles);
    CONSOLE.print(" reconnects="); CONSOLE.print(s_soakReconnects);
    CONSOLE.print(" fails="); CONSOLE.print(s_soakFails);
    CONSOLE.print(" reconnect_ms_max="); CONSOLE.print(s_soakReconnectMsMax);
    CONSOLE.print(" l2_free_base="); CONSOLE.print(s_soakL2FreeBase);
    CONSOLE.print(" l2_free_restream_min="); CONSOLE.print(rm);
    CONSOLE.print(" l2_leak="); CONSOLE.print(s_soakL2FreeBase > rm ? s_soakL2FreeBase - rm : 0);
    CONSOLE.print(" bonds="); CONSOLE.println(bonds.count());
}
static void soakTick(uint32_t now) {
    switch (s_soakSt) {
    case SOAK_STREAM:
        if (session.state() != BtSession::STREAMING) { s_soakAt = now; break; }        // not streaming yet: keep re-arming the period
        if (M2_BT_SOAK_CYCLES && s_soakCycles >= (uint32_t)M2_BT_SOAK_CYCLES) { s_soakSt = SOAK_DONE; soakPrintDone(); break; }
        if (now - s_soakAt >= (uint32_t)M2_BT_SOAK_PERIOD_MS) {
            uint16_t h = src.link().handle();
            uint8_t p[3] = { (uint8_t)h, (uint8_t)(h >> 8), 0x13 };                    // handle, reason 0x13 Remote User Terminated
            if (hci.submit(OP_DISCONNECT, p, 3, nullptr, nullptr) == Hci::OK) {
                s_soakCycles++; s_soakAt = now; s_soakSt = SOAK_DROPPING;
                CONSOLE.print("soak_drop cycle="); CONSOLE.print(s_soakCycles); CONSOLE.print(" handle=0x"); printHex16(h); CONSOLE.println();
            }
        }
        break;
    case SOAK_DROPPING:                                                                 // wait for BtSession to see the loss
        if (session.state() == BtSession::WAITING) { session.retryNow(); s_soakSt = SOAK_RECONNECTING; }
        else if (now - s_soakAt >= (uint32_t)M2_BT_SOAK_RECONNECT_BOUND_MS) {
            s_soakFails++; s_soakSt = SOAK_STREAM; s_soakAt = now; CONSOLE.println("soak_fail stage=drop-not-seen"); }
        break;
    case SOAK_RECONNECTING:
        if (session.state() == BtSession::STREAMING) {
            uint32_t ms = now - s_soakAt; if (ms > s_soakReconnectMsMax) s_soakReconnectMsMax = ms;
            s_soakReconnects++; s_soakSt = SOAK_STREAM; s_soakAt = now;
        } else if (now - s_soakAt >= (uint32_t)M2_BT_SOAK_RECONNECT_BOUND_MS) {
            s_soakFails++; s_soakSt = SOAK_STREAM; s_soakAt = now; CONSOLE.println("soak_fail stage=reconnect-timeout"); }
        break;
    case SOAK_DONE: break;
    }
}
static void soakPrintLine() {                    // every 2 s (NEW-8 cadence): the health signature
    CONSOLE.print("bt_soak cycles="); CONSOLE.print(s_soakCycles);
    CONSOLE.print(" reconnects="); CONSOLE.print(s_soakReconnects);
    CONSOLE.print(" fails="); CONSOLE.print(s_soakFails);
    CONSOLE.print(" reconnect_ms_max="); CONSOLE.print(s_soakReconnectMsMax);
    CONSOLE.print(" l2_free="); CONSOLE.print(src.l2().freeSlots());
    CONSOLE.print(" l2_free_base="); CONSOLE.print(s_soakL2FreeBase);
    CONSOLE.print(" handle=0x"); printHex16(src.link().handle());
    CONSOLE.print(" bonds="); CONSOLE.print(bonds.count());
    CONSOLE.print(" heap="); CONSOLE.print(soakHeapUsed());
    CONSOLE.print(" stack_free_min="); CONSOLE.println(soakStackFreeMin());
}
#endif /* M2_BT_SOAK */
```

- [ ] **Step 3: hook the streaming entry.** In `onStreamCb`, inside `if (streaming) {` right after `src.l2().resetCreditStats();`, add:
```cpp
#if defined(M2_BT_SOAK)
        soakOnStream();                // NEW-34 piece 5: sample the slot-leak baseline at every STREAMING entry
#endif
```

- [ ] **Step 4: tick + line in `loop()`.** After `btout.poll();` add:
```cpp
#if defined(M2_BT_SOAK)
    soakTick(millis());
    { static uint32_t lastSoak = 0; if (millis() - lastSoak >= 2000) { lastSoak = millis(); soakPrintLine(); } }
#endif
```
Also print the config once in `setup()`, right after the `bonds_boot=` line:
```cpp
#if defined(M2_BT_SOAK)
    CONSOLE.print("soak period_ms="); CONSOLE.print(M2_BT_SOAK_PERIOD_MS); CONSOLE.print(" cycles="); CONSOLE.print(M2_BT_SOAK_CYCLES);
    CONSOLE.print(" bound_ms="); CONSOLE.println(M2_BT_SOAK_RECONNECT_BOUND_MS);
#endif
```

- [ ] **Step 5: build both.** Default (must be byte-identical in behaviour; card-absent gate PASS) and the soak gate build:
```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake >/tmp/c.log 2>&1 && cmake --build build >/tmp/b.log 2>&1 && ./run_qemu.sh 2>&1 | tail -1
rm -rf build-soak && cmake -B build-soak -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_SOAK=ON -DM2_BT_SOAK_PERIOD_MS=2000 -DM2_BT_SOAK_CYCLES=10 -DM2_BT_SOAK_RECONNECT_BOUND_MS=8000 >/tmp/cs.log 2>&1 && cmake --build build-soak >/tmp/bs.log 2>&1 && strings build-soak/bt_tone_test.elf | grep -c "soak_done"
```
Expected: `PASS: ...` for the default; `1` for the soak build.

- [ ] **Step 6: Commit** (evkb): `git add examples/audio/bt_tone_test/bt_tone_test.cpp examples/audio/bt_tone_test/CMakeLists.txt && git commit -m "audio: bt_tone_test M2_BT_SOAK resilience-soak driver + bt_soak health line (NEW-34 piece 5)"` + trailer.

---

## Task 3: the `soak` phase in the fake peer (`hci_peer.py`)

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/networking/m2_hci_probe/hci_peer.py`

**Context:** The `reconnect` phase already models exactly the soak's flow — host-sent `Disconnect` (0x0406) answered with Command Status + Disconnection_Complete then `reset_link()`, a FRESH handle per `Create_Connection`, SSP on the first link (`keys[bd] = KEY1`), and stored-key verification (`key_ok`) on every re-page. `soak` = that flow with NO decoy, NO key rejection (`reject_on_link = 0` never matches), `N` cycles, and per-link media validation (lifecycle's `cur_media_cid` / `fresh_media(119)` pattern). Every edit below ADDS `"soak"` to an existing branch or adds a soak-only line; nothing existing changes behaviour.

- [ ] **Step 1: tables.** In `LAST_OPCODE` add `"soak": 0x0413`; in `DEADLINE` add `"soak": 55`. Add a module global after `DECOY_BD`: `SOAK_N = 10   # soak phase: forced drops the gate build performs (argv[3] overrides)`.

- [ ] **Step 2: `phase_done`.** Before the final `return peer.cmds.count(...)` add:
```python
    # soak's real end: N+1 links have STREAMED media (first link + one per forced drop) and N host Disconnects were seen.
    if phase == "soak":
        sk = peer.sk
        return sk["streamed"] >= sk["n"] + 1 and sk["disconnects"] >= sk["n"] and not peer.avdtp["error"] and peer.rc["errors"] == 0
```

- [ ] **Step 3: state.** In `Peer.__init__`, after the `self.lc = {...}` dict add:
```python
        # --- soak phase (NEW-34 piece 5): the reconnect flow N times over -- host-forced drops, fresh handle per page,
        # stored-key auth each re-page, media validated per link.  streamed counts a link once its media reached 5 packets.
        self.sk = {"n": SOAK_N, "disconnects": 0, "streamed": 0, "last_counted": 0}
        if phase == "soak": self.rc["reject_on_link"] = 0            # never reject a key: create_conns is >= 1 whenever a key is offered
```

- [ ] **Step 4: command handlers.** (a) In the `0x0405` Create_Connection branch change `if self.phase == "reconnect":` (the fresh-handle block) to `if self.phase in ("reconnect", "soak"):`. (b) In the `0x0406` Disconnect branch change `if self.phase == "reconnect" and h != self.cur_handle():` to `if self.phase in ("reconnect", "soak") and h != self.cur_handle():`, change the trailing `if self.phase == "reconnect":` (reset_link) to `if self.phase in ("reconnect", "soak"):`, and add after it: `if self.phase == "soak": self.sk["disconnects"] += 1`.

- [ ] **Step 5: ACL paths.** (a) In `feed()` change the NCP gate tuple `("reconnect", "lifecycle")` to `("reconnect", "lifecycle", "soak")`. (b) In `handle_acl()`: the bad-handle check tuple `("reconnect", "lifecycle")` → add `"soak"`; the media-channel CONN_REQ branch `elif psm == 0x0019 and self.phase == "lifecycle":` → `elif psm == 0x0019 and self.phase in ("lifecycle", "soak"):`; the data routing `elif psm == 0x0019 and self.phase == "lifecycle":` → `elif psm == 0x0019 and self.phase in ("lifecycle", "soak"):`.

- [ ] **Step 6: run loop + tally + argv.** In the main loop, after the `if phase == "lifecycle": ...` block add:
```python
        if phase == "soak":
            sk = peer.sk
            if peer.media["pkts"] >= 5 and sk["last_counted"] != peer.rc["create_conns"]:   # this link has streamed: count it once
                sk["streamed"] += 1; sk["last_counted"] = peer.rc["create_conns"]
            if phase_done("soak", peer) and not peer.pending:
                break                                                                          # the gate waits out the host's soak_done line
```
In the tally section, after the lifecycle print add:
```python
    if phase == "soak":
        r, sk = peer.rc, peer.sk
        print("PEER-SOAK links=%d disconnects=%d streamed=%d key_ok=%d notified=%d"
              % (r["create_conns"], sk["disconnects"], sk["streamed"], r["key_ok"], r["notified"]))
```
Where `phase` and the socket path are read from `sys.argv`, add: `if phase == "soak" and len(sys.argv) > 3: SOAK_N = int(sys.argv[3])` (and set `peer.sk["n"] = SOAK_N` after the Peer is constructed if the global is read before argv parsing — check the construction order and place the override so `sk["n"]` is the argv value).

- [ ] **Step 7: syntax check + no regression.** `python3 -m py_compile hci_peer.py`; then run the two existing peer gates that share these branches: `cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test && ./run_qemu_lifecycle.sh 2>&1 | tail -1` and `cd ../../networking/m2_hci_probe && ./run_qemu_reconnect.sh 2>&1 | tail -1` → both `PASS`.

- [ ] **Step 8: Commit** (evkb): `git add examples/networking/m2_hci_probe/hci_peer.py && git commit -m "hci_peer: soak phase -- N host-forced drops, fresh handle per page, stored-key auth, media validated per link (NEW-34 piece 5)"` + trailer.

---

## Task 4: the `bt_tone_test[soak]` gate + RED demonstrations + fixture

**Files:**
- Create: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/run_qemu_soak.sh` (mode 755)
- Create: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/transcript_qemu_soak.txt`

**Context:** Model on `run_qemu_lifecycle.sh` (same `gate-lib` lifecycle, own `build-soak/`, peer in the foreground, wait for the LAST line parsed, UART positives then peer tripwires then the peer tally then infrastructure last). N = 10 cycles at a 2 s period ≈ 30 s + boot/inquiry ≈ 35–40 s, under the peer's 55 s deadline and qrun's 60 s.

- [ ] **Step 1: the script.** Write `run_qemu_soak.sh`:
```sh
#!/bin/sh
# run_qemu_soak.sh -- the [soak] gate for bt_tone_test (NEW-34 piece 5): the A2DP link SURVIVES REPETITION.
#
# WHAT THIS PROVES
#   Built -DM2_BT_SOAK=ON -DM2_BT_SOAK_PERIOD_MS=2000 -DM2_BT_SOAK_CYCLES=10 -DM2_BT_SOAK_RECONNECT_BOUND_MS=8000
#   -DM2_BT_TARGET_NAME=FAKE-HEADSET-01, the firmware streams, then TEN times forces a drop with a RAW HCI_Disconnect
#   on the live handle and lets BtSession auto-reconnect (page + STORED key + L2CAP + AVDTP + resume) against
#   hci_peer.py's `soak` phase.  The STRUCTURAL leak invariants a soak exists to catch, invisible to [lifecycle]'s
#   three legs but fatal over hundreds of cycles:
#     every cycle reconnects   soak_done cycles=10 reconnects=10 fails=0, PEER-counted links=11 disconnects=10 streamed=11
#     no slot leak             l2_leak=0 -- the reusable L2CAP slot count at every re-stream equals the first stream's
#     no handle leak           PEER-ACL-BAD-HANDLE / PEER-DISCONNECT-BAD-HANDLE never fire (fresh handle per link)
#     no bond churn            bonds=1 after ten re-pages (a stored key re-used, never re-paired: key_ok=10 notified=1)
#     credit stats reset       the final bt_cred sent= is a fraction of the run's total packets= (per-attempt reset)
#     no HCI credit leak       bt_hci starved=0 and bt_cred clamp=0 -- deterministic against this peer (1 NCP per packet)
#   NOT asserted: credmin / starves / drops -- QEMU credit DYNAMICS are timing noise (piece 4); heap-over-hours,
#   reconnect latency and RF are the SILICON soak's claims (transcript_hw_evkb.txt, SOAK section).
#
# DEMONSTRATED RED (fill in the dates/outputs when run; each mutation in the named COMMITTED file, rebuilt, run, reverted):
#   (1) M2Radio bt/A2dpSource.cpp tick() loss teardown: `m_l2.reset()` removed -> FAIL: [soak] L2CAP slot leak (l2_leak=...)
#   (2) M2Radio bt/BondTable.cpp upsert(): the update-in-place path disabled (always insert) -> FAIL: [soak] bond churn (bonds=4)
#   (3) evkb bt_tone_test.cpp soakTick(): `uint16_t h = src.link().handle();` -> `= 0x0001;` (a stale handle) ->
#       FAIL: [soak] not every cycle reconnected (the peer refuses the stale handle: PEER-DISCONNECT-BAD-HANDLE, cycles stall)
#   (4) evkb bt_tone_test.cpp onStreamCb(): `src.l2().resetCreditStats();` removed -> FAIL: [soak] credit stats not reset per cycle
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || { echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board))"; exit 1; }
fail() { echo "FAIL: $*"; exit 1; }

BUILD_DIR="$DIR/build-soak"; ELF="$BUILD_DIR/bt_tone_test.elf"
if [ "${GATE_VACUITY:-}" = "1" ] && [ -x "$ELF" ]; then :; else
    mkdir -p "$BUILD_DIR"; CONFIGURE_RC=0
    cmake -S "$DIR" -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE="$EVKB/toolchain/rt1170-evkb.toolchain.cmake" \
          -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_SOAK=ON -DM2_BT_SOAK_PERIOD_MS=2000 -DM2_BT_SOAK_CYCLES=10 \
          -DM2_BT_SOAK_RECONNECT_BOUND_MS=8000 >"$BUILD_DIR/configure.log" 2>&1 || CONFIGURE_RC=$?
    BUILD_RC=0; [ "$CONFIGURE_RC" -eq 0 ] && { cmake --build "$BUILD_DIR" >"$BUILD_DIR/build.log" 2>&1 || BUILD_RC=$?; }
    [ "$CONFIGURE_RC" -eq 0 ] && [ "$BUILD_RC" -eq 0 ] || fail "build-soak/ did not build -- see build-soak/configure.log / build.log"
fi

OUT="$BUILD_DIR/soak.uart"; DBG="$BUILD_DIR/soak.dbg"; RES="$BUILD_DIR/soak.peer"
rm -f "$OUT" "$DBG" "$RES"
SOCK="/tmp/m2soak_$$.sock"; rm -f "$SOCK"; gate_tmp "$SOCK"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" -display none $(gate_console "$OUT") \
    -serial unix:"$SOCK",server -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
PEER_RC=0
python3 "$EVKB/examples/networking/m2_hci_probe/hci_peer.py" soak "$SOCK" 10 > "$RES" 2>&1 || PEER_RC=$?
# Wait for the LAST line parsed -- the firmware's soak_done -- never an earlier one.
LOOP=240; [ "$PEER_RC" -eq 0 ] || LOOP=40
for _ in $(seq 1 $LOOP); do
    [ -f "$OUT" ] && grep -qE "^soak_done " "$OUT" 2>/dev/null && [ -f "$RES" ] && grep -q "^PEER-SOAK " "$RES" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT" "soak phase"
echo "==== captured UART ===="; cat "$OUT"; echo "==== peer ===="; cat "$RES"

grep -q "RT1176 BT tone test up" "$OUT"                          || fail "[soak] banner missing"
grep -q "^soak period_ms=2000 cycles=10 bound_ms=8000" "$OUT"    || fail "[soak] soak config line missing (not a soak build?)"
grep -q "^streaming by=inquiry " "$OUT"                          || fail "[soak] never streamed (first link by inquiry missing)"
grep -qE "^soak_done cycles=10 " "$OUT"                          || fail "[soak] soak_done never printed with cycles=10"
DONE=$(grep -m1 "^soak_done " "$OUT")
echo "$DONE" | grep -q " reconnects=10 fails=0 "                 || fail "[soak] not every cycle reconnected: $DONE"
echo "$DONE" | grep -q " l2_leak=0 "                             || fail "[soak] L2CAP slot leak: $DONE"
echo "$DONE" | grep -q " bonds=1"                                || fail "[soak] bond churn (expected bonds=1 after ten stored-key re-pages): $DONE"
echo "$DONE" | grep -qE " reconnect_ms_max=[1-9]"                || fail "[soak] reconnect_ms_max never populated: $DONE"
grep -q "^soak_fail " "$OUT" && fail "[soak] a cycle failed: $(grep -m1 '^soak_fail ' "$OUT")"
grep -qE "^bt_link links=11 lost=10 " "$OUT"                     || fail "[soak] final bt_link wrong (expected links=11 lost=10)"
# credit stats RESET per cycle: the last bt_cred sent= (since the last STREAMING entry) must be a fraction of the run's
# cumulative packets=.  Without the per-attempt reset sent tracks packets.  Structural, not a credit-dynamics claim.
PK=$(grep "^hb streaming=" "$OUT" | tail -1 | sed -E 's/.* packets=([0-9]+).*/\1/'); SENT=$(grep "^bt_cred " "$OUT" | tail -1 | sed -E 's/.* sent=([0-9]+).*/\1/')
[ -n "$PK" ] && [ -n "$SENT" ] && [ "$PK" -gt 0 ] && [ $((SENT * 2)) -lt "$PK" ] || fail "[soak] credit stats not reset per cycle (final sent=$SENT vs cumulative packets=$PK)"
grep "^bt_hci " "$OUT" | tail -1 | grep -q " starved=0 "          || fail "[soak] HCI command-credit starvation: $(grep '^bt_hci ' "$OUT" | tail -1)"
grep "^bt_cred " "$OUT" | tail -1 | grep -q " clamp=0"            || fail "[soak] NCP clamp hit (controller double-count?): $(grep '^bt_cred ' "$OUT" | tail -1)"

for t in PEER-ACL-BAD-HANDLE PEER-ACL-UNKNOWN-CID PEER-DISCONNECT-BAD-HANDLE PEER-KEY-MISMATCH PEER-AUTH-NO-LINK PEER-EXCEPTION; do
    grep -q "^$t" "$RES" && fail "[soak] peer tripwire: $(grep -m1 "^$t" "$RES")"
done
grep -q "^PEER-CONNECTED phase=soak" "$RES"                       || fail "[soak] the fake controller never attached to LPUART2"
grep -q "^PEER-SOAK links=11 disconnects=10 streamed=11 key_ok=10 notified=1" "$RES" \
    || fail "[soak] peer tally wrong: $(grep -m1 PEER-SOAK "$RES")"
grep -q "^PEER-EOF" "$RES"      && fail "[soak] QEMU closed the socket under the peer -- infrastructure: $(grep -m1 '^PEER-EOF' "$RES")"
grep -q "^PEER-DEADLINE" "$RES" && fail "[soak] the peer gave up at its deadline (sweep load?): $(grep -m1 '^PEER-DEADLINE' "$RES")"
grep -q "^Traceback (most recent call last)" "$RES" && fail "[soak] the fake controller crashed: $(grep -A3 '^Traceback' "$RES" | tail -1)"
[ "$PEER_RC" -eq 0 ] || fail "[soak] peer exited $PEER_RC"
echo "PASS: A2DP link survives repetition -- 10 forced drops, 10 auto-reconnects on fresh handles with the stored key, no slot/handle/bond leak, credit stats reset per cycle"
```
`chmod 755 run_qemu_soak.sh`.

- [ ] **Step 2: run it green.** `./run_qemu_soak.sh 2>&1 | tail -3` → PASS. If the peer hits its deadline under a clean machine, lower `M2_BT_SOAK_CYCLES`/N to 8 in BOTH the cmake line and the `hci_peer.py soak "$SOCK" 8` call and the tally expectations (links=9 disconnects=8 streamed=9 key_ok=8; bt_link links=9 lost=8; soak_done cycles=8 reconnects=8) — keep the two in lockstep. Check the wall time (`time ./run_qemu_soak.sh`) stays ≤ 45 s.

- [ ] **Step 3: the four RED demonstrations**, each: mutate the COMMITTED file, rebuild `build-soak/` (`cmake --build build-soak`), run, confirm the NAMED failure, `git -C <repo> checkout -- <file>`, rebuild, confirm GREEN. Record each output line in the script header's DEMONSTRATED RED block (replace "fill in").
  1. `~/Development/M2Radio/bt/A2dpSource.cpp` line ~74: `m_link.ackLost(); m_avdtp.reset(); m_l2.reset();` → drop `m_l2.reset();` → expect `FAIL: [soak] L2CAP slot leak` (or `PEER-ACL-BAD-HANDLE` — record which fires first).
  2. `~/Development/M2Radio/bt/BondTable.cpp` `upsert()`: make the existing-entry branch fall through to insert → expect `FAIL: [soak] bond churn (... bonds=4 ...)`.
  3. `bt_tone_test.cpp` `soakTick()`: `uint16_t h = src.link().handle();` → `uint16_t h = 0x0001;` → expect `FAIL: [soak] not every cycle reconnected` (with `PEER-DISCONNECT-BAD-HANDLE` in the peer log from cycle 2).
  4. `bt_tone_test.cpp` `onStreamCb()`: remove `src.l2().resetCreditStats();` → expect `FAIL: [soak] credit stats not reset per cycle`.

- [ ] **Step 4: fixture.** `cp build-soak/soak.uart transcript_qemu_soak.txt`.

- [ ] **Step 5: Commit** (evkb): `git add examples/audio/bt_tone_test/run_qemu_soak.sh examples/audio/bt_tone_test/transcript_qemu_soak.txt && git commit -m "audio: bt_tone_test[soak] gate -- 10 forced drops / 10 auto-reconnects, structural leak invariants, RED four ways (NEW-34 piece 5)"` + trailer.

---

## Task 5: vacuity negatives for `[soak]`

**Files:**
- Modify: `~/Development/rt1170/evkb/tools/gate-vacuity.test.sh` (a new item 12 after the lifecycle block, same `$bt_rel`)

- [ ] **Step 1: add three cases** after item 11's closing `fi`:
```sh
# --- 12. bt_tone_test[soak] (NEW-34 piece 5) --------------------------------
# run_qemu_soak.sh asserts the link survives repetition. Three negatives, each failing BY NAME: an absent
# capture (never streamed), the fixture's soak_done rewritten to fails=1 (a cycle that did not reconnect
# must not read as success), and the fixture's l2_leak rewritten to 1 (a slot leak must be named).
bt_sk_elf="$EVKB/$bt_rel/build-soak/bt_tone_test.elf"
if [ ! -x "$bt_sk_elf" ]; then
    echo "SKIP: soak vacuity cases (no build-soak/bt_tone_test.elf -- build it first)"
else
    cat > "$WORK/bt_sk_absent.txt" <<'ABSENT'
RT1176 BT tone test up
serial2=up_115200
hci_reset=timeout reason=no_response attempts=10 timeouts=10 framing=0 starved=0 qfull=0 late=0
bonds_boot=0
soak period_ms=2000 cycles=10 bound_ms=8000
a2dp=deferred (no HCI: card absent)
hb streaming=0 blocks=0 packets=0 drops=0 hw=0
ABSENT
    export GATE_VACUITY=1; run_gate "$bt_rel" "run_qemu_soak.sh" "$WORK/bt_sk_absent.txt"; rc=$?; unset GATE_VACUITY
    result=0; [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "\[soak\] never streamed" || result=1
    report "absent_capture_fails_soak_gate" $result

    sed 's/^\(soak_done cycles=10 reconnects=\)10 fails=0 /\19 fails=1 /' "$EVKB/$bt_rel/transcript_qemu_soak.txt" > "$WORK/bt_sk_fail.txt"
    export GATE_VACUITY=1; run_gate "$bt_rel" "run_qemu_soak.sh" "$WORK/bt_sk_fail.txt"; rc=$?; unset GATE_VACUITY
    result=0; [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "\[soak\] not every cycle reconnected" || result=1
    report "failed_cycle_fixture_fails_soak_gate" $result

    sed 's/ l2_leak=0 / l2_leak=1 /' "$EVKB/$bt_rel/transcript_qemu_soak.txt" > "$WORK/bt_sk_leak.txt"
    export GATE_VACUITY=1; run_gate "$bt_rel" "run_qemu_soak.sh" "$WORK/bt_sk_leak.txt"; rc=$?; unset GATE_VACUITY
    result=0; [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "\[soak\] L2CAP slot leak" || result=1
    report "slot_leak_fixture_fails_soak_gate" $result
    rm -f "$EVKB/$bt_rel"/build-soak/soak.uart "$EVKB/$bt_rel"/build-soak/soak.peer "$EVKB/$bt_rel"/build-soak/soak.dbg
fi
```
- [ ] **Step 2: run the suite** → the three new cases `PASS:` and the total goes 38 → 41 (count `grep -c "^PASS:"` on a live run; a `FAIL:` on an existing case means a fixture went stale — see Task 6 Step 5).
- [ ] **Step 3: Commit** (evkb): `git add tools/gate-vacuity.test.sh && git commit -m "tools: gate-vacuity -- three [soak] negatives (absent, failed cycle, slot leak), by name (NEW-34 piece 5)"` + trailer.

---

## Task 6: close-out (software)

- [ ] **Step 1: push M2Radio + pin.** `cd ~/Development/M2Radio && git push origin master`; bump the M2Radio SHA in `~/Development/rt1170/evkb/evkb.cmake` (the single `M2Radio` pin; add a trailing-comment note "NEW-34 piece 5 -- L2cap::freeSlots()").
- [ ] **Step 2: fresh-user verify by RUNNING the new gate against the fetched ELF**: in `bt_tone_test`, `cmake -B build-fetch -DEVKB_FORCE_FETCH=ON <the [soak] -D set>`, build, confirm `fetching ... M2Radio @ <new sha>` in the configure log, then `mv build-soak build-soak.local && ln -s build-fetch build-soak && ./run_qemu_soak.sh` → PASS; restore (`rm build-soak && mv build-soak.local build-soak`), delete `build-fetch`.
- [ ] **Step 3: rebuild every bt-linking gate ELF fresh** (`bt_tone_test/{build,build-media,build-lifecycle,build-soak}`, `m2_hci_probe/{build,build-avdtp,build-reconnect}`, `acid_box/build` default) and freshness-check by symbol: `nm <elf> | grep -c freeSlots` must be ≥ 1 for every A2dpSource-linking ELF (the accessor is inline; check `soakOnStream` in build-soak and `_ZN5L2cap9freeSlotsEv` or an `l2cap` object mtime newer than the M2Radio commit — use the mtime of `L2cap.cpp.obj` vs `git -C ~/Development/M2Radio log -1 --format=%ct`).
- [ ] **Step 4: sweep.** `./tools/run-all-qemu-gates.sh -l | tail -1` → 132; then the full sweep, one at a time, output captured. Target `132 passed, 0 failed, 0 SKIP`; any red is re-run idle before it is believed (the load-sensitivity class), and `[soak]` is expected to be in that class (peer/socket timing).
- [ ] **Step 5: fixture hygiene.** The committed `transcript_qemu_media.txt` and `transcript_qemu_lifecycle.txt` predate piece 4's `bt_cred` line (checked: the lifecycle fixture has no `bt_cred`). Re-capture both from their green gate runs (`cp build-media/media.uart transcript_qemu_media.txt`; `cp build-lifecycle/lifecycle.uart transcript_qemu_lifecycle.txt` — confirm the capture filenames in each script), re-run `tools/gate-vacuity.test.sh` → 41/41.
- [ ] **Step 6: licence audit** (AFTER the sweep): `LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh 2>&1 | tail -2` → `LICENSE-AUDIT: PASS`.
- [ ] **Step 7: docs.** CLAUDE.md: a new measured block above the piece-4 one (132 gates; the [soak] gate; the four RED demos; what is and is not asserted; the fixture re-capture), the "131 gates" narrative/target/checked-value lines → 132 (search `131`), the count-chain sentence. Spec: status → IMPLEMENTED (gate), silicon soak PENDING; apply the two corrections from the header (BondTable::count() pre-existed; credit-stats-reset vs credit-recovery). Memory `new34-bt-reconnect.md` + `MEMORY.md`.
- [ ] **Step 8: commit** (`evkb.cmake CLAUDE.md docs/superpowers/specs/2026-09-06-bt-resilience-soak-design.md examples/audio/bt_tone_test/transcript_qemu_media.txt examples/audio/bt_tone_test/transcript_qemu_lifecycle.txt`) with a message naming the sweep number, the pin and the audit; then finish per `superpowers:finishing-a-development-branch` (the push decision; the work is on master like pieces 1–4).

---

## Task 7: the silicon soak (USER, bench) — the piece's actual claim

**Files (evidence):** `examples/audio/bt_tone_test/transcript_hw_evkb.txt` (a `SOAK` section).

- [ ] Build `bt_tone_test` with `-DM2_BT_SOAK=ON -DM2_BT_SOAK_PERIOD_MS=15000 -DM2_BT_SOAK_CYCLES=0 -DM2_BT_TARGET_NAME=EVKB-SINK` (+ the bench's usual `M2_BT_ASSERT_CTS=ON`, `M2_BT_LEGACY_PIN=ON` for the ESP32, the real firmware blobs). Flash with the VCOM detached (`LinkServer flash … load` → `verify`), attach `tools/rt1170-console.py <port> 115200 | tee soak.log`, free-run with SW4.
- [ ] Run **2–4 hours** (500–1000 cycles at 15 s). Do not touch the bench; no reset loops.
- [ ] **Verdict (spec §2/§4):** over the whole log, `heap=`, `stack_free_min=`, `l2_free=` (equal to `l2_free_base=` on every line printed while streaming), `bonds=1` hold FLAT; `reconnects == cycles` on the last line with `fails=0`; `bt_cred starves`/`clamp` and `bt_hci starved` stay 0 (here the credit DYNAMICS are real: also record `credmin` and `starve_max_ms` — a lost-NCP leak shows as credmin pinned at 0 with starve_max_ms growing, piece 4's fingerprint); `reconnect_ms_max` recorded; the tone audible after reconnects; the ESP32's play state tracking the cycles. Paste the first and last `bt_soak`/`bt_cred`/`bt_link` lines, the cycle tally and any `soak_fail` lines into the transcript's `SOAK` section.
- [ ] A CLEAN run closes piece 5 AND settles the pending silicon claims of pieces 1 (stored-key reconnect on a real link), 2 (the reconnect machinery), and 4 (no residual credit leak). A LEAK or drift is filed against the piece it belongs to (memory → NEW-34 follow-up; credit → piece 4's escalation).

---

## Self-review (completed by the plan author)

- **Spec coverage:** §2 driver → Task 2 (raw 0x0406, never `disconnect()`; `retryNow()`; bounded cycle; `bt_soak` line with the baselines); §3 gate → Tasks 3–4 (every-cycle-reconnects peer-counted, slot leak, handle leak, bond churn, credit-stats reset + clamp/starved) + Task 5 vacuity; §4 silicon → Task 7; §5 verification → Tasks 1 (host test), 4 (RED ×4), 6 (sweep/audit/fresh-user/freshness); §6 non-goals honoured (no ESP32-induced drops, no acid_box requirement, only `freeSlots()` added). Two spec corrections recorded and scheduled (Task 6 Step 7).
- **Placeholder scan:** the only deferred text is the RED-demo output lines in the gate header ("fill in when run"), which are measurements, not design — the expected failure NAMES are given.
- **Type consistency:** `L2cap::freeSlots()` (Tasks 1, 2, 4); `soakTick`/`soakOnStream`/`soakPrintDone`/`soakPrintLine` (Task 2 only); `peer.sk` fields `n/disconnects/streamed/last_counted` (Task 3, tally, phase_done); `soak_done` fields `cycles/reconnects/fails/reconnect_ms_max/l2_free_base/l2_free_restream_min/l2_leak/bonds` (Task 2 prints them, Task 4 greps them, Task 5 rewrites them); `PEER-SOAK links=11 disconnects=10 streamed=11 key_ok=10 notified=1` (Task 3 prints, Task 4 asserts). N=10 appears in the cmake line, the peer argv, and every tally — Step 2 of Task 4 says how to move them together.
