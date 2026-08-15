# AudioTransport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A drift-free tempo/position/loop transport (`AudioTransport`) in the Audio sibling repo, proven by a new two-gate example `examples/audio/transport_test`, ready for a step sequencer to be built on next.

**Architecture:** An `AudioStream` with 0 inputs and 0 outputs, so `update_all()` calls its `update()` once per 128-sample block — the same clock that clocks the DAC. Musical position is a 32.32 fixed-point PPQN tick phase (96 PPQN) that loops with exact integer modulo; elapsed audio time is a separate monotonic `uint64_t` sample counter. Consumers pull a per-block tick span with sub-block sample offsets.

**Tech Stack:** Teensy Audio library conventions (`AudioStream`, 128-sample blocks, `AUDIO_SAMPLE_RATE_EXACT` = 44100), evkb CMake harness (`import_evkb_audio_full`), `tools/gate-lib.sh` QEMU gates, LinkServer for hardware.

**Spec:** `docs/superpowers/specs/2026-08-15-transport-design.md` (approved 2026-08-15).

**Repos touched:** `~/Development/Audio` (component; local-first resolution makes edits live immediately) and this repo (example, gate, bookkeeping). Audio-repo work lands first; the `evkb.cmake` pin bump is the LAST task, after the Audio push.

---

## ★ Read before starting

**1. The QEMU binary may be missing.** As of 2026-08-15 a concurrent qemu2 ASan
`configure` reinitialised `~/Development/qemu2/build`, so
`build/qemu-system-arm` does not exist and EVERY gate reports
`FAIL: no UART capture`. Check before blaming your work:

```bash
ls -l ~/Development/qemu2/build/qemu-system-arm
```

If it is absent, Tasks 1, 2 and 4 still proceed (they only build firmware and
run the audit). Tasks 3, 5 and 6 are blocked until it is restored. **Confirm
with an untouched control gate** (`examples/audio/tone_test/run_qemu_tone.sh`)
before concluding anything about this component.

**2. Update order is construction order — already determined, still verify.**
`AudioStream.h:147-154` appends each new node at the TAIL of `first_update`,
and `software_isr` walks that list head-first (`AudioStream.cpp:322`). So a
node constructed earlier updates earlier, and globals construct in declaration
order within a translation unit. Task 2 adds a runtime assertion rather than
trusting this reading.

**3. Every gate assertion must compare audio-clock quantities on BOTH sides.**
This is the defect that made four `acid_bass_test` checks flake under load
(measured 1.6589 vs a 1.65 ceiling, 0.9008 vs a 0.90 bound, 1.791 vs a working
3.2, and one `nan`). A wall-clock `delay()` is not a measure of audio time.
`examples/audio/acid_bass_test/acid_bass_test.cpp` has the working pattern —
read `waitBlocks()` and `measureOver()` there before writing any assertion here.

---

## Task 1: The failing test — example + gate that cannot build yet

TDD at system level: the example and its gate are the test. They must fail
first for the honest reason (`AudioTransport` does not exist).

**Files:**
- Create: `examples/audio/transport_test/transport_test.cpp`
- Create: `examples/audio/transport_test/CMakeLists.txt`
- Create: `examples/audio/transport_test/run_qemu.sh` (mode 755)

No `boards` sidecar — rt1176 only.

- [ ] **Step 1.1: Write the sketch**

`examples/audio/transport_test/transport_test.cpp`:

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include <math.h>
#include <Audio.h>
#include "control_wm8962.h"   // Audio.h includes control_wm8960.h but NOT this one

#define CONSOLE Serial1
#define PPQN 96

// The transport is declared before its consumers so it updates first:
// AudioStream.h:147-154 appends each node at the tail of first_update and
// software_isr walks that list head-first, so update order is construction
// order. A consumer declared after it therefore sees the CURRENT block's tick
// span, not the previous one. TRANSPORT_ORDER asserts this at runtime rather
// than trusting the reading.
//
// The probes sit either side of the transport and each records the transport's
// sample count at the moment its own update() runs; the difference between
// them is what makes the ordering claim testable.
struct OrderProbe : public AudioStream {
    volatile uint64_t seen = 0;
    OrderProbe() : AudioStream(0, NULL) {}
    void update(void) override;      // defined below, once `transport` exists
};
OrderProbe         probeBefore;
AudioTransport     transport;
OrderProbe         probeAfter;
void OrderProbe::update(void) { seen = transport.samples(); }
AudioSynthAcidBass acid;
AudioAnalyzePeak   peak;
AudioOutputI2S     out;
AudioConnection    cPk(acid, 0, peak, 0);
AudioConnection    cL (acid, 0, out,  0);
AudioConnection    cR (acid, 0, out,  1);
AudioControlWM8962 wm;

