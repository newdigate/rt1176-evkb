# AudioStepSequencer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A 16-step TB-303-style sequencer (`AudioStepSequencer`) in the Audio sibling repo, driven by `AudioTransport`'s per-block tick span, proven by a new two-gate example `examples/audio/step_seq_test`.

**Architecture:** An `AudioStream` with 0 inputs and 0 outputs, holding a reference to an `AudioTransport` taken in its constructor. Each block it folds the transport's tick indices into the pattern, emits ordered note events into a pull-based queue, and a drainer applies them to `AudioSynthAcidBass`.

**Tech Stack:** Teensy Audio library conventions (`AudioStream`, 128-sample blocks, 96 PPQN), evkb CMake harness (`import_evkb_audio_full`), `tools/gate-lib.sh` QEMU gates, LinkServer for hardware.

**Spec:** `docs/superpowers/specs/2026-08-15-step-sequencer-design.md` (approved 2026-08-15).

**Repos touched:** `~/Development/Audio` (component) and this repo (example, gate, bookkeeping). Audio-repo work lands first; the `evkb.cmake` pin bump is the LAST task, after the Audio push.

---

## ★ Read before starting

**1. The two components this builds on are done, pushed and pinned.**
`AudioSynthAcidBass` and `AudioTransport` are both hardware-verified. Do not
modify either. If you become convinced a fix belongs in one of them, STOP and
report rather than editing.

**2. The transport's exact public API** — use these spellings, they are checked:

```cpp
static const uint32_t PPQN = 96;
static const uint32_t MIN_LOOP_TICKS = 8;
bool     playing() const;         float    tempo() const;
uint8_t  beatsPerBar() const;     bool     looping() const;
uint64_t samples() const;         float    beats() const;   float bars() const;
uint32_t ticksPerBar() const;     uint32_t loopTicks() const;
uint32_t loopStartTick() const;
int      tickCount() const;       uint32_t tickAt(int) const;
uint16_t tickOffsetAt(int) const; bool     tickOverflow() const;
bool     wrapped() const;
```

**3. Every gate assertion must compare audio-clock quantities on BOTH sides.**
`transport_test` produced *identical* numbers on QEMU and silicon because of
this; `acid_bass_test` flaked four assertions for want of it. Read
`examples/audio/transport_test/transport_test.cpp`'s `waitBlocks()` before
writing any assertion here.

**4. A connectionless `AudioStream` never updates.** `AudioStream.h:140` sets
`active = false`, only `AudioConnection::connect()` sets it true
(`AudioStream.cpp:222,225`), and `software_isr` skips inactive nodes (`:323`).
`AudioStepSequencer` has no audio connections, so its constructor **must** set
`active = true` or nothing runs and every assertion fails at once.

**5. `master` is 12 commits ahead of origin and unpushed** as of writing. Not
your concern, but do not be surprised by it, and do not push without being
asked.

---

## Task 1: The failing test — example + gate that cannot build yet

**Files:**
- Create: `examples/audio/step_seq_test/step_seq_test.cpp`
- Create: `examples/audio/step_seq_test/CMakeLists.txt`
- Create: `examples/audio/step_seq_test/run_qemu.sh` (mode 755)

No `boards` sidecar — rt1176 only.

- [ ] **Step 1.1: Write the sketch**

`examples/audio/step_seq_test/step_seq_test.cpp`:

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include <math.h>
#include <Audio.h>
#include "control_wm8962.h"   // Audio.h includes control_wm8960.h but NOT this one

#define CONSOLE Serial1

// ★ No `#define PPQN` here. transport.h declares the resolution as a class
// constant and a macro of that name would rewrite the library's own
// declaration into `static const uint32_t 96 = 96;`. Use AudioTransport::PPQN.

// The sequencer takes the transport by reference, so `transport` MUST be
// constructed first -- which is exactly the update-order requirement the
// transport documents, made structural. You cannot write these two lines in
// the wrong order and have it compile.
AudioTransport     transport;
AudioStepSequencer seq(transport);
AudioSynthAcidBass acid;
AudioAnalyzePeak   peak;
AudioOutputI2S     out;
AudioConnection    cPk(acid, 0, peak, 0);
AudioConnection    cL (acid, 0, out,  0);
AudioConnection    cR (acid, 0, out,  1);
AudioControlWM8962 wm;

// ---- event capture -------------------------------------------------------
// The gate needs to inspect the ORDER of emitted events, not just their count,
// so the drainer records them into a ring before applying them. Recording and
// applying in one pass is deliberate: it is exactly what a real consumer does,
// so the log cannot drift from what the voice actually heard.
struct LogEntry { uint8_t type, note, velocity; bool slide; uint16_t offset; };
static const int LOG_MAX = 256;
static LogEntry logBuf[LOG_MAX];
static volatile int logCount = 0;

