# Audio Guard Sweep (Phase A step 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Revive the six Audio components silently dead/degraded on RT1176 (chip-list guards) and prove each with runtime known-answer assertions, QEMU + hardware.

**Architecture:** 9 surgical edit sites across 8 files in `~/Development/Audio` (4 capability guards → `__ARM_ARCH_7EM__`, 3 memory guards gain `__IMXRT1176__`, 2 stale `arm_math.h` includes stripped) + one new evkb gate `examples/audio/guard_sweep_test` with six stages, written FIRST so it fails on pre-sweep sources (red) and passes after (green). Then Audio push → evkb.cmake pin bump → regressions → docs.

**Tech Stack:** CMake/teensy-cmake-macros, ARM GCC 10, qemu2 via `tools/qrun`, LinkServer. No CMSIS-DSP import (proves the include-strips).

**Spec:** `docs/superpowers/specs/2026-07-21-audio-guard-sweep-design.md`

**Key numbers used by the gate (44.1 kHz, 128-sample blocks ≈ 2.9 ms):**
- 200 ms delay tap = 69 blocks. Old `effect_delay.h` fallback queue = 6144/128 = 48 blocks ≈ 139 ms max → the DELAY stage **cannot pass on old sources** (tap already audible at the 55-block "must-be-silent" check) and must pass on new (silent at 55, loud at 85).
- Old `record_queue` ring caps at 53−1 = 52 held blocks; stage demands ≥55 of 60. Old `play_queue` caps at 32; stage plays 40 pattern blocks through and counts them.
- Wavetable synthetic instrument: 256-sample single-cycle sine, `INDEX_BITS=8` → the 32-bit phase spans exactly one cycle (natural uint32 wrap; loop fields set so the explicit wrap never fires). `PER_HERTZ_PHASE_INCREMENT = 2^32/44100 ≈ 97391.55`. Table has 260 entries (index 255 is read as a packed `uint32_t` pair → one guard sample required; we pad four). Envelope counts are in 8-sample units; **ATTACK_COUNT and DECAY_COUNT must be ≥1** (0 divides by zero in the state machine); `SUSTAIN_MULT=0` sustains at unity. Vibrato/modulation `*_DELAY=0xFFFFFFFF` so the LFOs never engage.
- `AudioMemory(150)`: delay ring (69, live from first pump because connections are static) + record-queue hold (60) + graph slack — pool exhaustion can never masquerade as a guard failure.

---

### Task 1: `guard_sweep_test` gate (red on current Audio sources)

**Files:**
- Create: `examples/audio/guard_sweep_test/CMakeLists.txt`
- Create: `examples/audio/guard_sweep_test/guard_sweep_test.cpp`
- Create: `examples/audio/guard_sweep_test/run_qemu.sh` (755)
- Create: `examples/audio/guard_sweep_test/toolchain/rt1170-evkb.toolchain.cmake` (copy)

- [ ] **Step 1: Directory + toolchain copy**

```bash
cd ~/Development/rt1170/evkb/examples/audio
mkdir -p guard_sweep_test/toolchain
cp audiostream_test/toolchain/rt1170-evkb.toolchain.cmake guard_sweep_test/toolchain/
```