// Count audio blocks by watching the transport's own sample counter: it
// advances by exactly AUDIO_BLOCK_SAMPLES per update(), so a change in it IS a
// block tick. This is an AUDIO-clock reference. A wall-clock delay() is not,
// and using one here would make every measurement below host-speed-dependent
// -- the exact defect that flaked four acid_bass_test assertions under load.
static uint64_t waitBlocks(int n) {
    uint64_t start = transport.samples(), prev = start;
    int seen = 0;
    uint32_t t0 = millis();
    while (seen < n && millis() - t0 < 10000) {
        uint64_t s = transport.samples();
        if (s != prev) { seen++; prev = s; }
        yield();
    }
    return prev - start;
}

// Total ticks the transport reports across `blocks` blocks. Reading tickCount()
// once per block is only correct because the loop above wakes on every block
// change; a slower poll would miss spans entirely.
static uint32_t countTicksOver(int blocks) {
    uint32_t total = 0;
    uint64_t prev = transport.samples();
    int seen = 0;
    uint32_t t0 = millis();
    while (seen < blocks && millis() - t0 < 10000) {
        uint64_t s = transport.samples();
        if (s != prev) { seen++; prev = s; total += transport.tickCount(); }
        yield();
    }
    return total;
}

void setup() {
    CONSOLE.begin(115200);
    while (!CONSOLE) {}
    CONSOLE.println("TRANSPORT-GATE v1");
    AudioMemory(16);
    wm.enable();
    wm.volume(0.6f);
    acid.cutoff(700.0f); acid.resonance(0.7f); acid.envMod(0.6f);
    acid.decay(0.15f); acid.level(0.7f);

    // --- TRANSPORT_ORDER: the transport updates before later-declared nodes.
    // Uses the two OrderProbe nodes declared either side of the transport. If
    // update order is construction order, `before` runs while the transport
    // still holds the PREVIOUS block's count and `after` runs once it has
    // advanced, so `after` leads `before` by exactly one block.
    //
    // ★ An earlier draft asserted `transport.samples() > 0`, which proves only
    // that update() ran at all -- it would have passed if the transport
    // updated LAST, or in a random order. The token would have claimed a
    // property it did not test, in the one place the sequencer depends on it.
    transport.tempo(120.0f);
    transport.play();
    waitBlocks(8);
    uint64_t sb = probeBefore.seen, sa = probeAfter.seen;
    int64_t lead = (int64_t)sa - (int64_t)sb;
    CONSOLE.print("TR: probe_before="); CONSOLE.println((uint32_t)sb);
    CONSOLE.print("TR: probe_after=");  CONSOLE.println((uint32_t)sa);
    CONSOLE.print("TR: probe_lead=");   CONSOLE.println((int32_t)lead);
    // Exactly one block. Not >= 0, which a same-order pair would also satisfy.
    bool orderOk = (lead == AUDIO_BLOCK_SAMPLES);
    CONSOLE.println(orderOk ? "TRANSPORT_ORDER=PASS" : "TRANSPORT_ORDER=FAIL");

    // --- TEMPO: ticks over a counted number of BLOCKS vs the analytic value.
    // At 120 BPM, 96 PPQN: ticks/block = PPQN*bpm*128/(60*44100) = 0.557278.
    // Over 400 blocks that is 222.9 ticks. Both sides are audio-clock
    // quantities, so this holds at any host speed.
    transport.looping(false);
    transport.stop();
    transport.play();
    uint32_t ticks400 = countTicksOver(400);
    float expect400 = (float)PPQN * 120.0f * 128.0f * 400.0f
                      / (60.0f * AUDIO_SAMPLE_RATE_EXACT);
    float tempoErr = (float)ticks400 / expect400 - 1.0f;
    CONSOLE.print("TR: ticks400=");   CONSOLE.println(ticks400);
    CONSOLE.print("TR: expect400=");  CONSOLE.println(expect400, 2);
    bool tempoOk = fabsf(tempoErr) < 0.02f;
    CONSOLE.println(tempoOk ? "TEMPO=PASS" : "TEMPO=FAIL");

    // --- LOOP: the seam must neither lose nor duplicate a tick.
    // A 1-bar loop at 4/4 is PPQN*4 = 384 ticks. Running long enough to wrap
    // several times, the total ticks reported must still equal the analytic
    // count -- wrapping changes WHICH tick indices appear, never HOW MANY
    // boundaries are crossed. An off-by-one at the seam is exactly the defect
    // this component would otherwise ship with, and it shows up here.
    transport.stop();
    transport.loop(0.0f, 1.0f);
    transport.looping(true);
    transport.play();
    uint32_t ticksLoop = countTicksOver(1200);
    float expectLoop = (float)PPQN * 120.0f * 128.0f * 1200.0f
                       / (60.0f * AUDIO_SAMPLE_RATE_EXACT);
    float loopErr = (float)ticksLoop / expectLoop - 1.0f;
    float barsNow = transport.bars();
    CONSOLE.print("TR: ticks_loop=");  CONSOLE.println(ticksLoop);
    CONSOLE.print("TR: expect_loop="); CONSOLE.println(expectLoop, 2);
    CONSOLE.print("TR: bars_in_loop="); CONSOLE.println(barsNow, 4);
    bool loopOk = fabsf(loopErr) < 0.02f && barsNow >= 0.0f && barsNow < 1.0f;
    CONSOLE.println(loopOk ? "LOOP=PASS" : "LOOP=FAIL");

    // --- ELAPSED: across a wrap, song position wraps but audio time does not.
    // Without this, a refactor could collapse the two query families into one
    // and every other assertion here would still pass.
    uint64_t s0 = transport.samples();
    float    b0 = transport.bars();
    waitBlocks(900);                       // >1 loop length at this tempo
    uint64_t s1 = transport.samples();
    float    b1 = transport.bars();
    bool elapsedOk = s1 > s0 && b1 < 1.0f && b0 < 1.0f;
    CONSOLE.print("TR: samples_delta="); CONSOLE.println((uint32_t)(s1 - s0));
    CONSOLE.print("TR: bars_b0=");       CONSOLE.println(b0, 4);
    CONSOLE.print("TR: bars_b1=");       CONSOLE.println(b1, 4);
    CONSOLE.println(elapsedOk ? "ELAPSED=PASS" : "ELAPSED=FAIL");

    // --- TEMPOCHANGE: loop length in BARS survives a tempo change. This is
    // the property that motivated ticks-as-master, so it is asserted.
    transport.tempo(180.0f);
    waitBlocks(20);
    float barsAfter = transport.bars();
    uint32_t ticks180 = countTicksOver(400);
    float expect180 = (float)PPQN * 180.0f * 128.0f * 400.0f
                      / (60.0f * AUDIO_SAMPLE_RATE_EXACT);
    bool changeOk = barsAfter >= 0.0f && barsAfter < 1.0f
                    && fabsf((float)ticks180 / expect180 - 1.0f) < 0.02f;
    CONSOLE.print("TR: bars_after_tempo="); CONSOLE.println(barsAfter, 4);
    CONSOLE.print("TR: ticks180=");         CONSOLE.println(ticks180);
    CONSOLE.println(changeOk ? "TEMPOCHANGE=PASS" : "TEMPOCHANGE=FAIL");

    // --- STATE: paused advances nothing; stop() rewinds.
    transport.tempo(120.0f);
    transport.pause();
    uint32_t pausedTicks = countTicksOver(200);
    float barsPaused = transport.bars();
    transport.play();
    waitBlocks(50);
    transport.stop();
    float barsStopped = transport.bars();
    bool stateOk = pausedTicks == 0 && barsStopped == 0.0f && barsPaused >= 0.0f;
    CONSOLE.print("TR: paused_ticks=");  CONSOLE.println(pausedTicks);
    CONSOLE.print("TR: bars_stopped=");  CONSOLE.println(barsStopped, 4);
    CONSOLE.println(stateOk ? "STATE=PASS" : "STATE=FAIL");

    CONSOLE.println("TRANSPORT-DONE");

    // Leave a musical loop running for the hardware session.
    transport.tempo(120.0f);
    transport.loop(0.0f, 1.0f);
    transport.looping(true);
    transport.play();
}

