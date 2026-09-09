# A2DP Sink Jitter Absorption (NEW-42) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `examples/audio/bt_sink_test` absorb an iPhone's A2DP delivery jitter — `over = 0`, `under ≤ reprimes + 2` on a 10-min bench run — by deepening the PCM ring, pre-filling it at START, re-priming it when it runs dry, and instrumenting the phone's inter-packet gaps; plus fix `Avrcp::unsupported()` miscounting answered commands.

**Architecture:** All of the jitter work is example-local: `AudioInputBluetooth` (the RTP/SBC → PCM-ring → SAI node) gains compile-time `RING`/`TARGET`/`PREFILL` configuration, a priming state in `update()`, and cumulative gap/fill/trim instruments; `servo.h` is untouched. The QEMU gate (139, extended in place) asserts the ARRIVAL-side instrument against the fake peer's known pacing plus one scripted gap; the CONSUME-side numbers (`reprimes`, fill extremes, `prime_ms`) are silicon claims measured by a controlled A/B bench session. The one M2Radio change is a three-way answer classification in `Avrcp`.

**Tech Stack:** C++11 (host tests under clang ASan/UBSan, ARM GCC 10 on target), CMake, POSIX sh gates, Python 3 fake HCI peer, LinkServer + iPhone for the bench.

**Spec:** `docs/superpowers/specs/2026-09-09-bt-sink-jitter-absorption-design.md` (commit `4cd4552`). Read §1–§5 before starting; every number below traces to it.

---

## Conventions every task relies on

- **Paths.** `$E` = `/Users/nicholasnewdigate/Development/rt1170/evkb`, `$S` = `$E/examples/audio/bt_sink_test`, `$M` = `$HOME/Development/M2Radio` (the sibling checkout `evkb.cmake` resolves local-first). Use absolute paths in every command — the harness resets the shell cwd between calls.
- **Host tests.** `$S/tests/run.sh` builds and runs `servo_test` and `node_test`; expected last lines are `servo_test: 113 checks, 0 failures` and `node_test: N checks, 0 failures` (N grows as cases are added — the number in each step is the expectation at that step). It aborts on the first failing binary (`set -e`).
- **Gate.** `cd $S && ./run_qemu.sh` (never `sh run_qemu.sh`). It rebuilds `build/` itself, so no separate `cmake --build` is needed before running it. It prints `PASS: A2DP SINK ...` on success. Runs ~30 s.
- **Commits.** One per task, on branch `new-42-sink-jitter` in `$E` (Task 0). Every commit message ends with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- **RED first.** Where a step says "run to verify it fails", the failing line's text is given; if it does not fail in that way, stop and find out why before implementing.
- **Do not run the full sweep or the licence audit concurrently with anything else** (CLAUDE.md: the load-sensitivity class). Task 8 sequences them.

---

### Task 0: Branch

**Files:** none

- [ ] **Step 1: Branch in place (NOT a worktree)**