static void drainSequencer(void) {
    SeqEvent ev[8]; int n;
    __disable_irq();                     // user context: update() can retire the queue
    n = seq.eventCount();
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) ev[i] = seq.eventAt(i);
    __enable_irq();
    for (int i = 0; i < n; i++) {
        if (logCount < LOG_MAX) {
            logBuf[logCount].type     = ev[i].type;
            logBuf[logCount].note     = ev[i].note;
            logBuf[logCount].velocity = ev[i].velocity;
            logBuf[logCount].slide    = ev[i].slide;
            logBuf[logCount].offset   = ev[i].sampleOffset;
            logCount++;
        }
        if (ev[i].type == SEQ_NOTE_ON) acid.noteOn(ev[i].note, ev[i].velocity, ev[i].slide);
        else                           acid.noteOff(ev[i].note);
    }
}

// Advance `n` audio blocks, draining every block. Block-referenced, not
// wall-clock: transport.samples() steps by exactly AUDIO_BLOCK_SAMPLES per
// update(), so a change in it IS a block tick. A delay()-based loop here would
// make every measurement below depend on how fast the host runs the guest.
static void runBlocks(int n) {
    uint64_t prev = transport.samples();
    int seen = 0;
    uint32_t t0 = millis();
    while (seen < n && millis() - t0 < 20000) {
        uint64_t s = transport.samples();
        if (s != prev) { seen++; prev = s; drainSequencer(); }
        yield();
    }
}

static void resetLog(void) { __disable_irq(); logCount = 0; __enable_irq(); }
static int  countType(uint8_t t) {
    int c = 0; for (int i = 0; i < logCount; i++) if (logBuf[i].type == t) c++; return c;
}