- [ ] **Step 2: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(guard_sweep_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(Audio EVKB_AUDIO_DIR)

teensy_add_executable(guard_sweep_test guard_sweep_test.cpp)

# Cherry-picked (no glob, per gate convention). Deliberately NO CMSIS-DSP
# import: none of these nodes needs arm_math once the stale includes are
# stripped -- this gate is also the proof of that.
target_sources(guard_sweep_test.elf PRIVATE
    ${EVKB_AUDIO_DIR}/synth_sine.cpp
    ${EVKB_AUDIO_DIR}/synth_karplusstrong.cpp
    ${EVKB_AUDIO_DIR}/synth_simple_drum.cpp
    ${EVKB_AUDIO_DIR}/synth_wavetable.cpp
    ${EVKB_AUDIO_DIR}/effect_delay.cpp
    ${EVKB_AUDIO_DIR}/play_queue.cpp
    ${EVKB_AUDIO_DIR}/record_queue.cpp
    ${EVKB_AUDIO_DIR}/analyze_peak.cpp
    ${EVKB_AUDIO_DIR}/data_waveforms.c)
target_include_directories(guard_sweep_test.elf PRIVATE
    ${EVKB_AUDIO_DIR} ${EVKB_AUDIO_DIR}/utility)

teensy_target_link_libraries(guard_sweep_test cores)
target_link_libraries(guard_sweep_test.elf stdc++)
# The toolchain's -lm precedes the objects in LINK_FLAGS; re-link libm after
# them (same convention as i2s_audio_test and the other audio gates) for the
# gate's sinf/lrintf and synth_wavetable.h's inline powf.
target_link_libraries(guard_sweep_test.elf m)

# TEMPORARY (removed in Task 2): synth_wavetable.cpp:30 carries a stale,
# unconditional `#include <SerialFlash.h>` (zero SerialFlash symbols used).
# Task 2 strips it in the Audio fork; until then this empty stub lets the
# red-phase gate compile against the UNMODIFIED sources.
target_include_directories(guard_sweep_test.elf PRIVATE ${CMAKE_CURRENT_LIST_DIR}/stub)
```

- [ ] **Step 2b: Write the temporary stub `stub/SerialFlash.h`**

```c
// TEMPORARY red-phase stub (Task 1 only; deleted in Task 2).
// ~/Development/Audio/synth_wavetable.cpp:30 includes <SerialFlash.h> without
// using any of its symbols; the real strip happens in the guard-sweep task.
// Intentionally empty.
```

- [ ] **Step 3: Write `guard_sweep_test.cpp`**

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"
#include "synth_sine.h"
#include "synth_karplusstrong.h"
#include "synth_simple_drum.h"
#include "synth_wavetable.h"
#include "effect_delay.h"
#include "play_queue.h"
#include "record_queue.h"
#include "analyze_peak.h"
#include <math.h>

// Guard-sweep revival gate: six stages, each impossible to pass with the
// pre-sweep Audio sources (dead update() bodies produce silence; the old
// delay/queue depth caps are below what the stages demand).

// No I/O node in this graph, so nothing calls the protected update_setup()
// that arms IRQ_SOFTWARE dispatch -- same pattern as audiostream_test.
struct GraphClock : public AudioStream {
    GraphClock() : AudioStream(0, NULL) { update_setup(); }
    void update(void) override {}
};
static GraphClock clock1;

static AudioSynthWaveformSine     sine1;      // feeds delay + record queue
static AudioEffectDelay           delay1;
static AudioSynthKarplusStrong    karplus1;
static AudioSynthSimpleDrum       drum1;
static AudioSynthWavetable        wt1;
static AudioPlayQueue             pq1;
static AudioRecordQueue           recq1;      // depth stage (from sine1)
static AudioRecordQueue           recp1;      // play-queue counting stage
static AudioAnalyzePeak           peak_del, peak_k, peak_d, peak_w;

static AudioConnection c1(sine1, 0, delay1, 0);
static AudioConnection c2(delay1, 0, peak_del, 0);   // 200 ms tap
static AudioConnection c3(sine1, 0, recq1, 0);
static AudioConnection c4(karplus1, 0, peak_k, 0);
static AudioConnection c5(drum1, 0, peak_d, 0);
static AudioConnection c6(wt1, 0, peak_w, 0);
static AudioConnection c7(pq1, 0, recp1, 0);

// --- synthetic single-cycle sine instrument for AudioSynthWavetable --------
// 256-sample cycle, INDEX_BITS=8: the 32-bit phase spans exactly one cycle,
// so looping is the natural uint32 wrap and the explicit loop adjustment
// (LOOP_PHASE_END/LENGTH = 0xFFFFFFFF) never fires. Entry 255 is fetched as a
// packed uint32 pair, so the table carries guard samples [256..259].
// Envelope counts are in 8-sample units; ATTACK/DECAY must be >=1 (a zero
// divides by zero in the envelope state machine). SUSTAIN_MULT=0 sustains at
// unity. Vibrato/modulation delays are maxed so the LFOs never engage.
static int16_t wt_table[260];
static const AudioSynthWavetable::sample_data wt_samples[1] = {{
    wt_table,                     // sample
    true,                         // LOOP
    8,                            // INDEX_BITS
    4294967296.0f / 44100.0f,     // PER_HERTZ_PHASE_INCREMENT
    0xFFFFFFFFu,                  // MAX_PHASE (unused when LOOP)
    0xFFFFFFFFu,                  // LOOP_PHASE_END
    0xFFFFFFFFu,                  // LOOP_PHASE_LENGTH
    0xFFFF,                       // INITIAL_ATTENUATION_SCALAR (no atten)
    0,                            // DELAY_COUNT
    6,                            // ATTACK_COUNT  (48 samples)
    1,                            // HOLD_COUNT
    1,                            // DECAY_COUNT   (>=1: div-by-zero guard)
    50,                           // RELEASE_COUNT (400 samples ~ 3 blocks)
    0,                            // SUSTAIN_MULT  (sustain at unity)
    0xFFFFFFFFu, 0, 0.0f, 0.0f,          // vibrato: never engages
    0xFFFFFFFFu, 0, 0.0f, 0.0f, 0, 0     // modulation: never engages
}};
static const uint8_t wt_ranges[1] = {127};
static const AudioSynthWavetable::instrument_data wt_instr = {1, wt_ranges, wt_samples};

static void pump(int n) {
    for (int i = 0; i < n; i++) {
        NVIC_SET_PENDING(IRQ_SOFTWARE);
        delayMicroseconds(200);
    }
}

static float read_peak(AudioAnalyzePeak &p) {
    return p.available() ? p.read() : 0.0f;   // no blocks seen = silence
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    AudioMemory(150);
    for (int i = 0; i < 260; i++)
        wt_table[i] = (int16_t)lrintf(20000.0f * sinf(2.0f * (float)M_PI * (i % 256) / 256.0f));

    sine1.amplitude(0.9f);
    sine1.frequency(440.0f);
    delay1.delay(0, 200.0f);      // 69 blocks: exceeds the old 48-block queue

    // STAGE_DELAY -- must run FIRST: the block count since boot is the clock.
    pump(55);                                  // 160 ms: before the 200 ms tap
    float d_early = read_peak(peak_del);
    pump(30);                                  // 246 ms: tap must be live
    float d_late = read_peak(peak_del);
    bool pass_delay = (d_early < 0.02f) && (d_late > 0.5f);
    Serial1.print("GS: delay early="); Serial1.print(d_early, 4);
    Serial1.print(" late=");           Serial1.println(d_late, 4);
    Serial1.println(pass_delay ? "STAGE_DELAY=PASS" : "STAGE_DELAY=FAIL");

    // STAGE_KARPLUS -- pluck burst then decay (windows separated so the
    // peak-hold reset in read() isolates the quiet tail).
    karplus1.noteOn(220.0f, 1.0f);
    pump(4);
    float k_a = read_peak(peak_k);
    pump(300); (void)read_peak(peak_k);        // discard the loud middle
    pump(60);
    float k_b = read_peak(peak_k);
    bool pass_k = (k_a > 0.10f) && (k_b < k_a * 0.6f);
    Serial1.print("GS: karplus a="); Serial1.print(k_a, 4);
    Serial1.print(" b=");            Serial1.println(k_b, 4);
    Serial1.println(pass_k ? "STAGE_KARPLUS=PASS" : "STAGE_KARPLUS=FAIL");

    // STAGE_DRUM -- 150 ms drum hit: burst, then silence after the envelope.
    drum1.frequency(60.0f);
    drum1.length(150);
    drum1.secondMix(0.0f);
    drum1.pitchMod(0.5f);
    drum1.noteOn();
    pump(3);
    float dr_a = read_peak(peak_d);
    pump(100); (void)read_peak(peak_d);        // ride out the 52-block decay
    pump(30);
    float dr_b = read_peak(peak_d);
    bool pass_dr = (dr_a > 0.2f) && (dr_b < 0.05f);
    Serial1.print("GS: drum a="); Serial1.print(dr_a, 4);
    Serial1.print(" b=");         Serial1.println(dr_b, 4);
    Serial1.println(pass_dr ? "STAGE_DRUM=PASS" : "STAGE_DRUM=FAIL");

    // STAGE_WAVETABLE -- synthetic instrument: sustained tone, then release.
    wt1.setInstrument(wt_instr);
    wt1.playNote(69, 127);                     // A4 = 440 Hz
    pump(12);
    float w_a = read_peak(peak_w);
    bool w_sustain = (wt1.getEnvState() == AudioSynthWavetable::STATE_SUSTAIN);
    wt1.stop();
    pump(6); (void)read_peak(peak_w);          // ride out the ~3-block release
    pump(10);
    float w_b = read_peak(peak_w);
    bool pass_w = (w_a > 0.2f) && (w_a < 0.9f) && w_sustain && (w_b < 0.05f);
    Serial1.print("GS: wt a="); Serial1.print(w_a, 4);
    Serial1.print(" b=");       Serial1.print(w_b, 4);
    Serial1.print(" sustain="); Serial1.println(w_sustain ? 1 : 0);
    Serial1.println(pass_w ? "STAGE_WAVETABLE=PASS" : "STAGE_WAVETABLE=FAIL");

    // STAGE_RECQ -- hold >52 blocks (old ring cap) then drain intact.
    recq1.begin();
    pump(60);
    int held = recq1.available();
    int drained = 0;
    bool content_ok = false;
    while (recq1.available() > 0) {
        int16_t *b = recq1.readBuffer();
        if (b) {
            if (drained == 0) {                // sine content, not silence
                for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
                    if (b[i] > 8000 || b[i] < -8000) { content_ok = true; break; }
            }
            drained++;
        }
        recq1.freeBuffer();
    }
    recq1.end();
    bool pass_rq = (held >= 55) && (drained == held) && content_ok;
    Serial1.print("GS: recq held="); Serial1.print(held);
    Serial1.print(" drained=");      Serial1.print(drained);
    Serial1.print(" content=");      Serial1.println(content_ok ? 1 : 0);
    Serial1.println(pass_rq ? "STAGE_RECQ=PASS" : "STAGE_RECQ=FAIL");

    // STAGE_PLAYQ -- enqueue 40 pattern blocks (old cap 32), play them
    // through, count and spot-check them on the far side.
    recp1.begin();
    for (int k = 0; k < 40; k++) {
        int16_t *b = pq1.getBuffer();          // pool is ample: never stalls
        if (!b) break;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) b[i] = (int16_t)((k + 1) * 100);
        pq1.playBuffer();
    }
    pump(45);
    int got = recp1.available();
    int16_t first_val = 0, last_val = 0;
    for (int k = 0; k < got; k++) {
        int16_t *b = recp1.readBuffer();
        if (b) {
            if (k == 0)       first_val = b[0];
            if (k == got - 1) last_val  = b[0];
        }
        recp1.freeBuffer();
    }
    recp1.end();
    bool pass_pq = (got == 40) && (first_val == 100) && (last_val == 4000);
    Serial1.print("GS: playq got="); Serial1.print(got);
    Serial1.print(" first=");        Serial1.print(first_val);
    Serial1.print(" last=");         Serial1.println(last_val);
    Serial1.println(pass_pq ? "STAGE_PLAYQ=PASS" : "STAGE_PLAYQ=FAIL");

    bool all = pass_delay && pass_k && pass_dr && pass_w && pass_rq && pass_pq;
    Serial1.println(all ? "GUARD_SWEEP_ALL=PASS" : "GUARD_SWEEP_ALL=FAIL");
}
void loop() {}
```

- [ ] **Step 4: Write `run_qemu.sh`** (then `chmod +x run_qemu.sh`)

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/guard_sweep_test.elf"; OUT="$DIR/guard_sweep.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/guard_sweep.dbg" &
P=$!; gate_pid $P; sleep 10; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
for t in DELAY KARPLUS DRUM WAVETABLE RECQ PLAYQ; do
    grep -q "STAGE_$t=PASS" "$OUT" || { echo "FAIL: $t"; exit 1; }
done
grep -q "GUARD_SWEEP_ALL=PASS" "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: GUARD_SWEEP_ALL"
```

- [ ] **Step 5: Build and run — must be RED for the right reasons**

```bash
cd ~/Development/rt1170/evkb/examples/audio/guard_sweep_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
./run_qemu.sh
```

Expected: build succeeds (dead `update()` bodies still compile), gate FAILS with `FAIL: DELAY` (first failing stage). The captured output must show: `delay early=` > 0.5 (old 48-block queue means the tap is already live at 160 ms), karplus/drum/wavetable `a=0.0000` (silent — dead update bodies), `recq held=` ≤ 52, `playq got=` ≤ 32 or a hang killed by the gate timeout. Record the observed values — they are the red-phase evidence. If the build itself fails, STOP: that contradicts the "compiles but silent" model — report it.

Note: `pq1.getBuffer()` can stall only on pool exhaustion (not the case here); the old play_queue cap may instead silently drop or stall on the 33rd `playBuffer` — a hang is acceptable red behavior (gate-lib's timeout converts it to a kill + FAIL).

- [ ] **Step 6: Commit the red gate (evkb repo)**

```bash
cd ~/Development/rt1170/evkb
git add examples/audio/guard_sweep_test
git commit -m "test: guard_sweep_test revival gate (red - Audio chip-list guards still dead on 1176)"
```

End the commit message body with: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

### Task 2: The guard sweep itself (green)

**Files:**
- Modify (Audio): `synth_karplusstrong.cpp:30`, `synth_karplusstrong.cpp:47`
- Modify (Audio): `synth_simple_drum.cpp:112`
- Modify (Audio): `synth_wavetable.cpp:179` and `:30` (SerialFlash strip)
- Modify (Audio): `effect_delay.h:33`
- Modify (Audio): `play_queue.h:36`
- Modify (Audio): `record_queue.h:36`
- Modify (Audio): `synth_waveform.h:32`, `analyze_notefreq.cpp:26`
- Modify (evkb): `examples/audio/guard_sweep_test/CMakeLists.txt` (drop the stub include-dir); Delete: `examples/audio/guard_sweep_test/stub/`

- [ ] **Step 1: Capability guards → `__ARM_ARCH_7EM__`** (4 sites, identical edit)

At `synth_karplusstrong.cpp:30` and `:47`, `synth_simple_drum.cpp:112`, `synth_wavetable.cpp:179`, change:

```c
#if defined(KINETISK) || defined(__IMXRT1062__)
```
to:
```c
#if defined(__ARM_ARCH_7EM__)   // Cortex-M4/M7: KINETISK + IMXRT1062 + IMXRT1176
```

(Behavior-preserving on every Teensy: KINETISK chips are Cortex-M4 and the 1062 is M7 — both define `__ARM_ARCH_7EM__`; KINETISL is M0+ and stays excluded. Any `#elif defined(KINETISL)` branches below are untouched.)

- [ ] **Step 2: Memory guards gain `__IMXRT1176__`** (3 sites)

`effect_delay.h:33`:
```c
#if defined(__IMXRT1062__)
```
→
```c
#if defined(__IMXRT1062__) || defined(__IMXRT1176__)
```

`play_queue.h:36` and `record_queue.h:36`:
```c
#if defined(__IMXRT1062__) || defined(__MK66FX1M0__) || defined(__MK64FX512__)
```
→
```c
#if defined(__IMXRT1062__) || defined(__IMXRT1176__) || defined(__MK66FX1M0__) || defined(__MK64FX512__)
```

- [ ] **Step 3: Strip the stale includes** (synth_sine.h NOTE-comment precedent)

`synth_waveform.h:32` — replace `#include <arm_math.h>    // github.com/...` with:
```c
// NOTE: upstream includes <arm_math.h> here, but nothing in this file or
// synth_waveform.cpp uses any arm_math symbol -- stripped so consumers don't
// need the CMSIS-DSP library (same rationale as synth_sine.h).
```

`analyze_notefreq.cpp:26` — replace `#include "arm_math.h"` with:
```c
// NOTE: upstream includes "arm_math.h" here, but this file makes no arm_
// calls -- stripped so the node doesn't require the CMSIS-DSP library.
```

`synth_wavetable.cpp:30` — replace `#include <SerialFlash.h>` with:
```c
// NOTE: upstream includes <SerialFlash.h> here, but this file uses no
// SerialFlash symbol -- stripped so the node doesn't require the SerialFlash
// library (discovered when guard_sweep_test became the first gate to compile
// this file; see the plan's Task 1 stub note).
```

Then **delete the Task 1 temporary stub** and its CMakeLists line:
```bash
cd ~/Development/rt1170/evkb/examples/audio/guard_sweep_test
rm -rf stub
```
and remove the `target_include_directories(guard_sweep_test.elf PRIVATE ${CMAKE_CURRENT_LIST_DIR}/stub)` line (and its TEMPORARY comment block) from `CMakeLists.txt`.

- [ ] **Step 4: Rebuild + rerun the gate — must be GREEN**

```bash
cd ~/Development/rt1170/evkb/examples/audio/guard_sweep_test
cmake --build build && ./run_qemu.sh
```

Expected: all six STAGE tokens PASS, `PASS: GUARD_SWEEP_ALL`. Info-line sanity: `delay early=0.0000 late=`≈0.9, karplus `a=`>0.1 with `b`<0.6a, drum burst-then-quiet, `wt a=`≈0.2-0.9 `sustain=1` `b=`<0.05, `recq held=59` or 60, `playq got=40 first=100 last=4000`. If a stage fails, debug root-cause against the relevant Audio source — thresholds and tokens are immutable without reporting; if a threshold is genuinely mis-derived, report the measured value and the derivation error BEFORE changing anything.

- [ ] **Step 5: Commit both repos**

```bash
cd ~/Development/Audio
git add synth_karplusstrong.cpp synth_simple_drum.cpp synth_wavetable.cpp \
        effect_delay.h play_queue.h record_queue.h synth_waveform.h analyze_notefreq.cpp
git commit -m "fix: revive Cortex-M7 builds — chip-list guards -> __ARM_ARCH_7EM__, RT1176 joins the big-RAM tiers

karplusstrong/simple_drum/wavetable update() bodies were silently empty on
any chip outside KINETISK||IMXRT1062; effect_delay/play_queue/record_queue
fell to their smallest fallback sizes. Also strip two stale arm_math.h
includes (synth_waveform.h, analyze_notefreq.cpp) so those consumers don't
need CMSIS-DSP. Verified by the evkb guard_sweep_test gate (red on the old
sources, green now)."

cd ~/Development/rt1170/evkb/examples/audio/guard_sweep_test
cp guard_sweep.uart transcript_qemu.txt
cd ~/Development/rt1170/evkb
git add examples/audio/guard_sweep_test/transcript_qemu.txt
git commit -m "test: guard_sweep_test green after the Audio guard sweep (QEMU transcript)"
```

End both commit message bodies with: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

### Task 3: Hardware verification

- [ ] **Step 1: Flash + capture** (board rules: LinkServer only, pkill daemons first, pyserial reader before flashing, never `cat` the port)

```bash
pkill LinkServer; pkill redlinkserv; sleep 1
cd ~/Development/rt1170/evkb/examples/audio/guard_sweep_test
python3 ~/Development/rt1170/evkb/tools/rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > transcript_hw_evkb.txt 2>&1 &
sleep 1
/Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/guard_sweep_test.elf
sleep 8
pkill -f rt1170-console.py
pkill LinkServer; pkill redlinkserv
```

- [ ] **Step 2: Verify**

```bash
grep "GUARD_SWEEP_ALL=PASS" transcript_hw_evkb.txt
```

Then compare all `GS:` info lines and STAGE tokens line-by-line against `transcript_qemu.txt` (strip CR/NUL for comparison). The karplus/drum/wavetable/queue paths are integer-deterministic — expect identical values; the delay/peak floats derive from integers and should also match. Any divergence: report verbatim, both values, no retries-until-it-matches.

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/audio/guard_sweep_test/transcript_hw_evkb.txt
git commit -m "test: guard_sweep_test HW-verified on EVKB (transcript)"
```

End the commit message body with: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

### Task 4: Regressions, Audio push, pin bump, fresh-user proof

- [ ] **Step 1: Regression gates** (Audio sources changed — both must stay green)

```bash
cd ~/Development/rt1170/evkb/examples/audio/filter_fir_test   && ./run_qemu.sh
cd ~/Development/rt1170/evkb/examples/audio/audiostream_test  && ./run_qemu_audiostream.sh
```

Expected: `PASS: FIR_ALL` and `PASS: AUDIOSTREAM_ALL`. (Rebuild first with `cmake --build build` if the build dirs are stale; configure from scratch if missing.)

- [ ] **Step 2: License audit** (Audio is in the Part-1 sweep and the gates walk its files)

```bash
~/Development/rt1170/evkb/tools/license-audit.sh
```

Expected: `LICENSE-AUDIT: PASS`.

- [ ] **Step 3: Push Audio, bump the pin**

```bash
cd ~/Development/Audio && git push && git rev-parse HEAD
```

Then in `~/Development/rt1170/evkb/evkb.cmake`, update the Audio manifest line — replace the SHA `0d9501ea9a73b0233efd21c8a26aad045918e897` with the new `git rev-parse HEAD` value (full 40 chars) on the `_evkb_lib(Audio ...)` line.

- [ ] **Step 4: Fresh-user proof of the pin**

```bash
cd ~/Development/rt1170/evkb/examples/audio/guard_sweep_test
cmake -B build-ff -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake -DEVKB_FORCE_FETCH=ON
cmake --build build-ff
```

Expected: configure logs fetching Audio @ the NEW SHA; build succeeds. Then boot the fetched-build ELF once in QEMU to prove the pinned sources pass, and clean up:

```bash
~/Development/rt1170/evkb/tools/qrun -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on \
    -kernel build-ff/guard_sweep_test.elf -display none -serial file:ff.uart -d guest_errors -D ff.dbg &
P=$!; sleep 10; kill $P 2>/dev/null; wait $P 2>/dev/null
grep "GUARD_SWEEP_ALL=PASS" ff.uart && rm -rf build-ff ff.uart ff.dbg
```

- [ ] **Step 5: Commit the pin bump**

```bash
cd ~/Development/rt1170/evkb
git add evkb.cmake
git commit -m "chore: bump Audio pin — guard sweep (ARM_ARCH_7EM capability guards, 1176 RAM tiers, arm_math include strips)"
```

End the commit message body with: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

### Task 5: Docs + push

- [ ] **Step 1: evkb `examples/README.md`** — add `guard_sweep_test` to the **audio** row of the category table (alphabetical placement, backticked).

- [ ] **Step 2: Audio status doc** (`~/Development/Audio/docs/rt1170-evkb-status.md`):
  - `synth_karplusstrong`, `synth_simple_drum`, `synth_wavetable` rows: 🟡 → ✅, note `guards -> __ARM_ARCH_7EM__; HW-verified via evkb guard_sweep_test` (wavetable note adds `synthetic single-cycle instrument`).
  - `effect_delay` row: 🟡 → ✅, note `1176 joins the 4-second tier; 200 ms tap HW-verified via guard_sweep_test`.
  - `play_queue, record_queue` row: 🟡 → ✅, note `deep-buffer tiers (80/209) HW-verified via guard_sweep_test`.
  - `synth_waveform` and `analyze_notefreq` rows: drop the stale-include caveats (now stripped), keep 🟢.
  - Legend 🟡 line: now reads as the remaining candidates only (no full-component rows remain; `Audio.h` keeps 🟡 until step 3's guard-audit/compile gate).
  - Roadmap: Phase A step 2 DONE 2026-07-21 (gate + commit refs); step 3 is next. Append a changelog line under the existing 2026-07-21 blockquote.
- [ ] **Step 3: Commit + push both repos**

```bash
cd ~/Development/rt1170/evkb
git add examples/README.md
git commit -m "docs: examples index gains guard_sweep_test"
git push
cd ~/Development/Audio
git add docs/rt1170-evkb-status.md
git commit -m "docs: guard sweep landed — six 🟡 components revived and HW-verified"
git push
```

End both commit message bodies with: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

(Memory updates are handled by the coordinator, not this task.)

---

## Verification (whole plan)

1. `guard_sweep_test` red on old sources (Task 1 evidence), green after sweep, QEMU + HW transcripts committed and matching.
2. Regressions green: `filter_fir_test`, `audiostream_test`.
3. `license-audit.sh` PASS.
4. Audio pushed; evkb Audio pin = pushed HEAD; `-DEVKB_FORCE_FETCH=ON` build boots green in QEMU.
5. Status doc, examples README, roadmap consistent with what shipped.