A worktree would carry none of the 139 built gate images (they live in this checkout's `build*/` dirs and gates do not build), and a longer checkout path would trip the 104-byte `sun_path` limit four gates already hit (CLAUDE.md). Branch here:

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git status --short && git checkout -b new-42-sink-jitter && git log --oneline -1
```

Expected: `git status --short` prints nothing (clean), then `Switched to a new branch 'new-42-sink-jitter'`, then `4cd4552 docs: NEW-42 spec ...`.

- [ ] **Step 2: Confirm the host suite and the gate are green before touching anything**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./tests/run.sh 2>&1 | tail -3
```

Expected: `servo_test: 113 checks, 0 failures` and `node_test: 100 checks, 0 failures` (the exact node count may differ by a few; `0 failures` is the requirement).

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./run_qemu.sh 2>&1 | tail -1
```

Expected: a line starting `PASS: A2DP SINK`.

---

### Task 1: Compile-time `RING` / `TARGET` / `PREFILL` configuration and the headroom test

The ring goes 16 → 32 and the target 8 → 16, both overridable from CMake so the bench can build the control arm from the same source. `setPrefill()` is introduced here as the STORED flag (its behaviour is Task 3) so that every existing node_test case can opt out of the START pre-fill explicitly and keep its pop-on-first-update semantics.

**Files:**
- Modify: `examples/audio/bt_sink_test/AudioInputBluetooth.h`
- Modify: `examples/audio/bt_sink_test/tests/node_test.cpp`
- Modify: `examples/audio/bt_sink_test/tests/run.sh`
- Modify: `examples/audio/bt_sink_test/CMakeLists.txt`

- [ ] **Step 1: Add the failing headroom case to node_test**

Append this case immediately before the final `printf("node_test: ...` line in `$S/tests/node_test.cpp`:

```cpp
    {   // 14. HEADROOM (NEW-42).  A real source delivers EIGHT frames per RTP packet (iPhone bench 2026-09-09:
        //     frames/pkts = 7.97), so three back-to-back packets are 24 blocks.  The DEFAULT ring must swallow
        //     them from empty without a single drop; the bench's CONTROL arm (RING 16, built by run.sh with
        //     -DNODE_TEST_CONTROL_ARM) must drop exactly 24 - (RING - 1) = 9, which is the arithmetic that made
        //     the old ring overrun.  The expectation is computed from RING so the case is honest in both builds.
        //     RED at RING 16 in the default build: overruns() == 9, not 0.
        Node nd; nd->setPrefill(false); nd->begin();
        std::vector<uint8_t> f = encodeFrame(9000), pl;
        for (int i = 0; i < 8; i++) pl.insert(pl.end(), f.begin(), f.end());
        for (uint16_t s = 1; s <= 3; s++) feed(nd, rtp(s, 0x08, pl.data(), pl.size()));
        const uint32_t usable = AudioInputBluetooth::RING - 1;                   // one slot is the SPSC full/empty sentinel
        const uint32_t expect = usable >= 24 ? 0 : 24 - usable;
        CHECK(nd->overruns() == expect);
        CHECK(nd->frames() == 24 - expect);
#if !defined(NODE_TEST_CONTROL_ARM)
        CHECK(nd->overruns() == 0);                                              // the DEFAULT configuration must not drop
        CHECK(AudioInputBluetooth::RING >= 32 && AudioInputBluetooth::TARGET >= 16);
#else
        CHECK(AudioInputBluetooth::RING == 16 && AudioInputBluetooth::TARGET == 8 && !AudioInputBluetooth::PREFILL);
#endif
    }
```

- [ ] **Step 2: Give every existing case an explicit `setPrefill(false)`**

The pre-fill (Task 3) changes what the first `update()` does; cases 1–10 exercise the ring WITHOUT it and say so. One substitution:

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && sed -i '' 's/Node nd; nd->begin();/Node nd; nd->setPrefill(false); nd->begin();/g' tests/node_test.cpp && grep -c 'setPrefill(false); nd->begin()' tests/node_test.cpp
```

Expected: `11` (cases 1–10 plus the new case 14, which already had it).

Then add this paragraph to the `Node` struct comment (after the sentence ending `...previous case's history.`):

```cpp
// Cases 1-10 call setPrefill(false) before begin(): they exercise the ring, the parser and the servo WITHOUT the
// START pre-fill (NEW-42), whose behaviour has its own cases (12, 13).  With the pre-fill on, update() holds
// silence until TARGET blocks are buffered, and "feed one frame, update once, it comes out" would be false.
```

- [ ] **Step 3: Make cases 4, 8 and 9 symbolic in RING/TARGET**

In `$S/tests/node_test.cpp`, case 4 (`RING OVERRUN drops the NEW frame`): replace the body's literals —

```cpp
        for (int i = 0; i < AudioInputBluetooth::RING - 1; i++) feed(nd, onePacket((uint16_t)(i + 1), loud));
        CHECK(nd->fill() == AudioInputBluetooth::RING - 1); CHECK(nd->frames() == AudioInputBluetooth::RING - 1); CHECK(nd->overruns() == 0);
        feed(nd, onePacket(AudioInputBluetooth::RING, quiet));                  // the ring is full: this one is dropped
        CHECK(nd->overruns() == 1);
        CHECK(nd->fill() == AudioInputBluetooth::RING - 1);                     // unchanged
        CHECK(nd->frames() == AudioInputBluetooth::RING - 1);                   // it never became a frame
        for (int i = 0; i < AudioInputBluetooth::RING - 1; i++) nd->update();
```

and change its comment's last sentence from `RING is 16, so fill saturates at 15.` to `fill saturates at RING - 1 (one slot is the SPSC sentinel).`

Cases 8 and 9: replace both occurrences of `while (nd->fill() < 12)` with `while (nd->fill() < AudioInputBluetooth::TARGET + 4)`.

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && grep -n "fill() < 12\|< 15; i++\|== 15" tests/node_test.cpp
```

Expected: no output (every literal replaced).

- [ ] **Step 4: Run node_test to verify it fails (RED)**

`setPrefill` does not exist yet, so this is a compile failure — the honest RED for an API that is not there:

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./tests/run.sh 2>&1 | grep -m1 "setPrefill"
```

Expected: an error line containing `no member named 'setPrefill'`.

- [ ] **Step 5: Add the configuration and the flag to the header**

In `$S/AudioInputBluetooth.h`, replace the line

```cpp
    static constexpr uint16_t RING = 16, TARGET = 8;
```

with

```cpp
    // NEW-42: ring depth and target in blocks, and whether START pre-fills the ring, all overridable from CMake
    // (BT_SINK_RING / BT_SINK_TARGET / BT_SINK_PREFILL) so the bench builds its CONTROL arm (16 / 8 / off -- the
    // NEW-41 behaviour) and its CHANGE arm from ONE source.  Sizing (spec s2): a source delivers EIGHT blocks per
    // packet, so the ring needs one packet of headroom above the operating point, the largest gap it should ride
    // out below it (~16 blocks = 46 ms), and the servo's standing offset (+-2.5 blocks at +-100 ppm).  32 / 16
    // gives 16 above and 16 below; the old 16 / 8 held exactly two packets, and the servo's offset parked the
    // mean at the ceiling (iPhone bench 2026-09-09, run 3).  The gate always builds the defaults.
    static constexpr uint16_t RING = BT_SINK_RING, TARGET = BT_SINK_TARGET;
    static constexpr bool PREFILL = (BT_SINK_PREFILL) != 0;
    static_assert(RING >= 4 && TARGET >= 1 && TARGET <= RING - 2,
        "AudioInputBluetooth: TARGET must leave at least one packet's worth of room below RING");
    // Whether begin() pre-fills to TARGET before the first pop, and whether a dry ring re-primes (Task 3/4 of the
    // NEW-42 plan).  Defaults to the compile-time PREFILL; the sketch never calls this, the host tests do, so that
    // the ring/parser cases run without the pre-fill and the pre-fill cases run with it, in BOTH CMake arms.
    void setPrefill(bool on) { m_prefill = on; }
```

and, ABOVE the `class AudioInputBluetooth` line (after the `static_assert(AUDIO_BLOCK_SAMPLES == 128, ...)`), add:

```cpp
#ifndef BT_SINK_RING
#define BT_SINK_RING 32
#endif
#ifndef BT_SINK_TARGET
#define BT_SINK_TARGET 16
#endif
#ifndef BT_SINK_PREFILL
#define BT_SINK_PREFILL 1
#endif
```

and in the `private:` section, after `volatile bool m_hold = false;`, add:

```cpp
    bool m_prefill = PREFILL;                      // set before begin(); read by begin()/update() (Task 3/4)
```

- [ ] **Step 6: Run node_test to verify it passes (GREEN)**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./tests/run.sh 2>&1 | tail -2
```

Expected: `node_test: N checks, 0 failures` (N ≈ 106).

- [ ] **Step 7: Build the CONTROL arm of node_test in run.sh**

In `$S/tests/run.sh`, after the block that compiles and runs `node_test` (the `$CXX ... -o "$OUT/node_test"` command and the `"$OUT/node_test"` line), append:

```sh
# NEW-42: the same node compiled as the bench's CONTROL arm (RING 16 / TARGET 8 / no pre-fill -- the NEW-41
# behaviour).  Cases whose expectation depends on the configuration compute it from RING/TARGET, and case 14
# additionally asserts the control arm DOES drop (9 of 24) where the default does not.  Both binaries must pass:
# a configuration that only works at one setting is a configuration nobody measured.
$CXX -std=c++11 -Wall -Wextra -Werror $SAN \
    -DBT_SINK_RING=16 -DBT_SINK_TARGET=8 -DBT_SINK_PREFILL=0 -DNODE_TEST_CONTROL_ARM=1 \
    -I"$DIR/shim" -I"$DIR/.." -I"$BT" \
    "$DIR/node_test.cpp" "$DIR/shim/shim.cpp" "$DIR/../AudioInputBluetooth.cpp" \
    "$BT/SbcDecoder.cpp" "$BT/Sbc.cpp" \
    -o "$OUT/node_test_control"
"$OUT/node_test_control"
```

- [ ] **Step 8: Run both arms**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./tests/run.sh 2>&1 | grep "node_test"
```

Expected: two lines, both `node_test: N checks, 0 failures` (the control arm's N equals the default's).

- [ ] **Step 9: Expose the three values as CMake cache entries**

In `$S/CMakeLists.txt`, before the `teensy_add_executable(bt_sink_test ...` line, add:

```cmake
# NEW-42: the PCM ring's depth and target (blocks) and the START pre-fill.  Defaults are the CHANGE arm
# (32 / 16 / on), which is what the gate builds.  The bench's CONTROL arm -- the NEW-41 behaviour, 16 / 8 / off --
# is `-DBT_SINK_RING=16 -DBT_SINK_TARGET=8 -DBT_SINK_PREFILL=OFF`.  The DelayReport the sink sends follows
# BT_SINK_TARGET automatically (bt_sink_test.cpp derives it), so a resized target is reported to the phone.
set(BT_SINK_RING "32" CACHE STRING "PCM ring depth in audio blocks (AudioInputBluetooth::RING)")
set(BT_SINK_TARGET "16" CACHE STRING "Ring fill the servo holds and START pre-fills to, in blocks (TARGET)")
option(BT_SINK_PREFILL "Pre-fill the ring to TARGET at START and re-prime it when it runs dry" ON)
if(BT_SINK_PREFILL)
    set(BT_SINK_PREFILL_DEF 1)
else()
    set(BT_SINK_PREFILL_DEF 0)
endif()
```

and after the `teensy_add_executable(bt_sink_test ...)` line add:

```cmake
target_compile_definitions(bt_sink_test.elf PRIVATE
    BT_SINK_RING=${BT_SINK_RING} BT_SINK_TARGET=${BT_SINK_TARGET} BT_SINK_PREFILL=${BT_SINK_PREFILL_DEF})
```

- [ ] **Step 10: Reconfigure and build the gate image; confirm the ring grew**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake >/dev/null && cmake --build build 2>&1 | tail -1 && /Applications/ARM_10/bin/arm-none-eabi-nm -S build/bt_sink_test.elf | grep " _ZL4btin$"
```

Expected: the build's last line names `bt_sink_test.hex` (or `Built target`), and the `nm` line shows `btin` with size `000049c4` (18,884 B — was `000029c4`).

- [ ] **Step 11: Run the gate — the defaults change nothing it asserts**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./run_qemu.sh 2>&1 | tail -1
```

Expected: `PASS: A2DP SINK ...`. (The golden `crc200` is computed at decode time; a deeper ring cannot move it, and `over=0` only gets easier.)

- [ ] **Step 12: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git add examples/audio/bt_sink_test/AudioInputBluetooth.h examples/audio/bt_sink_test/tests/node_test.cpp examples/audio/bt_sink_test/tests/run.sh examples/audio/bt_sink_test/CMakeLists.txt && git commit -q -F - <<'MSG' && git log --oneline -1
audio/bt_sink_test: RING 32 / TARGET 16 (was 16 / 8), configurable from CMake for the NEW-42 A/B bench

A source delivers EIGHT blocks per RTP packet (iPhone bench run 3: frames/pkts 7.97), so the old
ring held exactly two packets and the servo's standing offset parked the mean at the ceiling.
32 / 16 gives one packet of headroom above and 16 blocks below.  BT_SINK_RING / BT_SINK_TARGET /
BT_SINK_PREFILL are CMake cache values; the gate builds the defaults, the bench builds the
CONTROL arm (16 / 8 / off) from the same source.  node_test case 14 pins the headroom claim from
RING (RED at 16: 9 of 24 dropped) and run.sh now builds the control arm too.  setPrefill() is
introduced as the stored flag; its behaviour is the next two commits.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
MSG
```

---

### Task 2: The instrument — gap histogram, fill/trim extremes, overrun events

**Files:**
- Modify: `examples/audio/bt_sink_test/tests/shim/Arduino.h`
- Modify: `examples/audio/bt_sink_test/tests/shim/shim.cpp`
- Modify: `examples/audio/bt_sink_test/AudioInputBluetooth.h`
- Modify: `examples/audio/bt_sink_test/AudioInputBluetooth.cpp`
- Modify: `examples/audio/bt_sink_test/tests/node_test.cpp`

- [ ] **Step 1: Give the host shim an injectable `micros()`**

Append to `$S/tests/shim/Arduino.h`:

```cpp
// NEW-42: the node timestamps every accepted RTP packet with micros() to measure the source's inter-packet
// gaps.  On the target that is the core's clock; here it is whatever the test last set, so an interval is exact.
uint32_t micros(void);
void shimSetMicros(uint32_t us);
```

Append to `$S/tests/shim/shim.cpp`:

```cpp
namespace { uint32_t s_micros = 0; }
uint32_t micros(void) { return s_micros; }
void shimSetMicros(uint32_t us) { s_micros = us; }
```

- [ ] **Step 2: Write the failing instrument case**

Append to `$S/tests/node_test.cpp`, before the final `printf`:

```cpp
    {   // 11. THE INSTRUMENT (NEW-42).  Inter-packet intervals land in the bucket their length names and the max
        //     is the max; the ISR-side fill/trim extremes are the extremes of a scripted sequence; a RUN of
        //     dropped frames is ONE overrun event.  micros() is the shim's, so every interval is exact.
        Node nd; nd->setPrefill(false); nd->begin();
        std::vector<uint8_t> f = encodeFrame(8000);
        uint32_t t = 1000000; shimSetMicros(t); feed(nd, onePacket(1, f));
        for (uint8_t i = 0; i < AudioInputBluetooth::GAP_BUCKETS; i++) CHECK(nd->gapBucket(i) == 0);   // no predecessor yet
        // Bucket tops are INCLUSIVE (<=30 ms is the first bucket) and 30000 / 50000 / 120000 sit exactly on them:
        // RED with a `<` compare -- three entries each move up one bucket.
        const uint32_t gaps_us[] = { 10000, 30000, 25000, 45000, 50000, 70000, 100000, 120000, 130000 };
        uint16_t seq = 2;
        for (size_t i = 0; i < sizeof gaps_us / sizeof gaps_us[0]; i++) {
            nd->update(); t += gaps_us[i]; shimSetMicros(t); feed(nd, onePacket(seq++, f));
        }
        CHECK(nd->gapBucket(0) == 3);                                             // 10, 30, 25
        CHECK(nd->gapBucket(1) == 2);                                             // 45, 50
        CHECK(nd->gapBucket(2) == 1);                                             // 70
        CHECK(nd->gapBucket(3) == 2);                                             // 100, 120
        CHECK(nd->gapBucket(4) == 1);                                             // 130
        CHECK(nd->gapMaxUs() == 130000);
        // Fill extremes are sampled by update() at the instant it POPS (pre-pop fill) and only then: a sample
        // taken on a dry block would pin fillMin at 0 forever and say nothing about margin.  The ring is walked
        // up to 9 and down to 0; the only extremes consistent with that walk are max 9 / min 1.
        // RED against extremes latched at their init values: fillMin() reads RING, fillMax() 0.
        Node n2; n2->setPrefill(false); n2->begin();
        for (int i = 1; i <= 9; i++) feed(n2, onePacket((uint16_t)i, f));
        n2->update();                                                             // samples 9, pops -> 8
        for (int i = 0; i < 7; i++) n2->update();                                // samples 8..2, pops -> 1
        CHECK(n2->fillMax() == 9); CHECK(n2->fillMin() == 2);
        n2->update();                                                             // samples 1, pops -> 0
        CHECK(n2->fillMin() == 1);
        CHECK(n2->trimLo() <= 0 && n2->trimHi() >= 0 && n2->trimLo() <= n2->trimHi());
        // Overrun EVENTS: a full ring dropping eight in a row is over=8 overev=1; free one slot, land one, drop
        // one more -> overev=2.  RED with the run latch removed: overEvents() == 8.
        Node n3; n3->setPrefill(false); n3->begin();
        uint16_t s3 = 1;
        for (int i = 0; i < AudioInputBluetooth::RING - 1; i++) feed(n3, onePacket(s3++, f));
        CHECK(n3->overruns() == 0 && n3->overEvents() == 0);
        for (int i = 0; i < 8; i++) feed(n3, onePacket(s3++, f));
        CHECK(n3->overruns() == 8); CHECK(n3->overEvents() == 1);
        n3->update(); feed(n3, onePacket(s3++, f)); CHECK(n3->overruns() == 8);   // one slot freed, one frame landed
        feed(n3, onePacket(s3++, f));
        CHECK(n3->overruns() == 9); CHECK(n3->overEvents() == 2);
    }
```

- [ ] **Step 3: Run to verify it fails (RED)**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./tests/run.sh 2>&1 | grep -m1 "GAP_BUCKETS\|gapBucket"
```

Expected: an error containing `no member named 'GAP_BUCKETS'` (or `gapBucket`).

- [ ] **Step 4: Add the instrument to the header**

In `$S/AudioInputBluetooth.h`, in the `public:` section after the `setPrefill` method, add:

```cpp
    // --- the NEW-42 instrument: cumulative since begin(), never per-heartbeat --------------------------------
    // The committed transcript samples every 30th heartbeat, so a per-window maximum would be invisible in 29 of
    // 30 windows -- and an extreme that is never printed reads exactly like one that never happened.
    static constexpr uint8_t GAP_BUCKETS = 5;      // <=30 / <=50 / <=80 / <=120 / >120 ms, against a 23.2 ms nominal period
    static uint8_t gapBucketOf(uint32_t us) { return us <= 30000u ? 0 : us <= 50000u ? 1 : us <= 80000u ? 2 : us <= 120000u ? 3 : 4; }
    uint32_t gapMaxUs() const { return m_gapMaxUs; }                       // longest interval between two accepted RTP packets
    uint32_t gapBucket(uint8_t i) const { return i < GAP_BUCKETS ? m_gap[i] : 0; }
    uint8_t  fillMin() const { return m_fillMin; }                         // ring fill at the instant of a pop: the true low-side margin
    uint8_t  fillMax() const { return m_fillMax; }                         // ... and the high side; RING (min) / 0 (max) until the first pop
    int32_t  trimLo() const { return m_trimLo; }                           // the servo's output range: "is the trim settled" as a number
    int32_t  trimHi() const { return m_trimHi; }
    uint32_t overEvents() const { return m_overEv; }                       // RUNS of consecutive dropped frames (a burst of 8 is one event)
```

and in the `private:` section, after `bool m_prefill = PREFILL;`, add:

```cpp
    // Instrument state.  m_gap*/m_lastRx*: onMedia() (main context) writes, loop() reads -- same context.  The
    // fill/trim extremes are written by update() (the SAI ISR) and read by loop(): 8/32-bit aligned stores the
    // ISR replaces wholesale, so a torn read is not expressible; volatile so the heartbeat cannot cache them.
    uint32_t m_lastRxUs = 0; bool m_haveRx = false;
    uint32_t m_gapMaxUs = 0, m_gap[GAP_BUCKETS] = { 0, 0, 0, 0, 0 };
    volatile uint8_t m_fillMin = (uint8_t)RING, m_fillMax = 0;
    volatile int32_t m_trimLo = 0, m_trimHi = 0;
    uint32_t m_overEv = 0; bool m_inOverrun = false;
```

- [ ] **Step 5: Implement it in the node**

In `$S/AudioInputBluetooth.cpp`:

`begin()` — after the line `m_head = m_tail = 0; m_dec.reset(); ...`, add:

```cpp
    m_haveRx = false; m_lastRxUs = 0; m_gapMaxUs = 0; for (uint8_t i = 0; i < GAP_BUCKETS; i++) m_gap[i] = 0;
    m_fillMin = (uint8_t)RING; m_fillMax = 0; m_trimLo = 0; m_trimHi = 0; m_overEv = 0; m_inOverrun = false;
```

`pushFrame()` — replace the line `if (next == m_tail) { m_over++; return; }` with:

```cpp
    if (next == m_tail) { m_over++; if (!m_inOverrun) { m_inOverrun = true; m_overEv++; } return; }
```

and after `m_head = next; m_frames++;` add:

```cpp
    m_inOverrun = false;                           // a frame landed: the run of drops, if any, is over
```

`onMedia()` — after the line `m_lastSeq = seq; m_haveSeq = true; m_pkts++;` add:

```cpp
    // The source's delivery cadence, measured on every ACCEPTED packet (a rejected header is not a delivery).
    uint32_t now = micros();
    if (m_haveRx) { uint32_t d = now - m_lastRxUs; if (d > m_gapMaxUs) m_gapMaxUs = d; m_gap[gapBucketOf(d)]++; }
    m_lastRxUs = now; m_haveRx = true;
```

`update()` — replace the `else { memcpy(...) ... }` pop branch and the servo line so the function reads:

```cpp
void AudioInputBluetooth::update(void) {
    audio_block_t *l = allocate(), *r = allocate();
    if (!l || !r) { if (l) release(l); if (r) release(r); return; }
    uint16_t tail = m_tail;
    // HELD (the source SUSPENDed) is silence that is NOT an underrun: nothing is missing, the source stopped on
    // purpose.  Counting it would bury the real underruns under 344 of these a second, and the ring is left
    // alone so a RESUME plays what is already in it.
    bool run = m_live && !m_hold;
    if (!run || tail == m_head) { memset(l->data, 0, sizeof l->data); memset(r->data, 0, sizeof r->data); if (run) m_under++; }
    else {
        uint8_t f = fill();                        // pre-pop: the margin this block found, for fillMin/fillMax
        if (f < m_fillMin) m_fillMin = f;
        if (f > m_fillMax) m_fillMax = f;
        memcpy(l->data, m_ring[tail].l, sizeof l->data); memcpy(r->data, m_ring[tail].r, sizeof r->data); m_tail = (uint16_t)((tail + 1) % RING);
    }
    // The servo runs once per audio block -- this IS the block clock -- and only while the link is up and not
    // held: with the link down or the source suspended the trim is frozen where it was, so a resumed stream
    // starts from the rate it had learned.  (servo_step's own `hold` argument expresses the same freeze; the
    // node skips the call outright so a held stream costs nothing at all in the ISR.)
    if (run) {
        int32_t t = servo_step(&m_servo, fill(), 0);
        if (t < m_trimLo) m_trimLo = t;
        if (t > m_trimHi) m_trimHi = t;
        if (t != m_applied) { m_applied = t; audioPllTrimPpm(t); }
    }
    transmit(l, 0); transmit(r, 1); release(l); release(r);
}
```

- [ ] **Step 6: Run to verify it passes (GREEN), both arms**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./tests/run.sh 2>&1 | grep "node_test"
```

Expected: two `node_test: N checks, 0 failures` lines (N ≈ 130).

- [ ] **Step 7: Prove the RED arms by mutation, then restore**

Bucket edge: change `us <= 30000u` to `us < 30000u` in `gapBucketOf`, run, expect `FAIL ... gapBucket(0) == 3`; restore. Event latch: change `if (!m_inOverrun) { m_inOverrun = true; m_overEv++; }` to `m_overEv++;`, run, expect `FAIL ... overEvents() == 1`; restore.

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && git diff --stat -- AudioInputBluetooth.h AudioInputBluetooth.cpp | tail -1
```

Expected after restoring: the diff stat shows only the intended additions (no leftover mutation — re-run `./tests/run.sh` and see `0 failures`).

- [ ] **Step 8: Build the gate image and run the gate**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./run_qemu.sh 2>&1 | tail -1
```

Expected: `PASS: A2DP SINK ...` (nothing the gate reads has changed yet).

- [ ] **Step 9: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git add examples/audio/bt_sink_test && git commit -q -F - <<'MSG' && git log --oneline -1
audio/bt_sink_test: the jitter instrument -- inter-packet gap histogram + max, ISR-side fill/trim extremes, overrun EVENTS

All cumulative since begin(): the committed transcript samples every 30th heartbeat, so a
per-window extreme would be invisible in 29 of 30 windows.  Gaps are measured in onMedia() on
every accepted packet (<=30/50/80/120/>120 ms against a 23.2 ms period); fill extremes are
sampled at the instant of a pop -- the ISR is the only place that sees every block, and the
heartbeat's fill= is a random-phase sample of an 8-block sawtooth; a run of consecutive drops
is one event.  node_test case 11 pins the bucket edges (inclusive tops, RED with `<`), the
extremes (RED when latched at init) and the event latch (RED: 8 events for 8 drops).  The host
shim gains an injectable micros().

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
MSG
```

---

### Task 3: Pre-fill at START

**Files:**
- Modify: `examples/audio/bt_sink_test/AudioInputBluetooth.h`
- Modify: `examples/audio/bt_sink_test/AudioInputBluetooth.cpp`
- Modify: `examples/audio/bt_sink_test/tests/node_test.cpp`

- [ ] **Step 1: Write the failing pre-fill case**

Append to `$S/tests/node_test.cpp`, before the final `printf`:

```cpp
    {   // 12. PRE-FILL AT START (NEW-42).  With the pre-fill on, update() transmits silence and counts NO underrun
        //     until TARGET blocks are buffered; the block that finds TARGET completes the prime AND pops, and the
        //     first block out is the first block DECODED -- nothing is discarded.  primeBlocks() is how many
        //     silent blocks the prime took (the START figure, latched).
        //     RED against the pre-fill removed: underruns() reads TARGET-1 after the loop, not 0.
        //     RED against a prime that discards what it buffered: the first block out is the quiet one.
        Node nd; nd->setPrefill(true); nd->begin();
        CHECK(nd->priming()); CHECK(!nd->primed());
        std::vector<uint8_t> loud = encodeFrame(16000), quiet = encodeFrame(0);
        uint16_t seq = 1;
        feed(nd, onePacket(seq++, loud));                                         // the FIRST decoded block is loud
        for (int i = 1; i < AudioInputBluetooth::TARGET; i++) {                   // TARGET-1 silent periods, a packet each
            nd->update();
            CHECK(nd->priming()); CHECK(nd->underruns() == 0);
            const int16_t *tx = lastTx(0); CHECK(tx && allZero(tx));              // silence while priming, never poison
            feed(nd, onePacket(seq++, quiet));
        }
        CHECK(nd->fill() == AudioInputBluetooth::TARGET);
        CHECK(nd->primeBlocks() == (uint32_t)AudioInputBluetooth::TARGET - 1);
        ShimAudio::reset();
        nd->update();                                                             // finds TARGET: prime complete, block popped
        CHECK(!nd->priming()); CHECK(nd->primed());
        CHECK(nd->fill() == AudioInputBluetooth::TARGET - 1);
        // A stream's FIRST frame decodes quiet by construction (the filterbank ramps in; case 1 measured 4299 for
        // a 16384 sine), so the bar is 3000: well above the quiet frame's 0, well below steady state.
        const int16_t *first = lastTx(0); CHECK(first && ShimAudio::meanAbs(first) > 3000);
        CHECK(nd->underruns() == 0);
        CHECK(nd->primeBlocks() == (uint32_t)AudioInputBluetooth::TARGET - 1);    // latched at completion
        CHECK(nd->trimPpm() == 0);                                                // the servo restarted from zero error
        CHECK(ShimAudio::outstanding() == 0);
    }
```

- [ ] **Step 2: Run to verify it fails (RED)**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./tests/run.sh 2>&1 | grep -m1 "priming\|primed"
```

Expected: an error containing `no member named 'priming'`.

- [ ] **Step 3: Add the priming state to the header**

In `$S/AudioInputBluetooth.h`, in the `public:` section after `overEvents()`, add:

```cpp
    // --- START pre-fill / re-prime (NEW-42, spec s2) ----------------------------------------------------------
    // While priming, update() transmits silence, counts no underrun and steps no servo; the block that finds
    // TARGET in the ring ends the prime, recentres the servo's filter and pops straight away.  primeBlocks() is
    // the START prime's length in blocks (x 2.9 ms), latched when it completes; reprimes() counts the mid-stream
    // rebuilds (Task 4) and is NOT incremented by the START prime.
    bool     priming() const { return m_priming; }
    bool     primed() const { return m_primed; }
    uint32_t primeBlocks() const { return m_primeBlocks; }
    uint32_t reprimes() const { return m_reprimes; }
    const sink_servo_t &servo() const { return m_servo; }                  // host tests read the filter; nothing else may
```

and in the `private:` section, after the instrument state block, add:

```cpp
    volatile bool m_priming = false, m_primed = false;
    volatile uint32_t m_primeBlocks = 0, m_reprimes = 0;
```

- [ ] **Step 4: Implement the pre-fill in `begin()`, `end()` and `update()`**

In `$S/AudioInputBluetooth.cpp`:

`begin()` — after the instrument reset line added in Task 2, add:

```cpp
    m_priming = m_prefill; m_primed = false; m_primeBlocks = 0; m_reprimes = 0;
```

`end()` — replace the whole one-line function with:

```cpp
void AudioInputBluetooth::end() { m_live = false; m_priming = false; m_head = m_tail = 0; m_fragLen = 0; m_fragging = false; }   // the trim HOLDS (servo_step is skipped while !m_live)
```

`update()` — replace the whole function with:

```cpp
void AudioInputBluetooth::update(void) {
    audio_block_t *l = allocate(), *r = allocate();
    if (!l || !r) { if (l) release(l); if (r) release(r); return; }
    uint16_t tail = m_tail;
    // HELD (the source SUSPENDed) is silence that is NOT an underrun: nothing is missing, the source stopped on
    // purpose.  Counting it would bury the real underruns under 344 of these a second, and the ring is left
    // alone so a RESUME plays what is already in it.
    bool run = m_live && !m_hold;
    bool popped = false;
    if (run) {
        // A prime ends at the TOP of the block that finds TARGET in the ring, and that block is popped straight
        // away -- waiting one more period would add a silent block for nothing.  The servo's filter is
        // recentred so the excursion (0 -> TARGET) does not drag the trim; the trim itself is kept.
        if (m_priming && fill() >= TARGET) { m_priming = false; m_primed = true; servo_recentre(&m_servo); }
        if (m_priming) {
            if (!m_primed) m_primeBlocks++;         // the START prime is timed; a re-prime is counted (Task 4), not timed
        } else if (tail == m_head) {
            m_under++;                             // dry: one underrun for this block (Task 4 makes it ONE per event)
        } else {
            uint8_t f = fill();                    // pre-pop: the margin this block found, for fillMin/fillMax
            if (f < m_fillMin) m_fillMin = f;
            if (f > m_fillMax) m_fillMax = f;
            memcpy(l->data, m_ring[tail].l, sizeof l->data); memcpy(r->data, m_ring[tail].r, sizeof r->data); m_tail = (uint16_t)((tail + 1) % RING);
            popped = true;
        }
    }
    if (!popped) { memset(l->data, 0, sizeof l->data); memset(r->data, 0, sizeof r->data); }
    // The servo runs once per audio block -- this IS the block clock -- and only while the link is up, not held
    // and NOT PRIMING: a priming ring is being filled deliberately, and integrating that ramp would trim the
    // pitch against a fill the servo did not cause.  Post-pop fill, as it always was (the control arm's
    // behaviour must not move).
    if (run && !m_priming) {
        int32_t t = servo_step(&m_servo, fill(), 0);
        if (t < m_trimLo) m_trimLo = t;
        if (t > m_trimHi) m_trimHi = t;
        if (t != m_applied) { m_applied = t; audioPllTrimPpm(t); }
    }
    transmit(l, 0); transmit(r, 1); release(l); release(r);
}
```

- [ ] **Step 5: Run to verify it passes (GREEN), both arms**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./tests/run.sh 2>&1 | grep "node_test"
```

Expected: two `node_test: N checks, 0 failures` lines (N ≈ 170 — the loop adds 3 checks × (TARGET−1), so the control arm's N is smaller than the default's; both must read `0 failures`).

- [ ] **Step 6: Prove the RED arms by mutation, then restore**

(a) In `begin()`, change `m_priming = m_prefill;` to `m_priming = false;` → run → expect `FAIL ... nd->priming()` on the first check of case 12. Restore. (b) In `update()`, in the prime-completion line add `m_tail = m_head;` after `m_primed = true;` (a prime that discards its buffer) → run → expect `FAIL ... meanAbs(first) > 3000`. Restore. Then `./tests/run.sh` → `0 failures` in both arms.

- [ ] **Step 7: Run the gate**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./run_qemu.sh 2>&1 | tail -1
```

Expected: `PASS: A2DP SINK ...`. In QEMU the prime completes after four 5-frame packets; nothing asserted moves (`crc200` is decode-side, `over=0` holds, `trim_ppm` stays within its clamp).

- [ ] **Step 8: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git add examples/audio/bt_sink_test && git commit -q -F - <<'MSG' && git log --oneline -1
audio/bt_sink_test: pre-fill the ring to TARGET at START (silence, uncounted, servo recentred on completion)

begin() no longer pops from an empty ring: update() holds silence -- not an underrun, nothing is
missing yet -- until TARGET blocks are buffered, then the block that finds TARGET ends the prime,
recentres the servo's filter and pops.  Removes the ~27 start-up underruns of the iPhone bench.
primeBlocks() is the START prime's length.  node_test case 12: no underrun while priming, the
first block OUT is the first block DECODED (RED against a prime that discards its buffer), the
servo restarts from zero error.  Control arm (PREFILL=0) unchanged and still built by run.sh.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
MSG
```

---

### Task 4: Re-prime on a dry ring

**Files:**
- Modify: `examples/audio/bt_sink_test/AudioInputBluetooth.cpp`
- Modify: `examples/audio/bt_sink_test/tests/node_test.cpp`

- [ ] **Step 1: Write the failing re-prime cases**

Append to `$S/tests/node_test.cpp`, before the final `printf`:

```cpp
    {   // 13. RE-PRIME ON A DRY RING (NEW-42, spec s2).  Mid-stream the source stops and the ring runs dry.  Exactly
        //     ONE underrun is counted, reprimes() goes 1, and the node holds silence -- uncounted, servo frozen --
        //     until TARGET blocks are back, then pops.  The filter is RECENTRED on the way back so the excursion
        //     does not drag the trim: the servo is wound far off-centre first, or "recentred" would be satisfied
        //     by a filter that never moved.  Without this a 45 ms gap left the ring 15 blocks short for the
        //     ~72 s it takes the trim to refill it (spec s1, fact 3).
        //     RED against: an underrun per silent block (underruns() climbs past 1); a resume below TARGET (pops
        //     at TARGET-1); the recentre removed (filt stays ~5 blocks off after the resume).
        Node nd; nd->setPrefill(true); nd->begin();
        std::vector<uint8_t> f = encodeFrame(12000);
        uint16_t seq = 1;
        for (int i = 0; i < 1500; i++) { while (nd->fill() < AudioInputBluetooth::TARGET + 6) feed(nd, onePacket(seq++, f)); nd->update(); }
        CHECK(nd->primed() && !nd->priming());
        CHECK(nd->underruns() == 0 && nd->reprimes() == 0);
        CHECK(nd->servo().filt_x65536 - nd->servo().target_x65536 > 4 * 65536);   // wound up: >4 blocks above target
        while (nd->fill() > 0) nd->update();                                        // the source stops: drain to dry
        CHECK(nd->underruns() == 0);
        ShimAudio::reset();
        nd->update();                                                               // the dry block
        CHECK(nd->underruns() == 1); CHECK(nd->reprimes() == 1); CHECK(nd->priming());
        int32_t filtAtDry = nd->servo().filt_x65536;
        for (int i = 0; i < 50; i++) nd->update();                                  // silent, uncounted, servo frozen
        CHECK(nd->underruns() == 1);
        CHECK(nd->servo().filt_x65536 == filtAtDry);
        bool silent = true; for (int i = 0; i < ShimAudio::logCount(); i++) if (!allZero(ShimAudio::logData(i))) silent = false;
        CHECK(silent);
        for (int i = 1; i < AudioInputBluetooth::TARGET; i++) { feed(nd, onePacket(seq++, f)); nd->update(); CHECK(nd->priming()); }   // TARGET-1 is not enough
        CHECK(nd->fill() == AudioInputBluetooth::TARGET - 1);
        feed(nd, onePacket(seq++, f));                                              // TARGET
        ShimAudio::reset();
        nd->update();                                                               // resumes: pops
        CHECK(!nd->priming()); CHECK(nd->reprimes() == 1); CHECK(nd->underruns() == 1);
        CHECK(nd->fill() == AudioInputBluetooth::TARGET - 1);
        const int16_t *tx = lastTx(0); CHECK(tx && !allZero(tx));
        int32_t d = nd->servo().target_x65536 - nd->servo().filt_x65536;            // recentred, then ONE EMA step at TARGET-1
        CHECK(d >= 0 && d <= 65536 / 344 + 1);                                      // RED with the recentre removed: ~5 blocks
        CHECK(nd->primeBlocks() == 0);                                              // a re-prime is not timed as the START prime
        CHECK(ShimAudio::outstanding() == 0);
    }
    {   // 13b. With the pre-fill OFF (the bench's CONTROL arm) a dry ring is the NEW-41 behaviour exactly: one
        //      underrun per silent block, no re-prime, the servo integrating fill=0 -- the A/B's control must be
        //      the old firmware, not a third thing.
        Node nd; nd->setPrefill(false); nd->begin();
        std::vector<uint8_t> f = encodeFrame(12000);
        for (int i = 1; i <= 4; i++) feed(nd, onePacket((uint16_t)i, f));
        for (int i = 0; i < 4; i++) nd->update();
        int32_t trimFull = nd->trimPpm();
        for (int i = 0; i < 10; i++) nd->update();
        CHECK(nd->underruns() == 10); CHECK(nd->reprimes() == 0); CHECK(!nd->priming());
        CHECK(nd->trimPpm() <= trimFull);                                           // the servo kept integrating fill=0
    }
```

- [ ] **Step 2: Run to verify it fails (RED)**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./tests/run.sh 2>&1 | grep -m2 "FAIL"
```

Expected: `FAIL ... nd->reprimes() == 1` (and `nd->priming()`) — the dry block counts an underrun but nothing re-primes.

- [ ] **Step 3: Implement the re-prime**

In `$S/AudioInputBluetooth.cpp`, `update()`: replace the dry branch

```cpp
        } else if (tail == m_head) {
            m_under++;                             // dry: one underrun for this block (Task 4 makes it ONE per event)
        } else {
```

with

```cpp
        } else if (tail == m_head) {
            // DRY.  One underrun for the EVENT, then -- with the pre-fill on -- silence to TARGET, uncounted, and
            // the servo recentred when it completes.  A gap of G blocks otherwise leaves the ring G short until
            // the 72 s trim refills it; the bench measured that as the burst shape (spec s1).  `under` thereby
            // counts dropouts, not silent blocks, which is what makes `under <= reprimes + 2` a usable bound.
            m_under++;
            if (m_prefill) { m_reprimes++; m_priming = true; }
        } else {
```

- [ ] **Step 4: Run to verify it passes (GREEN), both arms**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./tests/run.sh 2>&1 | grep "node_test"
```

Expected: two `node_test: N checks, 0 failures` lines.

- [ ] **Step 5: Prove the RED arms by mutation, then restore**

(a) Remove `servo_recentre(&m_servo);` from the prime-completion line → run → expect `FAIL ... d >= 0 && d <= 65536 / 344 + 1`. Restore. (b) Change `if (m_prefill) { m_reprimes++; m_priming = true; }` to `if (m_prefill) { m_reprimes++; }` (silence never primes) → run → expect `FAIL ... nd->underruns() == 1` after the fifty silent blocks. Restore. Then `./tests/run.sh` → `0 failures` in both arms.

- [ ] **Step 6: Run the gate**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./run_qemu.sh 2>&1 | tail -1
```

Expected: `PASS: A2DP SINK ...`. Look at the captured `under=` in `build/sink.uart`'s last `hb` line: it is now small (one per re-prime — dozens) rather than thousands. That is the QEMU fiction the gate deliberately does not assert.

- [ ] **Step 7: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git add examples/audio/bt_sink_test && git commit -q -F - <<'MSG' && git log --oneline -1
audio/bt_sink_test: re-prime a dry ring -- one underrun per dropout, silence to TARGET, servo recentred

A gap of G blocks left the ring G short and only the 72 s trim refilled it, which is the burst
shape the iPhone bench recorded (15-36 underruns trailing each overrun burst).  Now a dry ring
counts ONE underrun, re-enters priming, and resumes at TARGET with the filter recentred, so a
dropout costs one bounded silence and the margin is restored at once.  under= therefore counts
dropout EVENTS.  node_test case 13 pins all of it (RED: an underrun per silent block; a resume
at TARGET-1; the recentre removed leaves the filter ~5 blocks off); 13b pins the control arm as
the NEW-41 behaviour exactly.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
MSG
```

---

### Task 5: M2Radio — `Avrcp` answers are a three-way fact (B)

**Files:**
- Modify: `~/Development/M2Radio/bt/Avrcp.h`
- Modify: `~/Development/M2Radio/bt/Avrcp.cpp`
- Modify: `~/Development/M2Radio/bt/test/avrcp_test.cpp`
- Modify: `evkb.cmake:119` (the M2Radio pin)

- [ ] **Step 1: Confirm the M2Radio checkout is clean and at the pin**

```bash
cd /Users/nicholasnewdigate/Development/M2Radio && git status --short && git log --oneline -1 && grep -rn "respond(" bt/*.cpp bt/*.h | grep -v "^bt/test"
```

Expected: no status lines; `9f24315 bt: A2dpSink opens AVCTP itself ...`; exactly three `respond(` references — the declaration in `Avrcp.h`, the definition and the one call in `Avrcp::service()` in `Avrcp.cpp`. (If a fourth appears, it must be updated in Step 4 too.)

- [ ] **Step 2: Write the failing test**

In `$M/bt/test/avrcp_test.cpp`, before the final `printf("avrcp_test: ...` line, add:

```cpp
    {   // K1 (NEW-42). WHAT the target answered is a three-way fact, not a bool: a NOTIFICATION, a properly ANSWERED
        //     command, or NOT IMPLEMENTED.  GetCapabilities and SetAbsoluteVolume are answered properly and must land in
        //     answered(), never unsupported() -- on the iPhone bench (2026-09-09) unsupported() climbed with every
        //     volume press and the heartbeat read as the phone sending commands we do not implement.
        //     RED against 9f24315: unsupported() == 2 and answered() does not exist.
        static uint8_t seenK = 0xFF; Avrcp::setVolumeCallback([](void *, uint8_t v) { seenK = v; }, nullptr);
        Avrcp a; CapIo io; L2cap l(io); l.begin(0x0001, 10);
        L2cap::Channel ch{}; ch.psm = Avrcp::PSM; ch.remoteCid = 0x0044; ch.state = L2cap::OPEN;
        std::vector<uint8_t> caps = { 0x10, 0x11, 0x0E, 0x01, 0x48, 0x00, 0x00, 0x19, 0x58, 0x10, 0x00, 0x00, 0x01, 0x03 };
        CHECK(a.onData(ch, caps.data(), (uint16_t)caps.size())); a.service(l); l.service();
        CHECK(a.answered() == 1); CHECK(a.unsupported() == 0); CHECK(a.notifications() == 0);
        std::vector<uint8_t> vol = { 0x30, 0x11, 0x0E, 0x00, 0x48, 0x00, 0x00, 0x19, 0x58, 0x50, 0x00, 0x00, 0x01, 0x40 };
        CHECK(a.onData(ch, vol.data(), (uint16_t)vol.size())); a.service(l); l.service();
        CHECK(a.answered() == 2); CHECK(a.unsupported() == 0); CHECK(seenK == 0x40);
        std::vector<uint8_t> gp = { 0x40, 0x11, 0x0E, 0x01, 0x48, 0x00, 0x00, 0x19, 0x58, 0x30, 0x00, 0x00, 0x00 };   // GetPlayStatus: never built
        CHECK(a.onData(ch, gp.data(), (uint16_t)gp.size())); a.service(l); l.service();
        CHECK(a.answered() == 2); CHECK(a.unsupported() == 1);
        // ... and the pure function reports the same three-way kind.
        uint8_t out[64], kind = 0xEE;
        CHECK(Avrcp::respond(caps.data(), (uint16_t)caps.size(), out, sizeof out, &kind) > 0 && kind == Avrcp::KIND_ANSWERED);
        CHECK(Avrcp::respond(gp.data(), (uint16_t)gp.size(), out, sizeof out, &kind) > 0 && kind == Avrcp::KIND_NOT_IMPLEMENTED);
        CHECK(Avrcp::respond(shokz, sizeof shokz, out, sizeof out, &kind) > 0 && kind == Avrcp::KIND_NOTIFICATION);
        Avrcp::setVolumeCallback(nullptr, nullptr);
    }
```

- [ ] **Step 3: Run to verify it fails (RED)**

```bash
cd /Users/nicholasnewdigate/Development/M2Radio/bt/test && ./run.sh 2>&1 | grep -m1 "answered\|KIND_"
```

Expected: an error containing `no member named 'answered'`.

- [ ] **Step 4: Change the signature and add the counter**

In `$M/bt/Avrcp.h`:

Replace the `unsupported()` accessor line with:

```cpp
    // WHAT each AV/C command was answered with, as three counters (NEW-42 -- one bool used to fold GetCapabilities
    // and SetAbsoluteVolume, both answered properly, into unsupported(), so the sink's heartbeat read a phone's
    // volume presses as commands we do not implement):
    uint32_t answered()      const { return m_answered; }        // answered from respond() with a real reply: GetCapabilities, SetAbsoluteVolume
    uint32_t unsupported()   const { return m_unsupported; }     // NOT IMPLEMENTED (unknown PDU), or IPID for a foreign profile id
    enum { KIND_NOT_IMPLEMENTED = 0, KIND_NOTIFICATION = 1, KIND_ANSWERED = 2 };
```

Replace the `respond` declaration with:

```cpp
    // Build the AV/C response for one AVCTP command frame (pure; host-tested).  Returns the response
    // length (0 = not an AVRCP command: wrong PID, fragment, or a response frame -- ignored).  kind (optional)
    // receives KIND_NOTIFICATION / KIND_ANSWERED / KIND_NOT_IMPLEMENTED for what was built; volumeSet (optional)
    // receives the volume a SetAbsoluteVolume applied, so the OBJECT can track what respond() did; it is left
    // untouched for every other command.
    static uint16_t respond(const uint8_t *cmd, uint16_t len, uint8_t *out, uint16_t outMax, uint8_t *kind,
                            uint8_t *volumeSet = nullptr);
```

Replace `uint32_t m_notifications = 0, m_unsupported = 0, m_dropped = 0;` with:

```cpp
    uint32_t m_notifications = 0, m_answered = 0, m_unsupported = 0, m_dropped = 0;
```

In `$M/bt/Avrcp.cpp`:

Replace the definition's first two lines

```cpp
uint16_t Avrcp::respond(const uint8_t *c, uint16_t len, uint8_t *out, uint16_t outMax, bool *wasNotification, uint8_t *volumeSet) {
    if (wasNotification) *wasNotification = false;
```

with

```cpp
uint16_t Avrcp::respond(const uint8_t *c, uint16_t len, uint8_t *out, uint16_t outMax, uint8_t *kind, uint8_t *volumeSet) {
    if (kind) *kind = KIND_NOT_IMPLEMENTED;
```

In the RegisterNotification branch replace `if (wasNotification) *wasNotification = true;` with `if (kind) *kind = KIND_NOTIFICATION;`.

In the GetCapabilities branch, insert `if (kind) *kind = KIND_ANSWERED;` immediately after the line `out[9] = PDU_GET_CAPABILITIES; out[10] = 0x00;`.

In the SetAbsoluteVolume branch, insert `if (kind) *kind = KIND_ANSWERED;` immediately after `if (volumeSet) *volumeSet = v;`.

In `service()`, replace

```cpp
    uint8_t out[MAX_CMD + 3]; bool notif = false; uint8_t volSet = 0xFF;
    uint16_t n = respond(m_cmd, m_len, out, sizeof out, &notif, &volSet);
```

with

```cpp
    uint8_t out[MAX_CMD + 3]; uint8_t kind = KIND_NOT_IMPLEMENTED; uint8_t volSet = 0xFF;
    uint16_t n = respond(m_cmd, m_len, out, sizeof out, &kind, &volSet);
```

and replace `if (notif) m_notifications++; else m_unsupported++;` with:

```cpp
    if (kind == KIND_NOTIFICATION) m_notifications++; else if (kind == KIND_ANSWERED) m_answered++; else m_unsupported++;
```

- [ ] **Step 5: Update the existing test call sites to the new signature**

In `$M/bt/test/avrcp_test.cpp`, the pure-function cases pass `bool *`; convert each:

- Case 1: `uint8_t out[64]; bool notif = false; uint16_t n = Avrcp::respond(shokz, sizeof shokz, out, sizeof out, &notif);` → `uint8_t out[64], kind = 0; uint16_t n = Avrcp::respond(shokz, sizeof shokz, out, sizeof out, &kind);` and `CHECK(notif);` → `CHECK(kind == Avrcp::KIND_NOTIFICATION);`
- Case 3: `uint8_t out[64]; bool notif = true; uint16_t n = Avrcp::respond(gc, ...&notif);` → `uint8_t out[64], kind = 0; uint16_t n = Avrcp::respond(gc, sizeof gc, out, sizeof out, &kind);` and `CHECK(!notif);` → `CHECK(kind == Avrcp::KIND_ANSWERED);`
- Case 3b: same shape → `CHECK(kind == Avrcp::KIND_NOT_IMPLEMENTED);`
- Case V1: `uint8_t out[64]; bool notif = false; uint16_t n = Avrcp::respond(cmd.data(), ... &notif);` → `... uint8_t kind = 0; ... &kind);` and the trailing `&& !notif)` → `&& kind == Avrcp::KIND_ANSWERED)`
- Case V2: `bool notif = false;` → `uint8_t kind = 0;` and `&notif)` → `&kind)`.

```bash
cd /Users/nicholasnewdigate/Development/M2Radio && grep -n "notif" bt/test/avrcp_test.cpp
```

Expected: no matches left except inside `notifications()` calls.

- [ ] **Step 6: Run the whole M2Radio bt host suite (GREEN)**

```bash
cd /Users/nicholasnewdigate/Development/M2Radio/bt/test && ./run.sh 2>&1 | grep -E "avrcp_test:|BT-HOST-TESTS"
```

Expected: `avrcp_test: N checks, 0 failures` and `BT-HOST-TESTS: PASS`.

- [ ] **Step 7: Commit and push M2Radio, then bump the pin**

```bash
cd /Users/nicholasnewdigate/Development/M2Radio && git add bt/Avrcp.h bt/Avrcp.cpp bt/test/avrcp_test.cpp && git commit -q -F - <<'MSG' && git push origin HEAD:master 2>&1 | tail -1 && git log --oneline -1
bt/Avrcp: answered() -- GetCapabilities and SetAbsoluteVolume no longer count as unsupported()

respond() reported a bool (notification or not), so every properly answered command landed in
unsupported() and the sink's heartbeat read an iPhone's volume presses as commands we do not
implement (NEW-42).  respond() now reports KIND_NOTIFICATION / KIND_ANSWERED / KIND_NOT_IMPLEMENTED
and service() keeps three counters.  avrcp_test K1 pins it (RED at 9f24315: unsupported()==2).
Signature change: the only caller outside the test is service().

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
MSG
```

Expected: the push's last line names `master -> master`; note the new SHA printed by `git log` (call it `NEWSHA`).

Then in `$E/evkb.cmake`, on line 119 (`teensy_declare_library(M2Radio ...`), replace `9f243155e808be40a306cbcec2123a6fb5707917` with the FULL 40-character `NEWSHA` (`git -C ~/Development/M2Radio rev-parse HEAD`), and append to the end of that line's comment: ` 2026-09-09 NEW-42: Avrcp::answered() + respond() reports a three-way kind (SIGNATURE change; service() is the only caller) -- bt_sink_test's heartbeat prints ans= from it and will not BUILD against anything older.`

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && NEWSHA=$(git -C ~/Development/M2Radio rev-parse HEAD) && grep -c "$NEWSHA" evkb.cmake
```

Expected: `1`.

- [ ] **Step 8: Commit the pin**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git add evkb.cmake && git commit -q -m "build: bump M2Radio to $(git -C ~/Development/M2Radio rev-parse --short HEAD) (Avrcp::answered(); respond() kind -- NEW-42 B)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" && git log --oneline -1
```

---

### Task 6: The heartbeat — `bt_jit` line and `ans=`

**Files:**
- Modify: `examples/audio/bt_sink_test/bt_sink_test.cpp`

- [ ] **Step 1: Add `ans=` and the `bt_jit` line**

In `$S/bt_sink_test.cpp`, in `loop()`'s heartbeat, replace

```cpp
        CONSOLE.print(" unsup="); CONSOLE.print(sink.avrcp().unsupported());
```

with

```cpp
        CONSOLE.print(" ans="); CONSOLE.print(sink.avrcp().answered());          // GetCapabilities / SetAbsoluteVolume, answered properly
        CONSOLE.print(" unsup="); CONSOLE.print(sink.avrcp().unsupported());     // NOT IMPLEMENTED (unknown PDU) or IPID
```

and insert, immediately after the `crc200` line (`CONSOLE.print(" crc200=0x"); CONSOLE.println(btin.crc(), HEX);`) and BEFORE the `bt_link` line:

```cpp
        // NEW-42 instrument, cumulative since START.  Placed right after bt_sink and before bt_link so the gate's
        // heartbeat-completeness check (a bt_hci line for every bt_sink line) covers it: a torn final block can
        // never leave a bt_jit line half-read.  gap*: the SOURCE's delivery cadence (main context, micros() per
        // accepted packet) -- REAL in QEMU, since the fake peer paces it.  fillmin/fillmax/trimlo/trimhi/overev/
        // reprimes/primed/prime_ms: the CONSUME side -- SILICON claims, exactly as under= is, because QEMU
        // walks update() on its own schedule and not at 44100/128 Hz.  prime_ms is the START prime's length in
        // blocks converted on the block clock (x 128 / 44100), so it needs no wall clock in the ISR.
        CONSOLE.print("bt_jit gapmax_ms="); CONSOLE.print(btin.gapMaxUs() / 1000u);
        CONSOLE.print(" g30=");  CONSOLE.print(btin.gapBucket(0));
        CONSOLE.print(" g50=");  CONSOLE.print(btin.gapBucket(1));
        CONSOLE.print(" g80=");  CONSOLE.print(btin.gapBucket(2));
        CONSOLE.print(" g120="); CONSOLE.print(btin.gapBucket(3));
        CONSOLE.print(" gbig="); CONSOLE.print(btin.gapBucket(4));
        CONSOLE.print(" fillmin="); CONSOLE.print(btin.fillMin());
        CONSOLE.print(" fillmax="); CONSOLE.print(btin.fillMax());
        CONSOLE.print(" trimlo="); CONSOLE.print(btin.trimLo());
        CONSOLE.print(" trimhi="); CONSOLE.print(btin.trimHi());
        CONSOLE.print(" overev="); CONSOLE.print(btin.overEvents());
        CONSOLE.print(" reprimes="); CONSOLE.print(btin.reprimes());
        CONSOLE.print(" primed="); CONSOLE.print(btin.primed() ? 1 : 0);
        CONSOLE.print(" prime_ms="); CONSOLE.println((uint32_t)btin.primeBlocks() * AUDIO_BLOCK_SAMPLES * 1000u / 44100u);
```

Also update the comment above `sink.setDelayTenthMs(...)` in `setup()`: replace the sentence `TARGET blocks x 128 samples at 44100 Hz = 8 * 128 * 10000 / 44100 = 232 (23.2 ms).` with `TARGET blocks x 128 samples at 44100 Hz: 16 * 128 * 10000 / 44100 = 464 (46.4 ms) at the NEW-42 default, 232 at the old 8.  The phone uses this for lip sync, so a resized target is reported, not hidden.`

- [ ] **Step 2: Build and run the gate**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./run_qemu.sh 2>&1 | grep -E "^bt_jit|^bt_avrcp|^PASS" | tail -3
```

Expected: a `bt_jit gapmax_ms=... g30=... primed=1 prime_ms=...` line, a `bt_avrcp avctp=1 notif=1 ans=2 unsup=0 drop=0 vol=64` line (the peer sends GetCapabilities and SetAbsoluteVolume — `ans=2`, and `unsup=0` where the old firmware printed `unsup=1`), and `PASS: A2DP SINK ...`.

- [ ] **Step 3: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git add examples/audio/bt_sink_test/bt_sink_test.cpp && git commit -q -F - <<'MSG' && git log --oneline -1
audio/bt_sink_test: bt_jit heartbeat line (gap histogram, fill/trim extremes, overrun events, re-primes, prime time) + bt_avrcp ans=

The arrival half (gap*) is real in QEMU because the fake peer paces it; the consume half is a
silicon claim for the same reason under= is.  Placed between bt_sink and bt_link so the gate's
completeness check covers it.  ans= counts properly answered AV/C commands (M2Radio pin), so a
phone's volume presses no longer read as unsupported.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
MSG
```

---

### Task 7: Gate 139 extended — the peer's scripted gap, the histogram assertions, vacuity

**Files:**
- Modify: `examples/networking/m2_hci_probe/hci_peer.py`
- Modify: `examples/audio/bt_sink_test/run_qemu.sh`
- Modify: `tools/gate-vacuity.test.sh`
- Modify: `examples/audio/bt_sink_test/transcript_qemu.txt` (re-captured)

- [ ] **Step 1: Add the gate assertions FIRST (they must fail against the unmodified peer — RED)**

In `$S/run_qemu.sh`, after the `bt_avrcp` block (the `echo "$LASTAVRCP" | grep -q "vol=64" ...` line) and before the final `echo "PASS: ...`, add:

```sh
# --- the NEW-42 instrument, judged against what only the PEER knows ---------------------------------------
# The peer paces media at SOURCE_PERIOD (100 ms) and scripts ONE ~400 ms pause after packet 75.  The
# firmware cannot know either number, so a histogram that reads them back is an instrument that works.
# Two assertions are load-proof by construction and are the ones kept: every one of the 149 inter-packet
# intervals must be bucketed EXACTLY once (a guest stall under sweep load pairs one long interval with one
# near-zero one -- it moves entries between buckets but never changes their count), and the scripted gap
# must show as gbig>=1 with gapmax_ms>=380 (a stall can only lengthen it).  The BULK's bucket is deliberately
# NOT asserted, for the same load reason; node_test case 11 pins the lower bucket edges on the host.
# ★ NOTHING CONSUME-SIDE IS ASSERTED: reprimes/fillmin/fillmax/trimlo/trimhi/prime_ms are as fictional here
# as under= (update() runs on QEMU's schedule) -- the sink re-primes dozens of times against a 100 ms pace,
# and a gate asserting reprimes=1 would be asserting a fiction.  Those are the bench's numbers (spec s4.3).
grep -q "^PEER-SOURCE-GAP after_pkt=75 ms=400" "$RES" || fail "[jit] the peer did not script its 400 ms gap (peer/gate mismatch): $(grep -m1 PEER-SOURCE-GAP "$RES")"
LASTJIT=$(grep -E "^bt_jit " "$OUT" | tail -1)
[ -n "$LASTJIT" ] || fail "[jit] no bt_jit instrument line"
echo "$LASTJIT" | awk '{for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="g30"||a[1]=="g50"||a[1]=="g80"||a[1]=="g120"||a[1]=="gbig") s+=a[2]}} END{exit !(s==149)}' \
    || fail "[jit] the histogram does not account for every interval exactly once (want 149 = pkts-1): $LASTJIT"
echo "$LASTJIT" | awk '{for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="gbig") v=a[2]+0}} END{exit !(v>=1)}' \
    || fail "[jit] the peer's scripted 400 ms gap never reached the >120 ms bucket: $LASTJIT"
echo "$LASTJIT" | awk '{for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="gapmax_ms") v=a[2]+0}} END{exit !(v>=380)}' \
    || fail "[jit] gapmax_ms is below the peer's scripted gap: $LASTJIT"
echo "$LASTJIT" | grep -q " primed=1 "                      || fail "[jit] the START pre-fill never completed: $LASTJIT"
```

and extend the final `PASS:` line's text by appending, before its closing quote: `; the NEW-42 gap instrument reads the peer's pacing back -- 149 intervals bucketed exactly once, the scripted 400 ms gap in gbig with gapmax_ms>=380 -- and the START pre-fill completed (primed=1); reprimes/fill/trim extremes are SILICON claims`.

- [ ] **Step 2: Run the gate to verify it fails (RED, by the peer-mismatch line)**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./run_qemu.sh 2>&1 | tail -1
```

Expected: `FAIL: [jit] the peer did not script its 400 ms gap (peer/gate mismatch): `.

- [ ] **Step 3: Script the gap in the peer**

In `$E/examples/networking/m2_hci_probe/hci_peer.py`, after the line `SOURCE_TS_STEP = SOURCE_FRAMES_PER_PKT * 128      # RTP timestamp units (samples) per packet`, add:

```python
# NEW-42: ONE scripted delivery gap, so the sink's inter-packet histogram has something beyond the 100 ms
# pace to read back (its >120 ms bucket and its max).  A PAUSE, never a burst: the H4 desynchronisation
# recorded above came from pacing too FAST, and a pause only lengthens one interval.  It drops no frame, so
# crc200 (computed at decode) and over=0 cannot move.  After packet 75 of 150: mid-stream, well clear of
# START and of the final heartbeat the gate waits for.
SOURCE_GAP_AFTER_PKT = 75
SOURCE_GAP = 0.400
```

In `src_pump()`, replace

```python
        s["seq"] = (s["seq"] + 1) & 0xFFFF; s["ts"] = (s["ts"] + SOURCE_TS_STEP) & 0xFFFFFFFF; s["pkts"] += 1
```

with

```python
        s["seq"] = (s["seq"] + 1) & 0xFFFF; s["ts"] = (s["ts"] + SOURCE_TS_STEP) & 0xFFFFFFFF; s["pkts"] += 1
        if s["pkts"] == SOURCE_GAP_AFTER_PKT:
            s["next_at"] += SOURCE_GAP                                      # the scripted pause: one interval of 100 + 400 ms
            self.log.append("PEER-SOURCE-GAP after_pkt=%d ms=%d" % (SOURCE_GAP_AFTER_PKT, int(SOURCE_GAP * 1000)))
```

- [ ] **Step 4: Run the gate (GREEN)**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./run_qemu.sh 2>&1 | grep -E "^bt_jit|PEER-SOURCE-GAP|^PASS|^FAIL" | tail -4
```

Expected: `PEER-SOURCE-GAP after_pkt=75 ms=400`, a final `bt_jit` line whose buckets sum to 149 with `gbig=1` (or more under load) and `gapmax_ms` ≥ 500 (100 + 400), and `PASS: A2DP SINK ...`.

- [ ] **Step 5: Demonstrate the two new assertions RED by name, then restore**

(a) In `AudioInputBluetooth.cpp`'s `onMedia()`, change `m_gap[gapBucketOf(d)]++;` to `m_gap[0]++;` (a stubbed histogram) → `./run_qemu.sh 2>&1 | tail -1` → expect `FAIL: [jit] the peer's scripted 400 ms gap never reached the >120 ms bucket`. Restore. (b) In `hci_peer.py`, change `SOURCE_GAP = 0.400` to `SOURCE_GAP = 0.0` → expect `FAIL: [jit] gapmax_ms is below the peer's scripted gap` (the `PEER-SOURCE-GAP ... ms=0` line no longer matches `ms=400` either — whichever fires first, it is a `[jit]` failure by name). Restore. Run the gate once more → `PASS`.

- [ ] **Step 6: Re-capture the committed QEMU transcript**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./run_qemu.sh >/dev/null 2>&1 && cp build/sink.uart transcript_qemu.txt && grep -c "^bt_jit" transcript_qemu.txt && grep "^bt_avrcp" transcript_qemu.txt | tail -1
```

Expected: a positive `bt_jit` count, and the last `bt_avrcp` line reading `... ans=2 unsup=0 ... vol=64`.

- [ ] **Step 7: Update the vacuity suite — green peer fixture, and a new negative**

In `$E/tools/gate-vacuity.test.sh`, section 13:

(a) In the `PEERGREEN` heredoc, insert the line `PEER-SOURCE-GAP after_pkt=75 ms=400` immediately after `PEER-SOURCE-STARTED`, and change `PEER-SINK-DELAYREPORT tenth_ms=232` to `PEER-SINK-DELAYREPORT tenth_ms=464` (the gate does not assert the value, but the fixture should say what the firmware now sends).

(b) After case (b) (`report "green_still_passes_bt_sink_test" $result`) and before `unset GATE_VACUITY GATE_PEER_FIXTURE`, add:

```sh
    # (c) NEW-42: the instrument must not be satisfiable by a line that merely EXISTS.  The committed transcript
    #     with every bt_jit line's gbig entry moved into g120 and gapmax_ms clamped to 100 -- a histogram that
    #     never saw the peer's scripted gap, with the sum still 149 so only the gap assertion can fire -- must
    #     fail by name, with the PERFECT peer fixture (which says the gap WAS sent) supplied.
    awk '/^bt_jit /{ for(i=1;i<=NF;i++){ split($i,a,"="); if(a[1]=="gbig"){ big=a[2]+0; $i="gbig=0" } }
                     for(i=1;i<=NF;i++){ split($i,a,"="); if(a[1]=="g120") $i="g120=" (a[2]+big); if(a[1]=="gapmax_ms") $i="gapmax_ms=100" } }
         { print }' "$EVKB/$sink_rel/transcript_qemu.txt" > "$WORK/sink_no_gap.txt"
    run_gate "$sink_rel" "run_qemu.sh" "$WORK/sink_no_gap.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1                                                                   # must not pass
    echo "$OUT_TEXT" | grep -q "\[jit\] the peer's scripted 400 ms gap never reached" || result=1   # and name it
    report "no_gap_in_histogram_fails_sink_gate" $result
```

- [ ] **Step 8: Run the vacuity suite**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && ./tools/gate-vacuity.test.sh 2>&1 | grep -E "sink|^PASS:|^FAIL:" | grep -E "sink_gate|bt_sink_test" ; ./tools/gate-vacuity.test.sh 2>&1 | grep -c "^PASS:"
```

Expected: `PASS: absent_capture_fails_sink_gate`, `PASS: green_still_passes_bt_sink_test`, `PASS: no_gap_in_histogram_fails_sink_gate`, no `FAIL:` lines, and a total of **46** `PASS:` lines (45 before this task). Re-derive the count from the run, never from this plan (CLAUDE.md's 28/25 lesson).

- [ ] **Step 9: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git add examples/networking/m2_hci_probe/hci_peer.py examples/audio/bt_sink_test/run_qemu.sh examples/audio/bt_sink_test/transcript_qemu.txt tools/gate-vacuity.test.sh && git commit -q -F - <<'MSG' && git log --oneline -1
audio/bt_sink_test gate: the fake source scripts ONE 400 ms gap and the gate reads it back from the bt_jit histogram

Assertions chosen to be load-proof: every one of the 149 intervals bucketed exactly once (a guest
stall moves entries between buckets, never their count), the scripted gap in gbig with
gapmax_ms>=380 (a stall can only lengthen it), primed=1.  Nothing consume-side is asserted --
reprimes/fill/trim extremes are as fictional in QEMU as under= is.  RED twice by name (stubbed
histogram; gap removed).  Transcript re-captured; vacuity gains no_gap_in_histogram (46/46).
Gate count unchanged at 139.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
MSG
```

---

### Task 8: Rebuild, fresh-user, sweep, audit

The M2Radio pin moved (Task 5), so every M2Radio-linking gate image must be rebuilt before the sweep or the sweep passes vacuously against the old library (CLAUDE.md, NEW-36 freshness discipline).

**Files:** none (build directories only)

- [ ] **Step 1: Rebuild every M2Radio-linking GATE build dir (skip the bench dirs that carry real firmware, and `build-fetch`)**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && FAILS=0; for d in $(grep -l "import_evkb_library(M2Radio" examples/*/*/CMakeLists.txt | xargs -n1 dirname); do for b in "$d"/build*; do [ -f "$b/CMakeCache.txt" ] || continue; case "$b" in *build-fetch*) continue;; esac; if grep -qE "^M2RADIO_IW416(_BT)?_FW:FILEPATH=/" "$b/CMakeCache.txt"; then echo "skip bench $b"; continue; fi; if cmake --build "$b" >"$b/rebuild.log" 2>&1; then echo "ok   $b"; else echo "FAIL $b (see $b/rebuild.log)"; FAILS=$((FAILS+1)); fi; done; done; echo "rebuild failures: $FAILS"
```

Expected: `ok` for each of the ~25 gate dirs, `skip bench` for the ~16 bench dirs, `rebuild failures: 0`. A dir failing with `COMPILERPATH is UNDEFINED` or "toolchain file not found" is the cached-toolchain-path trap (CLAUDE.md): `rm -rf` it and configure fresh with `cmake -B <dir> -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake` plus whatever `-D`s its sibling gate script names (read the `run_qemu*.sh` in that example), then build again.

- [ ] **Step 2: Freshness check — the rebuilt images postdate the library change**

`Avrcp::answered()` is an inline accessor and never a symbol, so a symbol grep cannot witness this pin (the
`audioPllConfigure` lesson in CLAUDE.md); ELF mtime against the file that moved is the check:

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && STALE=0; for d in $(grep -l "import_evkb_library(M2Radio" examples/*/*/CMakeLists.txt | xargs -n1 dirname); do for b in "$d"/build*; do [ -f "$b/CMakeCache.txt" ] || continue; case "$b" in *build-fetch*) continue;; esac; grep -qE "^M2RADIO_IW416(_BT)?_FW:FILEPATH=/" "$b/CMakeCache.txt" && continue; f=$(ls "$b"/*.elf 2>/dev/null | head -1); [ -n "$f" ] || continue; if [ "$f" -nt ~/Development/M2Radio/bt/Avrcp.cpp ]; then :; else echo "STALE $f"; STALE=$((STALE+1)); fi; done; done; echo "stale gate ELFs: $STALE"
```

Expected: `stale gate ELFs: 0`. Any `STALE` line names a dir Step 1 did not rebuild — rebuild it and re-run.

- [ ] **Step 3: Fresh-user verification — build from the GitHub pins and RUN the sink gate on the fetched ELF**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && rm -rf build-fetch && cmake -B build-fetch -DEVKB_FORCE_FETCH=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake > build-fetch/configure.log 2>&1; grep -E "M2Radio.*(clone|Already at requested ref)" build-fetch/configure.log | head -2; cmake --build build-fetch 2>&1 | tail -1
```

Expected: a configure-log line showing M2Radio fetched at the new SHA, and a successful build. Then run the gate against that ELF (the gate reads `build/`, so swap the directory for one run):

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && mv build build-local && ln -s build-fetch build && GATE_VACUITY=1 ./run_qemu.sh 2>&1 | tail -1; rm build && mv build-local build && ls -d build build-fetch
```

Expected: `PASS: A2DP SINK ...` (with `GATE_VACUITY=1` the gate skips its own rebuild and runs the existing ELF — here the fetched one — but still runs the REAL peer, because `GATE_PEER_FIXTURE` is unset), then `build build-fetch` listed with `build` a real directory again.

- [ ] **Step 4: The full sweep, alone, output captured**

Nothing else running on the machine — no host suites, no audit, no bench. Via a short-path symlink if this checkout's path is not already `~/Development/rt1170/evkb` (it is; 93 bytes).

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh 2>&1 | tee /tmp/new42-sweep.log | tail -5
```

Expected: `gates: 139 passed` with exit 0 (~24 min). The permitted exceptions are the documented load-sensitivity class (`cm4_audio_test`, `m2_rx_demo[txaggr]`, `m2_uap_lwip[uap]`, `bt_tone_test[media]`, `cm4_wire_int_slave_test`); any such red must be re-run ALONE and pass before it is dispositioned, and any OTHER red is a regression from this branch. Confirm the sink gate's own line:

```bash
grep -E "bt_sink_test" /tmp/new42-sweep.log
```

Expected: `PASS rt1176:audio/bt_sink_test` (with its duration).

- [ ] **Step 5: The licence audit — AFTER the sweep, never during**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && ./tools/license-audit.sh 2>&1 | tail -2
```

Expected: `LICENSE-AUDIT: PASS`. No new build dir was added, so the `GATES` manifest is unchanged.

- [ ] **Step 6: Host suites one last time, both trees**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && ./tests/run.sh 2>&1 | grep -E "servo_test:|node_test:" && cd ~/Development/M2Radio/bt/test && ./run.sh 2>&1 | tail -1
```

Expected: three `0 failures` lines and `BT-HOST-TESTS: PASS`.

---

### Task 9: The bench — a controlled A/B, one session

This task needs the user at the bench (SW4, the iPhone). Both arms build from this branch. Recipe per CLAUDE.md and the spec §4.3.

**Files:**
- Create: `examples/audio/bt_sink_test/transcript_hw_evkb.txt` (appended: a RUN 4 / RUN 5 section)

- [ ] **Step 1: Build BOTH arms**

The firmware blob path is the one `build-bench/CMakeCache.txt` already carries. `M2_BT_FORGET_BONDS=OFF` — the iPhone is bonded.

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && FW=/Users/nicholasnewdigate/Development/mcuxsdk-ws/mcuxsdk/components/conn_fwloader/fw_bin/inc/IW416/uartIW416_bt.bin.inc && for arm in control change; do if [ $arm = control ]; then X="-DBT_SINK_RING=16 -DBT_SINK_TARGET=8 -DBT_SINK_PREFILL=OFF"; else X="-DBT_SINK_RING=32 -DBT_SINK_TARGET=16 -DBT_SINK_PREFILL=ON"; fi; rm -rf build-bench-$arm; cmake -B build-bench-$arm -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DM2RADIO_IW416_BT_FW=$FW -DM2_BT_UART_DNLD=ON -DM2_BT_WAKE_PULSE=ON -DM2_BT_RTS_FLOW=ON -DM2_BT_RTS_WATER=1 -DM2_BT_FAST_BAUD=ON -DM2_BT_FAST_BAUD_RATE=3000000 -DM2_BT_FORGET_BONDS=OFF $X >/dev/null && cmake --build build-bench-$arm 2>&1 | tail -1; done; ls -la build-bench-*/bt_sink_test.hex
```

Expected: both builds succeed; two `.hex` files listed. Sanity-check the arms differ where they should:

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && for a in control change; do echo -n "$a: "; /Applications/ARM_10/bin/arm-none-eabi-nm -S build-bench-$a/bt_sink_test.elf | grep " _ZL4btin$"; done
```

Expected: `control:` shows `btin` at size `000029c4` (the 16-slot ring), `change:` at `000049c4`.

- [ ] **Step 2: Control arm — flash, capture, 10 minutes of music**

Kill any console reader first (holding the VCOM during programming kills the flash — CLAUDE.md), then:

```bash
pkill -f rt1170-console.py; pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 1; cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build-bench-control/bt_sink_test.hex 2>&1 | tail -2 && LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build-bench-control/bt_sink_test.hex 2>&1 | tail -1
```

Expected: the load reports success and verify reports `File matches flash`. If load fails with `-11` on the `.elf`, the `.hex` is already what is used here; if it fails with `Ep(03). Invalid ID`, retry while pressing SW4 (the WFI trap, CLAUDE.md). Then start the reader and ask the user to press SW4:

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && python3 tools/rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > /tmp/sink-bench-control.log 2>&1 &
echo "reader pid $!"
```

Ask the user to: press SW4; on the iPhone, select `EVKB-SINK` in Bluetooth settings (bonded — no pairing prompt); play music for **10 minutes** continuously (no pause); then stop. Watch progress:

```bash
tr -d '\r\000' < /tmp/sink-bench-control.log | grep -E "^hb |^bt_jit " | tail -4
```

Expected during the run: `hb streaming=1 ...` with `under`/`over` climbing in the paired bursts run 3 recorded, and a `bt_jit` line whose `gbig`/`g120` populate and `fillmax` reads 15 (the 16-slot ceiling). Those readings, on the OLD configuration, are the proof the instrument fires.

- [ ] **Step 3: Read the control arm and SIZE `TARGET`**

```bash
tr -d '\r\000' < /tmp/sink-bench-control.log | grep -E "^bt_jit " | tail -1; tr -d '\r\000' < /tmp/sink-bench-control.log | grep -E "^hb streaming=1" | tail -1
```

Decision rule (spec §4.3 step 2): `TARGET` must cover the largest gap the phone routinely produces, in blocks (`ms / 2.9`). If `gapmax_ms` ≤ 46 (16 blocks) and `gbig` ≤ 2 over 10 min, keep **16**. If gaps of 50–80 ms are routine (`g80` in the tens), raise `TARGET` — **and raise `RING` with it**: the sizing rule is `RING − 1 − TARGET ≥ 8` (one whole packet must still fit above the operating point, and one slot is the SPSC sentinel), so `TARGET 24` needs `-DBT_SINK_TARGET=24 -DBT_SINK_RING=40`, not `RING 32`, which would leave SEVEN — less than a packet, the exact defect this issue exists to remove. `RING = TARGET + 16` keeps the design's shape (one packet plus margin above, the gap allowance below). node_test case 14 enforces the rule, so a mis-sized pair fails the host suite before it ever reaches the board. If `gbig` is large with `gapmax_ms` in the hundreds, STOP and record it: that is the phone's delivery, not a buffer size, and the spec says so. Write the measured numbers and the decision into the spec §7 draft as you go.

- [ ] **Step 4: Change arm — flash, capture, the same 10 minutes**

```bash
pkill -f rt1170-console.py; pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 1; cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/bt_sink_test && LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build-bench-change/bt_sink_test.hex 2>&1 | tail -2 && LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build-bench-change/bt_sink_test.hex 2>&1 | tail -1
```

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && python3 tools/rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > /tmp/sink-bench-change.log 2>&1 &
echo "reader pid $!"
```

Same user procedure: SW4, select the sink, 10 min of music. This time ALSO ask the user to listen for any audible change versus the control arm (the +23 ms of latency is not audible on music; a tick or a dropout would be).

- [ ] **Step 5: Judge the change arm against spec §5**

```bash
tr -d '\r\000' < /tmp/sink-bench-change.log | grep -E "^hb streaming=1|^bt_sink |^bt_jit " | tail -3
```

Fill the table (every row a measured number):

| criterion | bound | control arm | change arm | verdict |
|---|---|---|---|---|
| `over` after START | `= 0` | | | |
| `under` vs `reprimes` | `under ≤ reprimes + 2` | | | |
| `fillmin` | `≥ TARGET/4` | | | |
| `fillmax` | `≤ RING − 4` | | | |
| `prime_ms` | ≈ `TARGET × 2.9` once, no start burst | | | |
| audible change | none | | | |
| `trimlo..trimhi` | reported | | | |
| `gapmax_ms`, histogram | reported | | | |

A row that fails is a finding, not a tuning target: record it, and if it is `over > 0` at `RING 32` the DESIGN is wrong (spec §5) — stop and diagnose before touching a constant.

- [ ] **Step 6: Append both runs to the hardware transcript**

Append to `$S/transcript_hw_evkb.txt` a `# ===== RUN 4 (control arm, NEW-42 instruments) =====` section and a `# ===== RUN 5 (change arm) =====` section, each with: the build's `-D` line, the complete event lines (`grep -vE "^(hb|bt_sink|bt_jit|bt_link|bt_hci|bt_avrcp) "`), and the heartbeats SAMPLED every 30th block, exactly as runs 1–3 are laid out — and update the file's header comment with the run-4/5 summary and the §5 table's verdicts. Then:

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git add examples/audio/bt_sink_test/transcript_hw_evkb.txt && git commit -q -m "audio/bt_sink_test: NEW-42 iPhone bench A/B -- control arm (16/8/off + instruments) and change arm (32/16/prefill) transcripts

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" && git log --oneline -1
```

---

### Task 10: Close-out — spec §7, CLAUDE.md, memory, Linear, push

**Files:**
- Modify: `docs/superpowers/specs/2026-09-09-bt-sink-jitter-absorption-design.md` (§7)
- Modify: `CLAUDE.md`
- Modify: `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170-evkb/memory/new41-a2dp-sink.md` and `MEMORY.md`; create `new42-sink-jitter.md`

- [ ] **Step 1: Write spec §7 from the capture**

Replace §7's placeholder paragraph with: the control arm's `bt_jit` line and last `hb`; the measured `TARGET` decision and why; the change arm's lines; the §5 table with verdicts; anything refuted (e.g. if the gap distribution differed from the 35–45 ms estimate, say by how much). Also fill the `PEER-SOURCE-GAP` and `46/46` vacuity facts into §4.2. Commit:

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git add docs/superpowers/specs/2026-09-09-bt-sink-jitter-absorption-design.md && git commit -q -m "docs: NEW-42 spec s7 -- the iPhone A/B bench, measured

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" && git log --oneline -1
```

- [ ] **Step 2: CLAUDE.md**

Insert, immediately BEFORE the line beginning `✅ **Measured 2026-09-09 (afternoon): 139 gates discovered, 139 passed` (line 668 at the time of writing), a new sweep entry and a NEW-42 paragraph in the file's house style:

```markdown
✅ **Measured 2026-09-09 (evening): 139 gates discovered, 139 passed, 0 failed, 0 SKIP** (`gates: 139
passed`, exit 0), on the **NEW-42 sink jitter-absorption** close-out (M2Radio `<short SHA>` pinned for the
`Avrcp::answered()` split; every M2Radio-linking gate dir rebuilt first; fresh-user `-DEVKB_FORCE_FETCH=ON`
verified by RUNNING the sink gate on the fetched ELF; vacuity **46/46**; `LICENSE-AUDIT: PASS` after the sweep).
**No new gate**: gate 139 was extended in place.

★ **NEW-42 (2026-09-09): the A2DP sink absorbs the phone's delivery jitter** (spec
`docs/superpowers/specs/2026-09-09-bt-sink-jitter-absorption-design.md`). Re-reading run 3's per-heartbeat
deltas refuted the issue's own hypothesis: ONE RTP packet delivers EIGHT blocks, so `RING 16` held exactly two
packets and `TARGET 8` was one; the P-only servo's standing offset (`drift/kp` = 2.3 blocks at the measured
90 ppm) parked the mean at the ceiling; and every dropped frame becomes an underrun BY CONSERVATION, with the
deficit permanent at the servo's 72 s timescale (a 45 ms gap left the ring 15 short for a minute). The run
contained its own control -- four consecutive 30 s windows at fill 5-6 with `dunder=0 dover=0`. Fix: `RING 32 /
TARGET 16` (CMake `BT_SINK_RING/TARGET/PREFILL`, the bench's control arm is `16/8/OFF` from the same source),
PRE-FILL at START and RE-PRIME on a dry ring (one counted underrun per dropout, silence to TARGET, servo
recentred), `servo.h` UNCHANGED, plus the `bt_jit` instrument (gap histogram + max, ISR-side fill/trim
extremes, overrun EVENTS, reprimes, prime time -- cumulative since START because the transcript samples every
30th heartbeat). node_test cases 11-14 RED-pin each (bucket tops inclusive; extremes latched; event latch;
prime discards buffer; underrun per silent block; resume below TARGET; recentre removed; headroom from RING);
run.sh builds the control arm too.
★ **The gate asserts only the ARRIVAL side**: the fake peer scripts ONE 400 ms gap after packet 75 and the
gate requires the 149 intervals bucketed EXACTLY once (load-proof: a guest stall moves entries between buckets,
never their count), the gap in `gbig` with `gapmax_ms>=380`, `primed=1`. `reprimes`/`fillmin`/`fillmax`/
`trimlo`/`trimhi`/`prime_ms` are as fictional in QEMU as `under=` -- the sink re-primes dozens of times against
a 100 ms pace -- and are SILICON claims (spec s7). `Avrcp::unsupported()` no longer counts answered
`GetCapabilities`/`SetAbsoluteVolume` (`answered()`, `bt_avrcp ans=`; `respond()` reports a three-way kind --
a SIGNATURE change, `service()` its only caller). Bench A/B (spec s7): <one line of the measured verdict>.
```

Replace `<short SHA>` and `<one line of the measured verdict>` with the real values. Commit:

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git add CLAUDE.md && git commit -q -m "docs: CLAUDE.md -- NEW-42 close-out sweep + the sink jitter-absorption entry

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" && git log --oneline -1
```

- [ ] **Step 3: Memory**

Create `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170-evkb/memory/new42-sink-jitter.md`:

```markdown
---
name: new42-sink-jitter
description: "NEW-42 A2DP sink jitter absorption — DONE <date>: the issue's 'delivery gap + catch-up burst' hypothesis was wrong; 8 blocks/packet into a 16-slot ring + the P servo's 2.3-block standing offset + over→under by conservation; fix RING 32/TARGET 16 + pre-fill + re-prime, servo unchanged; bt_jit instrument; gate asserts arrival side only; bench verdict <one line>"
metadata:
  type: project
---

NEW-42. Spec `docs/superpowers/specs/2026-09-09-bt-sink-jitter-absorption-design.md`, plan
`docs/superpowers/plans/2026-09-09-bt-sink-jitter-absorption.md`.

**The finding that changed the design** (from run 3's per-30 s deltas, not from the issue): one RTP packet is
EIGHT blocks; `RING 16` = two packets; the P-only servo's `drift/kp` offset (2.3 blocks at 90 ppm) parked the
mean at ~10.3 so the 8-block sawtooth hit the ceiling; every dropped frame becomes an underrun by
conservation and the deficit is PERMANENT at the servo's 72 s tau. The run's own control: fill 5-6 → four
windows of `dunder=0 dover=0`.

**What shipped:** `RING 32 / TARGET 16` (CMake `BT_SINK_RING/TARGET/PREFILL`; control arm `16/8/OFF`),
pre-fill at START + re-prime on dry (one counted underrun per dropout, `servo_recentre`), `servo.h` untouched
(options: windowed-min fill; deadband/PI — deferred, see spec §6), `bt_jit` line (cumulative since START),
`Avrcp::answered()` (M2Radio pin <sha>). Gate 139 extended in place: the peer scripts one 400 ms gap; the
gate asserts arrival-side only (149 intervals bucketed exactly once; gbig≥1; gapmax≥380; primed=1) — the
consume side is as fictional in QEMU as `under=`.

**Bench A/B (spec §7):** <control arm bt_jit line>; <TARGET decision>; <change arm verdict per §5 row>.

**Lessons:** [[new41-a2dp-sink]] — a counter target reachable only by adding latency is not a target; a gate
assertion on a consume-side number under a fictional clock asserts a fiction; a histogram's per-window max is
invisible in a 1-in-30 sampled transcript, so instruments are cumulative.
```

Then add to `MEMORY.md` one line: `- [NEW-42 sink jitter](new42-sink-jitter.md) — <the description's first clause>`, and append to `new41-a2dp-sink.md`'s description/body: `under/over jitter → NEW-42 DONE <date> (see [[new42-sink-jitter]])`. Fill every `<...>` from the measured run.

- [ ] **Step 4: Linear**

Move NEW-42 to Done (or In Progress with a comment, if the bench found a §5 row failing) with a comment carrying: the diagnosis in three lines, the change, the gate extension, the A/B table, and the deferred items (servo options 2/3, threshold re-prime, C the pairing mode — link the spec §6). Use `mcp__...__save_issue` with `id: NEW-42`, `state: Done`, and `mcp__...__save_comment`.

- [ ] **Step 5: Merge to master and push**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git checkout master && git merge --ff-only new-42-sink-jitter && git push origin master 2>&1 | tail -1 && git log --oneline -1
```

Expected: a fast-forward, the push's last line `master -> master`. If master moved under the branch (another session), `git rebase master new-42-sink-jitter` first, RE-RUN the sink gate and the vacuity suite on the merged tree, and only then merge — the piece-2 lesson (CLAUDE.md, `912c8d1`).

---

## Self-review (done while writing)

**Spec coverage.** §1 → CLAUDE.md/memory text (Task 10). §2 table: RING/TARGET (Task 1), pre-fill (Task 3), re-prime + `hold()` precedence (Task 4 — `run = m_live && !m_hold` gates every branch, so a held stream neither counts nor primes; `end()` clears priming), servo unchanged (no task touches `servo.h`), DelayReport follows TARGET (Task 6 comment; no code — it was already derived). §3 instrument (Task 2 + Task 6 line; cumulative in `begin()`). §4.1 host arms: pre-fill (12), re-prime (13/13b), headroom (14), instrument (11), avrcp K1 (Task 5). §4.2 gate: peer gap, arrival-only assertions, RED demos, vacuity negative, count 139 (Task 7). §4.3 bench (Task 9, with the TARGET decision rule). §4.4 close-out (Tasks 8, 10). §5 table (Task 9 step 5). §6 → memory/Linear deferred list. §7 → Task 10 step 1. §8 files: all named above; `tools/license-audit.sh` needs no change (no new build dir) — stated in Task 8 step 5.

**Placeholders.** The only `<...>` fields are in Task 10's docs/memory text and are explicitly "fill from the measured run" — they cannot be written before Task 9 runs.

**Type consistency.** `setPrefill(bool)`, `priming()`, `primed()`, `primeBlocks()`, `reprimes()`, `servo()`, `gapMaxUs()`, `gapBucket(uint8_t)`, `GAP_BUCKETS`, `gapBucketOf(uint32_t)`, `fillMin()`, `fillMax()`, `trimLo()`, `trimHi()`, `overEvents()` are declared in Tasks 1–3 with the names the tests (Tasks 1–4) and the sketch (Task 6) use. `Avrcp::KIND_*`, `answered()`, `respond(..., uint8_t *kind, uint8_t *volumeSet)` in Task 5 match the test and the sketch's `ans=`. `shimSetMicros()` in Task 2's shim matches its case 11 use. The peer's `PEER-SOURCE-GAP after_pkt=75 ms=400` string is identical in `hci_peer.py`, `run_qemu.sh` and the vacuity fixture.