void setup() {
    CONSOLE.begin(115200);
    while (!CONSOLE) {}
    CONSOLE.println("STEPSEQ-GATE v1");
    AudioMemory(16);
    wm.enable();
    wm.volume(0.6f);
    acid.cutoff(700.0f); acid.resonance(0.7f); acid.envMod(0.6f);
    acid.decay(0.15f); acid.level(0.7f);

    transport.tempo(120.0f);
    transport.loop(0.0f, 1.0f);          // 384 ticks == the pattern length
    transport.looping(true);

    // --- STEPS: a known pattern emits exactly the expected note-ons ---------
    // 4 gated steps out of 16. One pattern = 384 ticks = 2 s at 120 BPM =
    // 689.06 blocks. Running 1378 blocks is 2 patterns, so 8 note-ons.
    // Both sides are audio-clock quantities: the block count is counted, and
    // the expectation is derived from ticks, so host speed cannot move it.
    seq.clear();
    seq.step(0,  33, true,  true,  false);   // accented
    seq.step(4,  33, true,  false, false);
    seq.step(8,  36, true,  false, false);
    seq.step(12, 33, true,  false, false);
    transport.stop(); resetLog(); transport.play();
    runBlocks(1378);
    int onsA  = countType(SEQ_NOTE_ON);
    int offsA = countType(SEQ_NOTE_OFF);
    CONSOLE.print("SQ: ons=");  CONSOLE.println(onsA);
    CONSOLE.print("SQ: offs="); CONSOLE.println(offsA);
    // 8 expected; allow +/-1 for where the run starts and stops relative to a
    // step boundary. Nothing wider: a step-derivation bug moves this by 4x.
    bool stepsOk = (onsA >= 7 && onsA <= 9) && (offsA >= 6 && offsA <= 9);
    CONSOLE.println(stepsOk ? "STEPS=PASS" : "STEPS=FAIL");

    // --- ACCENT: accented steps carry 127, unaccented 80 -------------------
    int v127 = 0, v80 = 0, vOther = 0;
    for (int i = 0; i < logCount; i++) {
        if (logBuf[i].type != SEQ_NOTE_ON) continue;
        if      (logBuf[i].velocity == 127) v127++;
        else if (logBuf[i].velocity == 80)  v80++;
        else                                vOther++;
    }
    CONSOLE.print("SQ: v127="); CONSOLE.print(v127);
    CONSOLE.print(" v80=");     CONSOLE.print(v80);
    CONSOLE.print(" vother=");  CONSOLE.println(vOther);
    // One accented step in four, so roughly a quarter of the note-ons.
    bool accentOk = v127 >= 1 && v80 >= 3 && vOther == 0;
    CONSOLE.println(accentOk ? "ACCENT=PASS" : "ACCENT=FAIL");

    // --- ORDER: a slide step emits the next NOTE_ON before the NOTE_OFF ----
    // This is the load-bearing one. It inspects emission ORDER directly, and
    // it is the property the whole slide feature rests on: the voice glides
    // only if the new note is pressed while the old one is still held.
    // Pattern: step 0 slides into step 4.
    seq.clear();
    seq.step(0, 33, true, false, true);      // slide
    seq.step(4, 45, true, false, false);
    transport.stop(); resetLog(); transport.play();
    runBlocks(1378);
    // Find the first NOTE_ON of note 45 and check what precedes/follows it.
    int idx45 = -1;
    for (int i = 0; i < logCount; i++)
        if (logBuf[i].type == SEQ_NOTE_ON && logBuf[i].note == 45) { idx45 = i; break; }
    bool slideFlag = (idx45 >= 0) && logBuf[idx45].slide;
    // The note-off for 33 must come AFTER the note-on for 45, not before.
    int idxOff33 = -1;
    for (int i = idx45 + 1; i >= 0 && i < logCount; i++)
        if (logBuf[i].type == SEQ_NOTE_OFF && logBuf[i].note == 33) { idxOff33 = i; break; }
    CONSOLE.print("SQ: idx_on45=");  CONSOLE.println(idx45);
    CONSOLE.print("SQ: idx_off33="); CONSOLE.println(idxOff33);
    CONSOLE.print("SQ: slide_flag="); CONSOLE.println(slideFlag ? 1 : 0);
    bool orderOk = (idx45 >= 0) && (idxOff33 > idx45) && slideFlag;
    CONSOLE.println(orderOk ? "ORDER=PASS" : "ORDER=FAIL");

    // --- REST: a gate=false step is silent, and the step AFTER it fires -----
    // An off-by-one in step indexing breaks precisely this and nothing else.
    seq.clear();
    seq.step(0, 33, true,  false, false);
    seq.step(4, 40, false, false, false);    // REST -- must not sound
    seq.step(8, 45, true,  false, false);    // must still fire
    transport.stop(); resetLog(); transport.play();
    runBlocks(1378);
    int n33 = 0, n40 = 0, n45 = 0;
    for (int i = 0; i < logCount; i++) {
        if (logBuf[i].type != SEQ_NOTE_ON) continue;
        if (logBuf[i].note == 33) n33++;
        if (logBuf[i].note == 40) n40++;
        if (logBuf[i].note == 45) n45++;
    }
    CONSOLE.print("SQ: n33="); CONSOLE.print(n33);
    CONSOLE.print(" n40=");    CONSOLE.print(n40);
    CONSOLE.print(" n45=");    CONSOLE.println(n45);
    bool restOk = (n40 == 0) && (n33 >= 1) && (n45 >= 1);
    CONSOLE.println(restOk ? "REST=PASS" : "REST=FAIL");

    // --- WRAP: the pattern restarts at step 0 on a loop wrap ---------------
    // Only step 0 is gated, so every note-on is one pattern apart. Two
    // patterns must give two note-ons -- if the wrap reset the fold wrongly,
    // this reads 1 or 3.
    seq.clear();
    seq.step(0, 33, true, false, false);
    transport.stop(); resetLog(); transport.play();
    runBlocks(1378);
    int wrapOns = countType(SEQ_NOTE_ON);
    CONSOLE.print("SQ: wrap_ons="); CONSOLE.println(wrapOns);
    bool wrapOk = (wrapOns >= 2 && wrapOns <= 3);
    CONSOLE.println(wrapOk ? "WRAP=PASS" : "WRAP=FAIL");

    // --- SHORTLOOP: a loop shorter than the pattern leaves the tail unused --
    // Pins the fold's defined behaviour: steps beyond the loop NEVER fire.
    // Without this a future change could turn "unused tail" into "compressed
    // pattern" and every other assertion here would still pass.
    // Loop = 0.5 bar = 192 ticks = steps 0..7 only.
    seq.clear();
    seq.step(0,  33, true, false, false);     // inside the loop
    seq.step(12, 50, true, false, false);     // OUTSIDE it -- must never sound
    transport.stop();
    transport.loop(0.0f, 0.5f);
    resetLog(); transport.play();
    runBlocks(1378);
    int inLoop = 0, outLoop = 0;
    for (int i = 0; i < logCount; i++) {
        if (logBuf[i].type != SEQ_NOTE_ON) continue;
        if (logBuf[i].note == 33) inLoop++;
        if (logBuf[i].note == 50) outLoop++;
    }
    CONSOLE.print("SQ: in_loop=");  CONSOLE.print(inLoop);
    CONSOLE.print(" out_loop=");    CONSOLE.println(outLoop);
    bool shortOk = (outLoop == 0) && (inLoop >= 3);
    CONSOLE.println(shortOk ? "SHORTLOOP=PASS" : "SHORTLOOP=FAIL");

    CONSOLE.println("STEPSEQ-DONE");

    // Leave a real acid pattern running for the hardware session.
    transport.stop();
    transport.loop(0.0f, 1.0f);
    seq.clear();
    seq.step(0,  33, true,  true,  false);
    seq.step(2,  33, true,  false, false);
    seq.step(3,  45, true,  false, true );    // slide into step 4
    seq.step(4,  36, true,  true,  false);
    seq.step(6,  33, true,  false, false);
    seq.step(8,  40, true,  false, true );    // slide into step 9
    seq.step(9,  33, true,  false, false);
    seq.step(11, 31, true,  true,  false);
    seq.step(14, 33, true,  false, false);
    transport.play();
}