// --- the audible part: four-on-the-floor driven by the transport ------------
// One note per quarter = every PPQN ticks. Reading the span each loop pass is
// enough for an audible demo; the sample-accurate path is the sequencer's job.
void loop() {
    static uint32_t lastQuarter = 0xFFFFFFFFu;
    static uint32_t lastBeat    = 0;
    // Snapshot the span under a brief IRQ guard: this runs in USER context and
    // update() can retire the span mid-read. A sequencer would not need this --
    // it runs inside update_all()'s serial walk, where the span is stable by
    // construction. Copying is cheap: <= 8 entries, normally 0-2.
    uint32_t idx[8]; int n;
    __disable_irq();
    n = transport.tickCount();
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) idx[i] = transport.tickAt(i);
    __enable_irq();
    for (int i = 0; i < n; i++) {
        uint32_t q = idx[i] / PPQN;
        if (q != lastQuarter) {
            lastQuarter = q;
            acid.noteOff(33);
            acid.noteOn(33, (q % 4 == 0) ? 127 : 90);
        }
    }
    uint32_t now = millis();
    if (now - lastBeat >= 250) {
        lastBeat = now;
        if (peak.available()) {
            CONSOLE.print("TR_ALIVE bar="); CONSOLE.print(transport.barNumber());
            CONSOLE.print(" beat=");        CONSOLE.print(transport.beatInBar());
            CONSOLE.print(" peak=");        CONSOLE.println(peak.read(), 4);
        } else {
            CONSOLE.println("TR_ALIVE peak=(no update)");
        }
    }
}
```

- [ ] **Step 1.2: Write the CMakeLists**

`examples/audio/transport_test/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(transport_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
import_evkb_audio_full()

teensy_add_executable(transport_test transport_test.cpp)
teensy_target_link_libraries(transport_test cores Audio Wire SPI SdFat SD SerialFlash)
# CMSIS-DSP is a plain STATIC lib (no `.o` suffix on the target name), so it
# links via raw target_link_libraries -- same as audio_h_test / acid_bass_test.
target_link_libraries(transport_test.elf CMSIS-DSP stdc++ m)
```

- [ ] **Step 1.3: Write the gate script**

`examples/audio/transport_test/run_qemu.sh`:

```sh
#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location.
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
# The measurement script waits on AUDIO blocks rather than wall-clock delays,
# which is what makes its numbers host-speed-independent -- but it also means a
# loaded host stretches the run in wall time. 90 s and 200 poll iterations give
# that room. No assertion was relaxed to fit.
QRUN_TIMEOUT="${QRUN_TIMEOUT:-90}"; export QRUN_TIMEOUT
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/transport_test.elf"
OUT=$(gate_capture_path "$DIR" transport.uart)
DBG=$(gate_capture_path "$DIR" transport.dbg)
rm -f "$OUT" "$DBG"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
for _ in $(seq 1 200); do
    kill -0 "$P" 2>/dev/null || break
    n=$(grep -c 'TR_ALIVE' "$OUT" 2>/dev/null || true)
    [ "${n:-0}" -ge 3 ] && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "TRANSPORT-GATE v1" "$OUT" || { echo "FAIL: banner"; exit 1; }
grep -q "TRANSPORT-DONE"    "$OUT" || { echo "FAIL: script never completed (truncated run)"; exit 1; }
grep -q "TRANSPORT_ORDER=PASS" "$OUT" || { echo "FAIL: update order"; exit 1; }
grep -q "TEMPO=PASS"        "$OUT" || { echo "FAIL: tempo"; exit 1; }
grep -q "LOOP=PASS"         "$OUT" || { echo "FAIL: loop seam"; exit 1; }
grep -q "ELAPSED=PASS"      "$OUT" || { echo "FAIL: elapsed vs song position"; exit 1; }
grep -q "TEMPOCHANGE=PASS"  "$OUT" || { echo "FAIL: tempo change"; exit 1; }
grep -q "STATE=PASS"        "$OUT" || { echo "FAIL: play/pause/stop"; exit 1; }
# The pattern player is the audible half; assert it actually ran and produced
# audio, and that the count is real rather than a wait that merely expired.
[ "$(grep -c 'TR_ALIVE' "$OUT")" -ge 3 ] \
    || { echo "FAIL: fewer than 3 heartbeats -- loop() stopped or QEMU died early"; exit 1; }
grep -qE "TR_ALIVE bar=[0-9]+ beat=[0-9]+ peak=(0\.[0-9]*[1-9]|1\.)" "$OUT" \
    || { echo "FAIL: pattern player silent or not running"; exit 1; }
echo "PASS: TRANSPORT_ORDER TEMPO LOOP ELAPSED TEMPOCHANGE STATE"
```

Then `chmod 755 examples/audio/transport_test/run_qemu.sh` and `bash -n run_qemu.sh`.

- [ ] **Step 1.4: Verify the build fails for the right reason**

```bash
cd examples/audio/transport_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```

Expected: configure succeeds; the build **fails** with
`'AudioTransport' does not name a type` and consequent `'transport' was not
declared in this scope`. Any OTHER failure must be fixed first — the only red
allowed out of Task 1 is the missing class.

No commit — the example is committed green in Task 3.

---

## Task 2: The component — transport in the Audio repo

**Files:**
- Create: `~/Development/Audio/transport.h`
- Create: `~/Development/Audio/transport.cpp`
- Modify: `~/Development/Audio/Audio.h` (one include after `synth_acidbass.h`)

- [ ] **Step 2.1: Write the header**

`~/Development/Audio/transport.h`:

```cpp
/* Audio Library for Teensy - transport: tempo, song position and loop
 * Copyright (c) 2026, Nic Newdigate
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef transport_h_
#define transport_h_

#include <Arduino.h>
#include <AudioStream.h>

// Tempo, song position and a musical loop, advanced on the AUDIO BLOCK CLOCK.
//
// It is an AudioStream with no inputs and no outputs purely so update_all()
// calls update() once per block. That matters: the audio graph runs on its own
// clock and drifts against wall time (measured 0.81x wall under QEMU, 1.0x on
// silicon), so a transport ticked from loop() or an IntervalTimer would slide
// against the audio it is sequencing.
//
// TWO COUNTERS, TWO QUESTIONS -- do not confuse them:
//   samples()/seconds()  ELAPSED AUDIO TIME. Monotonic, unaffected by looping.
//   beats()/bars()/...   SONG POSITION. Derived from the tick phase, so it
//                        WRAPS at the loop seam.
// In the mulch Transport these are one number; a reader porting from it will
// expect seconds() to be the playhead, and here it is not.
class AudioTransport : public AudioStream
{
public:
	static const uint32_t PPQN = 96;        // pulses per quarter note; 4x MIDI clock's 24
	static const int MAX_TICKS_PER_BLOCK = 8;

	AudioTransport() : AudioStream(0, NULL) { recalc(); }

	// tempo & state -- USER CONTEXT ONLY (they take __disable_irq guards)
	void tempo(float bpm);
	void beatsPerBar(uint8_t n);
	void play();
	void pause();
	void stop();                            // rewinds to loop start, else to 0
	void rewindBar();
	void forwardBar();
	bool playing() const { return playing_; }
	float tempo() const  { return bpm_; }

	// loop -- bounds in BARS, so they survive tempo changes
	void loop(float startBar, float endBar);
	void looping(bool on);
	bool looping() const { return looping_; }

	// query -- see the two-counter note above
	uint64_t samples() const { return samplesElapsed_; }
	float seconds() const { return (float)samplesElapsed_ / AUDIO_SAMPLE_RATE_EXACT; }
	float beats() const   { return (float)(tickPhase_ >> 32) / (float)PPQN; }
	float bars() const    { return beats() / (float)beatsPerBar_; }
	int barNumber() const { return (int)bars() + 1; }
	int beatInBar() const { return (int)beats() % beatsPerBar_ + 1; }

	// THE SEQUENCER INTERFACE -- ticks that began during the block just processed
	int      tickCount() const { return tickCount_; }
	uint32_t tickAt(int i) const {
		return (i >= 0 && i < tickCount_) ? tickIdx_[i] : 0;
	}
	uint16_t tickOffsetAt(int i) const {
		return (i >= 0 && i < tickCount_) ? tickOff_[i] : 0;
	}
	bool tickOverflow() const { return overflow_; }   // >8 ticks in one block

	// external clock (MIDI sync scaffolding)
	void externalClock(bool on);
	void externalPulse();                   // one MIDI pulse = PPQN/24 ticks

	virtual void update(void);

private:
	void recalc();                          // tick increment per block, from bpm
	void emit(uint64_t from, uint64_t to, uint32_t sampleBase, uint32_t sampleSpan);
	static uint64_t barsToPhase(float bars, uint8_t bpb) {
		return (uint64_t)((double)bars * (double)bpb * (double)PPQN * 4294967296.0);
	}

	// 32.32 fixed point: upper 32 bits are the tick index, lower 32 the fraction.
	uint64_t tickPhase_   = 0;
	uint64_t tickInc_     = 0;              // ticks per block, 32.32
	uint64_t loopStart_   = 0;
	uint64_t loopEnd_     = 0;
	uint64_t samplesElapsed_ = 0;
	float    bpm_         = 120.0f;
	uint8_t  beatsPerBar_ = 4;
	bool     playing_     = false;
	bool     looping_     = false;
	bool     external_    = false;
	// per-block tick span
	uint32_t tickIdx_[MAX_TICKS_PER_BLOCK];
	uint16_t tickOff_[MAX_TICKS_PER_BLOCK];
	int      tickCount_ = 0;
	bool     overflow_  = false;
};

#endif
```

- [ ] **Step 2.2: Write the implementation**

`~/Development/Audio/transport.cpp`:

```cpp
/* Audio Library for Teensy - transport: tempo, song position and loop
 * Copyright (c) 2026, Nic Newdigate
 * MIT license -- see transport.h for the full text.
 */

#include "transport.h"

// Ticks advanced per block, in 32.32 fixed point:
//   ticks/block = PPQN * bpm * AUDIO_BLOCK_SAMPLES / (60 * sr)
// Computed in double and converted once per tempo change, so the per-block
// path is a single 64-bit add. Rounding is ~2.3e-10 tick per block, i.e. 2e-4
// tick over a million blocks -- inaudible over any session.
void AudioTransport::recalc() {
	double t = (double)PPQN * (double)bpm_ * (double)AUDIO_BLOCK_SAMPLES
	           / (60.0 * (double)AUDIO_SAMPLE_RATE_EXACT);
	tickInc_ = (uint64_t)(t * 4294967296.0);
}

void AudioTransport::tempo(float bpm) {
	if (bpm < 20.0f)  bpm = 20.0f;          // a zero/negative tempo can never
	if (bpm > 999.0f) bpm = 999.0f;         // reach the increment arithmetic
	__disable_irq();
	bpm_ = bpm;
	recalc();
	__enable_irq();
}

void AudioTransport::beatsPerBar(uint8_t n) {
	if (n < 1)  n = 1;
	if (n > 32) n = 32;
	__disable_irq();
	// Loop bounds are stored as a phase derived from the OLD beatsPerBar, so
	// they must be re-derived or a time-signature change would silently move
	// the loop. Recover the bar values first, then rebuild at the new signature.
	float startBar = (float)((double)loopStart_ / 4294967296.0 / (double)PPQN / (double)beatsPerBar_);
	float endBar   = (float)((double)loopEnd_   / 4294967296.0 / (double)PPQN / (double)beatsPerBar_);
	beatsPerBar_ = n;
	loopStart_ = barsToPhase(startBar, n);
	loopEnd_   = barsToPhase(endBar,   n);
	__enable_irq();
}

void AudioTransport::play()  { __disable_irq(); playing_ = true;  __enable_irq(); }
void AudioTransport::pause() { __disable_irq(); playing_ = false; __enable_irq(); }

void AudioTransport::stop() {
	__disable_irq();
	playing_   = false;
	tickPhase_ = looping_ ? loopStart_ : 0;
	tickCount_ = 0;
	__enable_irq();
}

void AudioTransport::rewindBar() {
	uint64_t bar = barsToPhase(1.0f, beatsPerBar_);
	__disable_irq();
	tickPhase_ = (tickPhase_ > bar) ? tickPhase_ - bar : 0;
	__enable_irq();
}

void AudioTransport::forwardBar() {
	uint64_t bar = barsToPhase(1.0f, beatsPerBar_);
	__disable_irq();
	tickPhase_ += bar;
	__enable_irq();
}

void AudioTransport::loop(float startBar, float endBar) {
	if (!(endBar > startBar)) return;       // ignore an invalid range
	if (startBar < 0.0f) startBar = 0.0f;
	uint64_t s = barsToPhase(startBar, beatsPerBar_);
	uint64_t e = barsToPhase(endBar,   beatsPerBar_);
	__disable_irq();
	loopStart_ = s;
	loopEnd_   = e;
	__enable_irq();
}

void AudioTransport::looping(bool on) { __disable_irq(); looping_ = on; __enable_irq(); }

void AudioTransport::externalClock(bool on) { __disable_irq(); external_ = on; __enable_irq(); }

void AudioTransport::externalPulse() {
	uint64_t pulse = ((uint64_t)(PPQN / 24)) << 32;   // 4 ticks at 96 PPQN
	__disable_irq();
	if (external_) tickPhase_ += pulse;
	__enable_irq();
}

// Record every tick boundary strictly inside (from, to], with its sample offset
// inside the block. `sampleBase`/`sampleSpan` describe which part of the block
// this segment covers, so a loop wrap can call this twice with the right offsets.
void AudioTransport::emit(uint64_t from, uint64_t to,
                          uint32_t sampleBase, uint32_t sampleSpan) {
	if (to <= from) return;
	uint64_t first = (from >> 32) + 1;
	uint64_t last  = to >> 32;
	for (uint64_t t = first; t <= last; t++) {
		if (tickCount_ >= MAX_TICKS_PER_BLOCK) { overflow_ = true; return; }
		uint64_t at = t << 32;
		// Where in this segment the boundary falls, scaled into samples.
		uint64_t num = (at - from) * (uint64_t)sampleSpan;
		uint64_t den = to - from;
		uint32_t off = sampleBase + (uint32_t)(den ? num / den : 0);
		if (off > AUDIO_BLOCK_SAMPLES - 1) off = AUDIO_BLOCK_SAMPLES - 1;
		tickIdx_[tickCount_] = (uint32_t)t;
		tickOff_[tickCount_] = (uint16_t)off;
		tickCount_++;
	}
}

void AudioTransport::update(void) {
	// Elapsed AUDIO time advances whether or not the transport is playing:
	// audio is still being produced, and this counter answers "how much".
	samplesElapsed_ += AUDIO_BLOCK_SAMPLES;
	tickCount_ = 0;
	overflow_  = false;
	if (!playing_ || external_) return;

	uint64_t from = tickPhase_;
	uint64_t to   = from + tickInc_;

	if (looping_ && loopEnd_ > loopStart_ && to >= loopEnd_ && from < loopEnd_) {
		// The block straddles the seam. Emit the two segments separately so
		// every boundary is counted exactly once and its sample offset stays
		// inside the block -- the seam neither loses nor duplicates a tick.
		uint64_t spanA = loopEnd_ - from;
		uint32_t samplesA = (uint32_t)((spanA * AUDIO_BLOCK_SAMPLES) / tickInc_);
		emit(from, loopEnd_, 0, samplesA ? samplesA : 1);
		// Carry the overshoot, exactly (integer modulo, no seam error).
		uint64_t len  = loopEnd_ - loopStart_;
		uint64_t over = (to - loopEnd_) % len;
		uint64_t newFrom = loopStart_;
		uint64_t newTo   = loopStart_ + over;
		emit(newFrom, newTo, samplesA,
		     AUDIO_BLOCK_SAMPLES > samplesA ? AUDIO_BLOCK_SAMPLES - samplesA : 1);
		tickPhase_ = newTo;
	} else {
		emit(from, to, 0, AUDIO_BLOCK_SAMPLES);
		tickPhase_ = to;
	}
}
```

- [ ] **Step 2.3: Register in Audio.h**

In `~/Development/Audio/Audio.h`, after the `#include "synth_acidbass.h"` line, add:

```cpp
#include "transport.h"
```

- [ ] **Step 2.4: Build the example — Task 1's test compiles**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/transport_test
cmake --build build
```

Expected: compiles and links → `build/transport_test.elf` (+ `.hex`). The
library root globs use `CONFIGURE_DEPENDS`, so the new .cpp is picked up
without a reconfigure.

- [ ] **Step 2.5: Commit the Audio repo**

```bash
cd ~/Development/Audio
git add transport.h transport.cpp Audio.h
git commit -m "transport: tempo, song position and loop on the audio block clock

An AudioStream with no inputs or outputs, so update_all() advances it once per
128-sample block -- the same clock that clocks the DAC. mulch's Transport
advances per GL frame, and this tree measured the audio graph at 0.81x wall
under QEMU and 1.0x on silicon, so a wall-clocked transport would slide against
the audio it sequences.

Musical position is a 32.32 fixed-point PPQN tick phase rather than a value
derived from the sample counter, because loop bounds are musical: ticks-per-bar
is tempo-independent, so a loop survives a tempo change exactly and the wrap is
integer modulo with no seam error. Elapsed audio time is a separate monotonic
sample counter -- the two answer different questions and diverge at the seam."
```

Do NOT push — Task 7 pushes after hardware verification.

---

## Task 3: QEMU gate green + commit the example

★ **Blocked if `~/Development/qemu2/build/qemu-system-arm` is missing.** Check
first; confirm with `examples/audio/tone_test/run_qemu_tone.sh` as a control.

- [ ] **Step 3.1: Run the gate**

```bash
cd examples/audio/transport_test
./run_qemu.sh
```

(`./run_qemu.sh`, never `sh run_qemu.sh` — it re-execs under gtimeout.)

Expected: the capture, then
`PASS: TRANSPORT_ORDER TEMPO LOOP ELAPSED TEMPOCHANGE STATE`, exit 0.

If a token FAILs, the `TR:` lines say which quantity missed. Debug with the
systematic-debugging skill; do not widen a band without first explaining the
measured value. Specific guidance:
- `TEMPO` off by a constant factor → check `recalc()`'s units.
- `LOOP` ticks low by ~1 per wrap → an off-by-one in `emit()`'s half-open
  range. This is the defect the assertion exists to catch; fix `emit`, do not
  loosen the 2% band.
- `ELAPSED` failing → the two query families have been collapsed.

- [ ] **Step 3.2: Run it 5 times and record the spread**

```bash
for i in 1 2 3 4 5; do ./run_qemu.sh >/tmp/tr$i.log 2>&1; \
  echo "run$i exit=$? $(grep -E 'ticks400|ticks_loop' /tmp/tr$i.log | tr '\n' ' ')"; done
```

All five must pass. If any value moves between runs, the assertion is not
purely audio-clock-referenced — find the wall-clock dependency before
proceeding. (Nominal `ticks400` is 223; `ticks_loop` 669.)

- [ ] **Step 3.3: Save the committed transcript fixture**

```bash
cp build/transport.uart transcript_qemu.txt
```

(Under `build/` because the gate routes artifacts through `gate_capture_path`.)

- [ ] **Step 3.4: Commit the example**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
git add examples/audio/transport_test/transport_test.cpp \
        examples/audio/transport_test/CMakeLists.txt \
        examples/audio/transport_test/run_qemu.sh \
        examples/audio/transport_test/transcript_qemu.txt
git commit -m "audio: transport_test -- AudioTransport example + QEMU token gate

Every assertion compares audio-clock quantities on both sides, so none of them
depends on how fast the host runs the guest. The loop check is the load-bearing
one: wrapping changes WHICH tick indices appear, never HOW MANY boundaries are
crossed, so an off-by-one at the seam shows up as a tick-count error. ELAPSED
pins song position and elapsed audio time as genuinely different quantities --
without it a refactor could collapse them and every other assertion would still
pass. loop() plays four-on-the-floor for the hardware session."
```

---

## Task 4: License audit GATES entry

**Files:**
- Modify: `tools/license-audit.sh` (the `GATES` list)

- [ ] **Step 4.1: Add the entry**

In the `GATES=` list, insert alphabetically in the audio cluster (after
`examples/audio/tone_test:tone_test`):

```
examples/audio/transport_test:transport_test \
```

- [ ] **Step 4.2: Run the audit**

```bash
bash -n tools/license-audit.sh && ./tools/license-audit.sh 2>&1 | tail -3
```

Expected: `LICENSE-AUDIT: PASS`, with `examples/audio/transport_test` appearing
in the depfile walk. New sources are MIT; no `ALLOW` change is needed. If the
audit fails naming a transport path, read the finding — do not add a
`GATES_EXEMPT`.

- [ ] **Step 4.3: Commit**

```bash
git add tools/license-audit.sh
git commit -m "tools: license-audit GATES entry for transport_test"
```

---

## Task 5: Full sweep + baseline

★ Blocked while the QEMU binary is missing.

- [ ] **Step 5.1: Read `docs/KNOWN-BROKEN-GATES.md`** — the CLAUDE.md rule
  before any sweep. Note the resolved WM8962 entry so its reds are not
  re-investigated.

- [ ] **Step 5.2: Build every gate-owning example, then sweep**

A missing ELF is reported as SKIP, not failure, so a non-zero SKIP means the
sweep measured less than it claims.

```bash
./tools/run-all-qemu-gates.sh 2>&1 | tail -5
```

Expected: **91 passed, 0 failed, 0 SKIP** (90 before this example), or 90/1/0
when the documented nondeterministic `dualcore/cm4_audio_test` is red — re-run
that one idle before believing it.

- [ ] **Step 5.3: Update the CLAUDE.md baseline**

Change `**90 gates**` to `**91 gates**`, prepend `90 before the transport added
audio/transport_test;` to the history parenthetical, and update the target line
to `**91 passed, 0 failed, 0 SKIP**` / `**90 passed, 1 failed, 0 SKIP**`.
Replace the "not re-measured" caveat added on 2026-08-15 with the numbers this
sweep actually printed. **The numbers you write must be the ones you measured.**

- [ ] **Step 5.4: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: sweep baseline 90 -> 91 (transport_test), measured"
```

---

## Task 6: Hardware verification

★ Needs the board and the user present.

- [ ] **Step 6.1: Flash — VCOM-free**

```bash
pkill -f rt1170-console.py; pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
cd examples/audio/transport_test
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/transport_test.elf
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build/transport_test.elf
```

`status 131` / `LOAD_EXIT=255` means something holds the VCOM — detach, pkill
the daemons, retry.

- [ ] **Step 6.2: Attach the reader, then ask the user to press SW4**

```bash
(python3 ../../../tools/rt1170-console.py /dev/cu.usbmodem* 115200 > /tmp/tr_hw.txt 2>&1 &)
```

★ **Do NOT trigger the reset with `LinkServer run` while the reader is
attached.** That re-enumerates the VCOM and has kernel-panicked this host three
times (IOSerialFamily use-after-free, `python3.12` the panicking task). Ask the
user to press **SW4** instead.

- [ ] **Step 6.3: Listening check (user)**

Headphones on J101. Ask the user to confirm:
1. a steady four-on-the-floor is audible;
2. the first beat of each bar is accented;
3. the tempo is steady — no drift or stumble at the loop seam;
4. the loop is seamless, with no gap or double-hit where it wraps.

Point 4 is the one QEMU cannot fully settle: the tick-count assertion proves no
boundary is lost, but only the ear proves the seam is musically clean.

- [ ] **Step 6.4: Stop the reader, write the transcript**

Detach BEFORE any further LinkServer use. Trim to boot + tokens + a dozen
heartbeats, and annotate the user's verdict as comment lines at the top,
matching `examples/audio/acid_bass_test/transcript_hw_evkb.txt`.

- [ ] **Step 6.5: Commit**

```bash
git add examples/audio/transport_test/transcript_hw_evkb.txt
git commit -m "audio: transport_test hardware transcript -- audible on the EVKB"
```

---

## Task 7: Push Audio, bump the pin, close out

- [ ] **Step 7.1: Review and push**

```bash
cd ~/Development/Audio
git log origin/master..master --oneline    # must be only the transport commit
git push
git rev-parse HEAD
```

Confirm with the user before pushing if anything unrelated is on master.

- [ ] **Step 7.2: Bump the pin**

In `evkb.cmake`, replace the Audio SHA on the `teensy_declare_library(Audio …)`
line with the SHA from Step 7.1.

- [ ] **Step 7.3: Prove the fresh-user path**

```bash
cd examples/audio/transport_test
cmake -B build-forcefetch -DEVKB_FORCE_FETCH=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build-forcefetch
rm -rf build-forcefetch
```

Expected: builds clean — a clone with no local Audio checkout gets the class
from the pinned SHA.

- [ ] **Step 7.4: Commit and push evkb**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
git add evkb.cmake
git commit -m "build: bump Audio pin for transport"
git push
```

---

## Self-review notes (already applied)

- **Spec coverage**: §1 architecture → Task 2.1/2.2 (AudioStream, two counters,
  32.32 ticks); §2 API → 2.1, with the two-query-family note in the header;
  §2 concurrency → `__disable_irq` on every user-context setter; §2 update
  ordering → determined from `AudioStream.h:147-154` and asserted at runtime by
  `TRANSPORT_ORDER`; §3 gates → Task 1.3 with one assertion per spec bullet
  including `ELAPSED`; bookkeeping → Tasks 4, 5, 7.
- **Ambiguity fixed while writing**: `beatsPerBar()` must re-derive the loop
  bounds, because they are stored as a phase computed from the *old* signature.
  The spec did not say this and a naive implementation would silently move the
  loop on a time-signature change.
- **Type consistency**: `tickCount()`/`tickAt()`/`tickOffsetAt()`/`samples()`/
  `bars()` are spelled identically in the sketch (Task 1), the header (Task 2)
  and the spec.
- **YAGNI honored**: no sequencer, no swing, no tempo ramps, no MIDI clock
  output, no mid-song time-signature automation.