void loop() {
    static uint32_t lastBeat = 0;
    drainSequencer();
    uint32_t now = millis();
    if (lastBeat == 0) lastBeat = now;       // seed: else heartbeat 1 predates any audio
    if (now - lastBeat >= 250) {
        lastBeat = now;
        if (peak.available()) {
            CONSOLE.print("SQ_ALIVE step="); CONSOLE.print(seq.currentStep());
            CONSOLE.print(" beat=");         CONSOLE.print(transport.beatInBar());
            CONSOLE.print(" peak=");         CONSOLE.println(peak.read(), 4);
        } else {
            CONSOLE.println("SQ_ALIVE peak=(no update)");
        }
    }
}
```

- [ ] **Step 1.2: Write the CMakeLists**

`examples/audio/step_seq_test/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(step_seq_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
import_evkb_audio_full()

teensy_add_executable(step_seq_test step_seq_test.cpp)
teensy_target_link_libraries(step_seq_test cores Audio Wire SPI SdFat SD SerialFlash)
# CMSIS-DSP is a plain STATIC lib (no `.o` suffix on the target name), so it
# links via raw target_link_libraries -- same as transport_test / acid_bass_test.
target_link_libraries(step_seq_test.elf CMSIS-DSP stdc++ m)
```

- [ ] **Step 1.3: Write the gate script**

`examples/audio/step_seq_test/run_qemu.sh`:

```sh
#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location.
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
# The script waits on AUDIO blocks rather than wall-clock delays, which is what
# makes its numbers host-speed-independent -- but it also means a loaded host
# stretches the run in wall time. 120 s and 300 poll iterations give that room.
# No assertion was relaxed to fit. Five measurement runs of ~1378 blocks each.
QRUN_TIMEOUT="${QRUN_TIMEOUT:-120}"; export QRUN_TIMEOUT
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/step_seq_test.elf"
OUT=$(gate_capture_path "$DIR" step_seq.uart)
DBG=$(gate_capture_path "$DIR" step_seq.dbg)
rm -f "$OUT" "$DBG"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
for _ in $(seq 1 300); do
    kill -0 "$P" 2>/dev/null || break
    n=$(grep -c 'SQ_ALIVE' "$OUT" 2>/dev/null || true)
    [ "${n:-0}" -ge 3 ] && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "STEPSEQ-GATE v1" "$OUT" || { echo "FAIL: banner"; exit 1; }
grep -q "STEPSEQ-DONE"    "$OUT" || { echo "FAIL: script never completed (truncated run)"; exit 1; }
grep -q "STEPS=PASS"      "$OUT" || { echo "FAIL: step count"; exit 1; }
grep -q "ACCENT=PASS"     "$OUT" || { echo "FAIL: accent velocity"; exit 1; }
grep -q "ORDER=PASS"      "$OUT" || { echo "FAIL: slide event order"; exit 1; }
grep -q "REST=PASS"       "$OUT" || { echo "FAIL: rest handling"; exit 1; }
grep -q "WRAP=PASS"       "$OUT" || { echo "FAIL: loop wrap"; exit 1; }
grep -q "SHORTLOOP=PASS"  "$OUT" || { echo "FAIL: short loop tail"; exit 1; }
# The pattern player is the audible half; assert it ran AND produced audio, and
# that the count is real rather than a wait that merely expired.
[ "$(grep -c 'SQ_ALIVE' "$OUT")" -ge 3 ] \
    || { echo "FAIL: fewer than 3 heartbeats -- loop() stopped or QEMU died early"; exit 1; }
grep -qE "SQ_ALIVE step=[0-9]+ beat=[0-9]+ peak=(0\.[0-9]*[1-9]|1\.)" "$OUT" \
    || { echo "FAIL: pattern player silent or not running"; exit 1; }
echo "PASS: STEPS ACCENT ORDER REST WRAP SHORTLOOP"
```

Then `chmod 755 examples/audio/step_seq_test/run_qemu.sh` and `bash -n run_qemu.sh`.

- [ ] **Step 1.4: Verify the build fails for the right reason**

```bash
cd examples/audio/step_seq_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```

Expected: configure succeeds; the build **fails** with `'AudioStepSequencer'
does not name a type`, plus consequent errors on `seq`, `SeqEvent` and
`SEQ_NOTE_ON`. Those three are expected fallout of the missing header, not
separate problems. `AudioTransport`, `AudioSynthAcidBass`, `AudioAnalyzePeak`,
`AudioOutputI2S` and `AudioControlWM8962` must all resolve — if any of those
fails, the Audio checkout is wrong and you should report that rather than work
around it.

No commit — the example is committed green in Task 3.

---

## Task 2: The component — seq_step in the Audio repo

**Files:**
- Create: `~/Development/Audio/seq_step.h`
- Create: `~/Development/Audio/seq_step.cpp`
- Modify: `~/Development/Audio/Audio.h` (one include after `transport.h`)

- [ ] **Step 2.1: Write the header**

`~/Development/Audio/seq_step.h`:

```cpp
/* Audio Library for Teensy - 16-step TB-303 style sequencer
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

#ifndef seq_step_h_
#define seq_step_h_

#include <Arduino.h>
#include <AudioStream.h>
#include "transport.h"

#define SEQ_NOTE_OFF 0
#define SEQ_NOTE_ON  1

// One pattern step -- the four things a TB-303 stores.
struct AcidStep {
	uint8_t note   = 33;      // 0..127
	bool    gate   = false;   // false = rest
	bool    accent = false;
	bool    slide  = false;   // tie into the next step
};

// One emitted event. sampleOffset says WHERE in the block the step falls; the
// drainer may apply it at block granularity (AudioSynthAcidBass has no offset
// parameter) but the queue does not throw the information away.
struct SeqEvent {
	uint8_t  type         = SEQ_NOTE_OFF;
	uint8_t  note         = 0;
	uint8_t  velocity     = 0;
	bool     slide        = false;
	uint16_t sampleOffset = 0;
};

// 16-step TB-303 style sequencer, clocked by an AudioTransport.
//
// ★ THE TRANSPORT IS TAKEN BY REFERENCE IN THE CONSTRUCTOR, AND THAT IS THE
// POINT. AudioTransport requires its consumers to be declared AFTER it --
// update order is construction order (AudioStream.h:147-154 appends at the
// tail, software_isr walks head-first), so a node declared earlier reads the
// PREVIOUS block's tick span, a silent 2.9 ms staleness. transport.h can only
// warn about that in prose. Binding by reference makes it unrepresentable:
// you cannot pass a reference to an object that has not been constructed.
//
//     AudioTransport     transport;
//     AudioStepSequencer seq(transport);   // will not compile in the wrong order
//
// ★ `active = true` in the constructor is NOT boilerplate. AudioStream's
// constructor sets active = false (AudioStream.h:140) and only
// AudioConnection::connect() sets it true (AudioStream.cpp:222,225);
// software_isr skips every inactive node (AudioStream.cpp:323). This node has
// no audio connections, so without that line update() is never called at all.
class AudioStepSequencer : public AudioStream
{
public:
	static const int STEPS          = 16;
	static const int TICKS_PER_STEP = AudioTransport::PPQN / 4;   // 24, a 16th
	static const int PATTERN_TICKS  = STEPS * TICKS_PER_STEP;     // 384, one 4/4 bar
	static const int MAX_EVENTS     = 8;

	AudioStepSequencer(AudioTransport &t)
		: AudioStream(0, NULL), transport_(t) { active = true; }

	// pattern -- USER CONTEXT ONLY (these take __disable_irq guards)
	void step(int i, uint8_t note, bool gate, bool accent, bool slide);
	AcidStep step(int i) const;
	void clear();
	void gateLength(float fraction);      // clamped 0.1..0.9, default 0.5
	void accentVelocity(uint8_t v);       // default 127
	void normalVelocity(uint8_t v);       // default 80

	// -1 until the first step fires, so "nothing has played yet" is
	// distinguishable from "step 0 played".
	int currentStep() const { return currentStep_; }

	// THE EVENT QUEUE -- events emitted during the block just processed.
	// Same contract as AudioTransport's tick span, deliberately: valid only for
	// that block, stable inside update_all()'s walk, and a USER-CONTEXT reader
	// must snapshot under __disable_irq() because update() rebuilds it.
	int             eventCount() const { return eventCount_; }
	const SeqEvent &eventAt(int i) const {
		return (i >= 0 && i < eventCount_) ? events_[i] : empty_;
	}
	bool eventOverflow() const { return overflow_; }

	virtual void update(void);

private:
	void emitOn(const AcidStep &s, uint16_t offset);
	void emitOff(uint8_t note, uint16_t offset);
	void push(uint8_t type, uint8_t note, uint8_t vel, bool slide, uint16_t offset);

	AudioTransport &transport_;
	AcidStep  pattern_[STEPS];
	SeqEvent  events_[MAX_EVENTS];
	SeqEvent  empty_;                       // returned for an out-of-range index
	volatile int  eventCount_ = 0;
	volatile bool overflow_   = false;
	volatile int  currentStep_ = -1;
	uint8_t  gateTicks_    = TICKS_PER_STEP / 2;   // 12 == 0.5 gate
	uint8_t  accentVel_    = 127;
	uint8_t  normalVel_    = 80;
	// sounding-note state
	int      heldNote_     = -1;            // -1 = nothing sounding
	bool     heldSlide_    = false;         // the sounding step had slide set
	uint32_t heldOffTick_  = 0;             // folded tick at which to release
	bool     heldPending_  = false;         // a release is still owed
};

#endif
```

- [ ] **Step 2.2: Write the implementation**

`~/Development/Audio/seq_step.cpp`:

```cpp
/* Audio Library for Teensy - 16-step TB-303 style sequencer
 * Copyright (c) 2026, Nic Newdigate
 * MIT license -- see seq_step.h for the full text.
 */

#include "seq_step.h"

void AudioStepSequencer::step(int i, uint8_t note, bool gate, bool accent, bool slide) {
	if (i < 0 || i >= STEPS) return;        // ignore an out-of-range index
	if (note > 127) note = 127;
	__disable_irq();
	pattern_[i].note   = note;
	pattern_[i].gate   = gate;
	pattern_[i].accent = accent;
	pattern_[i].slide  = slide;
	__enable_irq();
}

AcidStep AudioStepSequencer::step(int i) const {
	if (i < 0 || i >= STEPS) return AcidStep();   // a rest
	return pattern_[i];
}

void AudioStepSequencer::clear() {
	__disable_irq();
	for (int i = 0; i < STEPS; i++) pattern_[i] = AcidStep();
	currentStep_ = -1;
	heldNote_ = -1; heldPending_ = false;
	__enable_irq();
}

void AudioStepSequencer::gateLength(float fraction) {
	// Clamped so a gate can neither be zero-length nor swallow the next step.
	if (!(fraction >= 0.1f)) fraction = 0.1f;     // also catches NaN
	if (fraction > 0.9f)     fraction = 0.9f;
	uint8_t t = (uint8_t)(fraction * TICKS_PER_STEP + 0.5f);
	if (t < 1) t = 1;
	__disable_irq();
	gateTicks_ = t;
	__enable_irq();
}

void AudioStepSequencer::accentVelocity(uint8_t v) {
	if (v > 127) v = 127;
	__disable_irq(); accentVel_ = v; __enable_irq();
}

void AudioStepSequencer::normalVelocity(uint8_t v) {
	if (v > 127) v = 127;
	__disable_irq(); normalVel_ = v; __enable_irq();
}

void AudioStepSequencer::push(uint8_t type, uint8_t note, uint8_t vel,
                              bool slide, uint16_t offset) {
	if (eventCount_ >= MAX_EVENTS) { overflow_ = true; return; }
	events_[eventCount_].type         = type;
	events_[eventCount_].note         = note;
	events_[eventCount_].velocity     = vel;
	events_[eventCount_].slide        = slide;
	events_[eventCount_].sampleOffset = offset;
	eventCount_++;
}

void AudioStepSequencer::emitOn(const AcidStep &s, uint16_t offset) {
	push(SEQ_NOTE_ON, s.note, s.accent ? accentVel_ : normalVel_, heldSlide_, offset);
}

void AudioStepSequencer::emitOff(uint8_t note, uint16_t offset) {
	push(SEQ_NOTE_OFF, note, 0, false, offset);
}

void AudioStepSequencer::update(void) {
	eventCount_ = 0;
	overflow_   = false;

	// A wrap means the playhead jumped backward. Drop the pending release: the
	// tick it was waiting for will not arrive in order, and a note left held
	// across a restart is the one failure a listener notices immediately.
	if (transport_.wrapped() && heldPending_) {
		emitOff((uint8_t)heldNote_, 0);
		heldNote_ = -1; heldPending_ = false; heldSlide_ = false;
	}

	const uint32_t loopTicks = transport_.loopTicks();
	if (loopTicks == 0) return;             // not reachable via loop(); guard anyway

	for (int i = 0; i < transport_.tickCount(); i++) {
		// FOLD the absolute tick into the loop before deriving a step. An
		// unfolded index runs ...383, 384, 1, 2... across the seam and yields
		// FIVE quarters per bar -- the audible flam already found in
		// acid_bass_test. This is the idiom transport.h documents.
		uint32_t abs    = transport_.tickAt(i);
		uint32_t base   = transport_.loopStartTick();
		uint32_t folded = (abs >= base) ? ((abs - base) % loopTicks)
		                                : ((loopTicks - ((base - abs) % loopTicks)) % loopTicks);
		uint16_t off    = transport_.tickOffsetAt(i);

		// Release first, unless the sounding step slid -- a slide emits NO
		// note-off at all, which is what makes the voice glide.
		if (heldPending_ && !heldSlide_ && folded == heldOffTick_) {
			emitOff((uint8_t)heldNote_, off);
			heldNote_ = -1; heldPending_ = false;
		}

		if (folded % TICKS_PER_STEP != 0) continue;    // not a step boundary
		int idx = (int)((folded / TICKS_PER_STEP) % STEPS);
		const AcidStep &s = pattern_[idx];
		currentStep_ = idx;
		if (!s.gate) {
			// A rest still ends a non-slid note that is still sounding.
			if (heldPending_ && !heldSlide_) {
				emitOff((uint8_t)heldNote_, off);
				heldNote_ = -1; heldPending_ = false;
			}
			continue;
		}

		// ★ ORDER IS THE SEQUENCER'S JOB. When the sounding step slid, the new
		// note-on is emitted BEFORE the old note-off, so a drainer applying
		// events blindly in order gets legato and the voice glides. Emitting
		// them the other way round retriggers instead -- the defect the
		// acid_bass example originally shipped.
		bool tie = heldPending_ && heldSlide_;
		push(SEQ_NOTE_ON, s.note, s.accent ? accentVel_ : normalVel_, tie, off);
		if (tie) {
			emitOff((uint8_t)heldNote_, off);
		}
		heldNote_    = s.note;
		heldSlide_   = s.slide;
		heldPending_ = true;
		heldOffTick_ = (folded + gateTicks_) % loopTicks;
	}
}
```

- [ ] **Step 2.3: Register in Audio.h**

After the `#include "transport.h"` line, add `#include "seq_step.h"`.

- [ ] **Step 2.4: Build the example**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/audio/step_seq_test
cmake --build build
```

Expected: compiles and links → `build/step_seq_test.elf`.

- [ ] **Step 2.5: Commit the Audio repo (do NOT push)**

```bash
cd ~/Development/Audio
git add seq_step.h seq_step.cpp Audio.h
git commit -m "seq_step: 16-step TB-303 sequencer clocked by AudioTransport

Folds the transport's per-block tick span into a 16-step pattern and emits
ordered note events into a pull-based queue. Two things are deliberate.

The transport is taken by REFERENCE in the constructor, which turns its
update-order requirement from a documented warning into something the compiler
enforces: a consumer cannot be declared before its clock, because you cannot
pass a reference to an object that does not exist yet.

Event ORDER is the sequencer's responsibility, not the drainer's. A slide step
emits the next note-on BEFORE the previous note-off, which is what makes
AudioSynthAcidBass glide rather than retrigger. Putting it here means no
consumer has to know it -- the acid_bass example originally got it wrong and
flammed every downbeat."
```

## Things to get right, and to REPORT on

1. **The fold's negative branch.** `abs` can be below `loopStartTick()` in
   principle; the expression above handles it without signed arithmetic.
   Convince yourself it is correct, or simplify it and say why the simpler form
   is safe.
2. **`heldOffTick_` wraps modulo `loopTicks`.** A gate that would end past the
   loop end therefore lands early in the next pass. Decide whether that is
   right (it means a note never outlives its loop) and document the decision.
3. **Build a host harness.** The previous two components in this series both
   found real defects that way — a stuck note and a silent tick loss — and both
   used mutation testing to prove the tests could see them. Strongly
   recommended here: the ORDER and REST properties are exactly the kind a
   harness pins cheaply.
4. **Verify the step-after-a-rest case by hand** before relying on the gate.
   An off-by-one in `folded / TICKS_PER_STEP` breaks it and little else.

---

## Task 3: QEMU gate green + commit the example

- [ ] **Step 3.1: Confirm QEMU is healthy** with an untouched control gate
  before trusting anything: `cd examples/audio/tone_test && ./run_qemu_tone.sh`.
  A concurrent qemu2 rebuild has removed the binary before in this tree.

- [ ] **Step 3.2: Run the gate**

```bash
cd examples/audio/step_seq_test
./run_qemu.sh
```

(`./run_qemu.sh`, never `sh run_qemu.sh` — it re-execs under gtimeout.)

Expected: `PASS: STEPS ACCENT ORDER REST WRAP SHORTLOOP`, exit 0.

Debugging guidance, in the order failures are likely:
- `STEPS` off by ~4x → the step derivation is using unfolded ticks.
- `ORDER` failing with `idx_off33 < idx_on45` → the note-off is being emitted
  before the note-on for a tie. **Fix the emission order; do not relax the
  assertion** — the ordering IS the feature.
- `REST` with `n45 == 0` → an off-by-one in the step index.
- `SHORTLOOP` with `out_loop > 0` → the fold is compressing the pattern into
  the loop instead of leaving the tail unused.

- [ ] **Step 3.3: Run it 5 times and record the spread**

```bash
for i in 1 2 3 4 5; do ./run_qemu.sh >/tmp/ss$i.log 2>&1; \
  echo "run$i exit=$? $(grep -E 'SQ: ons=|SQ: wrap_ons=' /tmp/ss$i.log | tr '\n' ' ')"; done
```

All five must pass. Every number here is audio-clock referenced, so the spread
should be zero or near it — `transport_test` achieved exactly zero. If a value
moves, find the wall-clock dependency before proceeding.

- [ ] **Step 3.4: Save the fixture and commit**

```bash
cp build/step_seq.uart transcript_qemu.txt
cd /Users/nicholasnewdigate/Development/rt1170/evkb
git add examples/audio/step_seq_test/step_seq_test.cpp \
        examples/audio/step_seq_test/CMakeLists.txt \
        examples/audio/step_seq_test/run_qemu.sh \
        examples/audio/step_seq_test/transcript_qemu.txt
git commit -m "audio: step_seq_test -- AudioStepSequencer example + QEMU token gate

Six tokens. ORDER is the load-bearing one: it inspects the emission ORDER of a
slide step's events directly, because a note-off emitted before the tie's
note-on retriggers the voice instead of gliding it, and no count-based
assertion can tell those apart. SHORTLOOP pins the fold's defined behaviour --
steps beyond a short loop are an unused tail, not a compressed pattern -- so a
future change to step derivation cannot quietly invert it while every other
assertion still passes."
```

---

## Task 4: License audit GATES entry

**Files:** Modify `tools/license-audit.sh` (the `GATES` list)

- [ ] **Step 4.1:** In the `GATES=` list, insert alphabetically in the audio
  cluster, after `examples/audio/sd_wav_play_test:sd_wav_play_test`:

```
examples/audio/step_seq_test:step_seq_test \
```

- [ ] **Step 4.2: Run the audit**

```bash
bash -n tools/license-audit.sh && ./tools/license-audit.sh 2>&1 | tail -3
```

Expected: `LICENSE-AUDIT: PASS` with `examples/audio/step_seq_test` in the
depfile walk. New sources are MIT; no `ALLOW` change needed. The drift check
will name the example if you forget this step — that is the check working.

- [ ] **Step 4.3: Commit**

```bash
git add tools/license-audit.sh
git commit -m "tools: license-audit GATES entry for step_seq_test"
```

---

## Task 5: Full sweep + baseline

- [ ] **Step 5.1: Read `docs/KNOWN-BROKEN-GATES.md`** — the CLAUDE.md rule
  before any sweep.

- [ ] **Step 5.2: Sweep**

```bash
./tools/run-all-qemu-gates.sh 2>&1 | tail -5
```

Expected: **92 passed, 0 failed, 0 SKIP** (91 before this example), or 91/1/0
when the documented nondeterministic `dualcore/cm4_audio_test` is red — re-run
that one idle before believing it. A non-zero SKIP means an example did not
build, so the sweep measured less than it claims.

- [ ] **Step 5.3: Update CLAUDE.md**

Change `**91 gates**` to `**92 gates**`, prepend `91 before the step sequencer
added audio/step_seq_test;` to the history parenthetical, and update both
target lines (`92 passed, 0 failed, 0 SKIP` / `91 passed, 1 failed, 0 SKIP`)
and the measured-clean line. **Write the numbers you measured, not these.**

- [ ] **Step 5.4: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: sweep baseline 91 -> 92 (step_seq_test), measured"
```

---

## Task 6: Hardware verification

★ Needs the board and the user present.

- [ ] **Step 6.1: Flash — VCOM-free**

```bash
pkill -f rt1170-console.py; pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
cd examples/audio/step_seq_test
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/step_seq_test.elf
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build/step_seq_test.elf
```

Check daemons with `pgrep -x`, not `ps | grep` — the latter matches your own
command line and reads as a false positive.

- [ ] **Step 6.2: Attach the reader, then ask the user to press SW4**

```bash
(python3 ../../../tools/rt1170-console.py /dev/cu.usbmodem* 115200 > /tmp/ss_hw.txt 2>&1 &)
```

★ **Never trigger the reset with `LinkServer run` while the reader is
attached.** It re-enumerates the VCOM and has kernel-panicked this host three
times (IOSerialFamily use-after-free, `python3.12` the panicking task). Ask the
user to press **SW4**.

- [ ] **Step 6.3: Listening check (user)**

Headphones on J101. Ask the user to confirm:
1. a 16-step acid pattern is audible and rhythmically steady;
2. accented steps are louder and brighter;
3. **slides bend between notes** rather than retriggering;
4. **rests are silent** — gaps where the pattern has no step;
5. the loop seam is clean.

Points 3 and 4 are the ones the event stream can only assert structurally. The
gate proves a slide's note-on precedes its note-off; only the ear proves the
result actually glides.

- [ ] **Step 6.4:** Detach the reader BEFORE any further LinkServer use. Trim
  the capture to boot + tokens + a dozen heartbeats, annotate the user's
  verdict as comment lines at the top, matching
  `examples/audio/transport_test/transcript_hw_evkb.txt`.

- [ ] **Step 6.5: Commit**

```bash
git add examples/audio/step_seq_test/transcript_hw_evkb.txt
git commit -m "audio: step_seq_test hardware transcript -- acid pattern audible on the EVKB"
```

---

## Task 7: Push Audio, bump the pin

- [ ] **Step 7.1:** `cd ~/Development/Audio && git log origin/master..master --oneline`
  (must be only the seq_step commit), then `git push` and `git rev-parse HEAD`.
  Confirm with the user first if anything unrelated is on master.

- [ ] **Step 7.2:** In `evkb.cmake`, replace the Audio SHA on the
  `teensy_declare_library(Audio …)` line with the SHA from Step 7.1.

- [ ] **Step 7.3: Prove the fresh-user path**

```bash
cd examples/audio/step_seq_test
cmake -B build-forcefetch -DEVKB_FORCE_FETCH=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build-forcefetch
rm -rf build-forcefetch
```

- [ ] **Step 7.4: Commit**

```bash
git add evkb.cmake
git commit -m "build: bump Audio pin for the step sequencer"
```

Do not push evkb without asking — `master` already carries 12 unpushed commits
that are not yours to decide about.

---

## Self-review notes (already applied)

- **Spec coverage**: §1 architecture and constructor binding → Task 2.1; §2
  pattern/event model and the three loop-length cases → 2.2, with `SHORTLOOP`
  in 1.3 pinning the third; §3 gate length and emission → 2.2; §4 gates → 1.3,
  one assertion per spec bullet; bookkeeping → Tasks 4, 5, 7; error handling →
  2.2 (index guards, `gateLength` NaN-safe clamp, `loopTicks == 0` guard,
  queue saturation).
- **Ambiguity resolved while writing**: the spec says a rest emits no note-on
  but did not say whether a rest *ends* a sounding note. It does, unless that
  note slid — otherwise a rest after a gated step would sustain through the
  gap, which is not what a rest sounds like. Implemented and commented.
- **Type consistency**: `eventCount()`/`eventAt()`/`eventOverflow()`/
  `currentStep()`/`step()`/`clear()`/`gateLength()` and the `SeqEvent` field
  names are spelled identically in Task 1's sketch and Task 2's header.
- **YAGNI honored**: no swing, no configurable division or pattern length, no
  chaining, no per-step velocity, no MIDI.
