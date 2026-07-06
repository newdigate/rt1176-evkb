# RT1176 AudioOutputI2S Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring up `AudioOutputI2S` on RT1176 — the audio-graph playback node that streams `audio_block_t`s from the graph out over SAI1 TX to the WM8962 DAC — proven via `AudioSynthWaveformSine → AudioOutputI2S` in QEMU (SAI1 TX tap) then on hardware (audible tone on J101).

**Architecture:** Playback twin of the completed `AudioInputI2S`. Nodes live in the Audio **fork** (`~/Development/Audio`, `master`); `AudioStream` + register defs + `DMAChannel` + `config_i2s()` are already in the core (`cores/imxrt1176`, **untouched** by this effort); the gate is local in `evkb/`. 44.1 kHz throughout. `config_i2s()` (SAI TX + audio-PLL), `AudioOutputI2S::isr()` (guard already widened, `ac70035`), `AudioOutputI2S::update()` (graph-side, platform-independent), and `AudioControlWM8962::enable()` (DAC→headphone route) are all already done.

**Tech Stack:** C++ (Teensy Audio structure), NXP MIMXRT1176-EVKB (Cortex-M7), eDMA/SAI1, QEMU `mimxrt1170-evk` machine (via `evkb/tools/qrun`), LinkServer flashing.

---

## Repos & conventions

- **Fork** `~/Development/Audio` (`git@github.com:newdigate/Audio.git`, `master`) — the only code repo touched. Commit node work here.
- **Core** `~/Development/rt1170/evkb/cores/imxrt1176` (teensy-cores) — **not modified** (dspinst.h/data_waveforms/config_i2s/isr/dcache all already present).
- **Gate** `~/Development/rt1170/evkb/audiooutput_i2s_test/` — local only, never pushed.
- Commit to `master` in each repo as you go. **Push only when the user asks** (Task 5).
- QEMU runs go through `evkb/tools/qrun` (gtimeout + log cap). Reference gates to mirror: `evkb/audioinput_i2s_test/` (CMake + toolchain + AudioStream+fork wiring) and `evkb/i2s_audio_test/` (the `sai1-tap` chardev for capturing TX samples).

## File structure

| File | Repo | Responsibility | Change |
|---|---|---|---|
| `output_i2s.cpp` | fork | `AudioOutputI2S::begin()` — add `__IMXRT1176__` TX-DMA branch | **modify** |
| `synth_sine.{cpp,h}`, `data_waveforms.c`, `utility/dspinst.h` | fork | sine oscillator + table + DSP intrinsics | **verify compile** (no edit expected) |
| `audiooutput_i2s_test/*` | evkb | the QEMU gate (firmware + CMake + run script) | **create** |

## What is already done (reuse, DO NOT rebuild)

- `config_i2s()` — fork `output_i2s.cpp` `__IMXRT1176__` branch (SAI TX + audio-PLL @ 44.1 kHz, incl. TX pin mux AD_21). `begin()` calls it first.
- `AudioOutputI2S::isr()` — `output_i2s.cpp:130` guard is `#if defined(KINETISK) || defined(__IMXRT1062__) || defined(__IMXRT1176__)`; the 16-bit-fill body applies. `arm_dcache_flush_delete` is a no-op inline in `imxrt1176.h:576`.
- `AudioOutputI2S::update()` — `output_i2s.cpp:259`, platform-independent (`receiveReadOnly(0/1)` → queue into `block_left/right_1st/2nd`). No change.
- `AudioOutputI2S` ctor auto-begins (`output_i2s.h:40`, inside `#if !defined(KINETISL)`): `AudioOutputI2S(void) : AudioStream(2, inputQueueArray) { begin(); }`.
- `AudioControlWM8962::enable()` — DAC→headphone playback route already configured (`wm8962.cpp`, HW-verified 1 kHz on J101).
- `AudioSynthWaveformSine` (`synth_sine.cpp`) includes `utility/dspinst.h` (has `multiply_32x32_rshift32` + `_rounded`/`_accumulate_`/`_subtract_` for `__ARM_ARCH_7EM__`) and `AudioWaveformSine[257]` (`data_waveforms.c`) — all in the fork, portable via `__ARM_ARCH_7EM__` (RT1176 M7 satisfies it).

---

## Task 1: AudioSynthWaveformSine sanity gate (STAGE_SYNTH)

Prove the synth compiles/links against the core and produces a real sine — **before** it's used to test output, so a later gate failure is unambiguous. Reuse `audioinput_i2s_test`'s scaffolding; verify via `synth → analyze_peak` (a full-scale sine's peak ≈ its amplitude).

**Files:**
- Create: `evkb/audiooutput_i2s_test/CMakeLists.txt` (copy from `evkb/audioinput_i2s_test/CMakeLists.txt`)
- Create: `evkb/audiooutput_i2s_test/toolchain/rt1170-evkb.toolchain.cmake` (copy from `audioinput_i2s_test/toolchain/`)
- Create: `evkb/audiooutput_i2s_test/audiooutput_i2s_test.cpp`
- Create: `evkb/audiooutput_i2s_test/run_qemu_audiooutput.sh` (copy from `audioinput_i2s_test/run_qemu_audioinput.sh`, strip the RX injector — no chardev yet)

- [ ] **Step 1: Copy the gate scaffolding.**

```bash
mkdir -p ~/Development/rt1170/evkb/audiooutput_i2s_test/toolchain
cd ~/Development/rt1170/evkb/audiooutput_i2s_test
cp ../audioinput_i2s_test/CMakeLists.txt .
cp ../audioinput_i2s_test/toolchain/rt1170-evkb.toolchain.cmake toolchain/
cp ../audioinput_i2s_test/run_qemu_audioinput.sh run_qemu_audiooutput.sh
```

- [ ] **Step 2: Point CMake at this gate's fork sources.** In `CMakeLists.txt`, update the project name and the explicit fork `target_sources` (they are outside the core GLOB) to the files this effort needs. Replace the `audioinput`-specific source list so it reads (keep the existing `$ENV{HOME}/Development/Audio` include-path line and the core-glob wiring exactly as the input gate had them):

```cmake
project(audiooutput_i2s_test C CXX ASM)
# ... (core glob + toolchain unchanged from audioinput_i2s_test) ...
target_sources(audiooutput_i2s_test.elf PRIVATE
    audiooutput_i2s_test.cpp
    $ENV{HOME}/Development/Audio/output_i2s.cpp
    $ENV{HOME}/Development/Audio/synth_sine.cpp
    $ENV{HOME}/Development/Audio/data_waveforms.c
    $ENV{HOME}/Development/Audio/control_wm8962.cpp
    $ENV{HOME}/Development/Audio/analyze_peak.cpp
)
target_include_directories(audiooutput_i2s_test.elf PRIVATE $ENV{HOME}/Development/Audio)
```

- [ ] **Step 3: Write the STAGE_SYNTH firmware** — `audiooutput_i2s_test.cpp`:

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"
#include "synth_sine.h"
#include "analyze_peak.h"

// Task 1: prove AudioSynthWaveformSine compiles + runs on RT1176 (dspinst.h
// __ARM_ARCH_7EM__ path) by measuring a full-scale sine's peak. No peripheral.
AudioSynthWaveformSine sine;
AudioAnalyzePeak       peak;
AudioConnection        patchCord(sine, 0, peak, 0);

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    AudioMemory(12);
    sine.frequency(1000.0f);
    sine.amplitude(0.5f);
    // Drive graph passes: no audio clock owns update here, so pend it directly
    // (the audiostream_test idiom — IRQ_SOFTWARE=44 runs update_all()).
    float pk = 0.0f;
    for (int i = 0; i < 200; i++) {
        NVIC_SET_PENDING(IRQ_SOFTWARE);
        for (volatile uint32_t d = 20000; d; d--) { }
        if (peak.available()) { float v = peak.read(); if (v > pk) pk = v; }
    }
    bool ok = pk > 0.40f && pk < 0.60f;   // full-scale sine @ amplitude 0.5
    Serial1.print("info synth_peak="); Serial1.println(pk, 4);
    Serial1.println(ok ? "STAGE_SYNTH=PASS" : "STAGE_SYNTH=FAIL");
    Serial1.println(ok ? "AUDIOOUTPUT_ALL=PASS" : "AUDIOOUTPUT_ALL=FAIL");
}
void loop() {}
```

- [ ] **Step 4: Strip the RX injector from the run script.** In `run_qemu_audiooutput.sh`, remove the `-chardev pipe,id=...rxinject` line and the fifo-pump `while`-loop (this gate has no RX). Keep the VCOM capture, `qrun` invocation, `sleep`, and the `grep`/PASS logic. Update the ELF name to `audiooutput_i2s_test.elf`.

- [ ] **Step 5: Build.**

Run: `cd ~/Development/rt1170/evkb/audiooutput_i2s_test && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build`
Expected: links `audiooutput_i2s_test.elf` (only the benign `DMAMEM` redefinition warning). If `synth_sine.cpp`/`dspinst.h`/`data_waveforms.c` fail to compile, that's the real risk to resolve here (missing intrinsic → add the `__ARM_ARCH_7EM__` macro to the fork's `utility/dspinst.h` from `~/.platformio/.../libraries/Audio/utility/dspinst.h`; do NOT touch the core).

- [ ] **Step 6: Run the gate.**

Run: `./run_qemu_audiooutput.sh 2>&1 | tail -6`
Expected: `info synth_peak=0.50xx`, `STAGE_SYNTH=PASS`, `AUDIOOUTPUT_ALL=PASS`. A wrong peak (0, clipped, or garbage) means the synth math/intrinsics are off on this target.

- [ ] **Step 7: Commit.**

```bash
cd ~/Development/rt1170/evkb
git add audiooutput_i2s_test/
git commit -m "audiooutput_i2s_test: STAGE_SYNTH gate (AudioSynthWaveformSine -> analyze_peak) green"
```

---

## Task 2: AudioOutputI2S::begin() `__IMXRT1176__` TX-DMA branch

The one piece of new node code. Add an `#elif defined(__IMXRT1176__)` branch to `AudioOutputI2S::begin()`, mirroring the `__IMXRT1062__` branch and the core's HW-verified `I2SClass::beginDMA()` (`I2S.cpp:137-149`), with the **`+0`** TDR alignment (pre-empting the input `+2` bug).

**Files:**
- Modify: `~/Development/Audio/output_i2s.cpp` — `AudioOutputI2S::begin()`, add the `__IMXRT1176__` branch before the final `#endif` of the platform `#if`.

- [ ] **Step 1: Add the `__IMXRT1176__` branch.** In `output_i2s.cpp`, `AudioOutputI2S::begin()`, immediately before the `#endif` that closes the `__IMXRT1062__` branch, insert:

```cpp
#elif defined(__IMXRT1176__)
	// SAI1 TX-DATA00 pin (GPIO_AD_21) mux is already set by config_i2s() above,
	// so this branch is just the TX DMA + enable. Mirrors the core's HW-verified
	// I2SClass::beginDMA() (I2S.cpp): SADDR advances through i2s_tx_buffer, DADDR
	// is the fixed FIFO register, TCSR = TE|BCE|FRDE.
	dma.TCD->SADDR = i2s_tx_buffer;
	dma.TCD->SOFF = 2;                    // advance the source (buffer) by 16 bits
	// imxrt1176.h has no DMA_TCD_ATTR_SSIZE/DSIZE macros (confirmed absent); use
	// DMAChannel.h's ATTR_SRC/ATTR_DST union fields (bit-identical to 0x0101),
	// the same idiom AudioInputI2S uses.
	dma.TCD->ATTR_SRC = 1;                // 16-bit source (i2s_tx_buffer)
	dma.TCD->ATTR_DST = 1;                // 16-bit dest (SAI1_TDR0)
	dma.TCD->NBYTES_MLNO = 2;
	dma.TCD->SLAST = -sizeof(i2s_tx_buffer);   // wrap the source each major loop
	dma.TCD->DOFF = 0;                    // dest is a fixed FIFO register
	dma.TCD->CITER_ELINKNO = sizeof(i2s_tx_buffer) / 2;
	dma.TCD->DLASTSGA = 0;                // dest fixed, no adjustment
	dma.TCD->BITER_ELINKNO = sizeof(i2s_tx_buffer) / 2;
	dma.TCD->CSR = DMA_TCD_CSR_INTHALF | DMA_TCD_CSR_INTMAJOR;
	// Write the LOWER 16 bits of TDR0 (offset +0), NOT +2. The Teensy-1062
	// reference uses +2 (its SAI left-packs); our core's configureSAI (FBT=15)
	// right-packs into the lower half -- proven by the HW-verified core TX path
	// (I2S.cpp: i2s_dma.destination(*(volatile uint16_t *)&SAI1_TDR0), +0). Using
	// +2 would drop every sample into the ignored upper half -> silence on J101.
	// (This is the TX analog of the AudioInputI2S RDR0 +0 fix.)
	dma.TCD->DADDR = (void *)((uint32_t)&SAI1_TDR0);
	dma.triggerAtHardwareEvent(DMAMUX_SOURCE_SAI1_TX);
	dma.enable();
	// Enable the transmitter: TX + bit clock + FIFO DMA request. config_i2s()
	// set up TCRn and left TCSR=0; FRDE makes the SAI raise the TX DMA request as
	// the FIFO drains. Matches the core's beginDMA (hw->tcsr |= TE|BCE|FRDE).
	SAI1_TCSR = SAI_TCSR_TE | SAI_TCSR_BCE | SAI_TCSR_FRDE;
#endif
```

(The existing common tail `update_responsibility = update_setup(); dma.attachInterrupt(isr);` runs after this `#endif` for all targets — do not duplicate it.)

- [ ] **Step 2: Confirm macro spellings compile.** These are used by the branch and must exist in `cores/imxrt1176/imxrt1176.h`: `SAI1_TDR0`, `DMAMUX_SOURCE_SAI1_TX`, `SAI_TCSR_TE`, `SAI_TCSR_BCE`, `SAI_TCSR_FRDE`, `DMA_TCD_CSR_INTHALF`, `DMA_TCD_CSR_INTMAJOR`. Verify:

Run: `grep -oE 'SAI1_TDR0|DMAMUX_SOURCE_SAI1_TX|SAI_TCSR_(TE|BCE|FRDE)|DMA_TCD_CSR_INT(HALF|MAJOR)' ~/Development/rt1170/evkb/cores/imxrt1176/imxrt1176.h | sort -u`
Expected: all eight names present. (`SAI_TCSR_FRDE` bit is `1u<<0`; if by any chance absent, it is defined alongside the other `SAI_TCSR_*` in `imxrt1176.h` — add `#define SAI_TCSR_FRDE (1u<<0)` there only if missing, but it should exist from the SAI RX/TX work.)

- [ ] **Step 3: Commit the fork branch** (compile-verified at Task 3, the first link point).

```bash
cd ~/Development/Audio
git add output_i2s.cpp
git commit -m "output_i2s: AudioOutputI2S::begin() RT1176 TX-DMA branch (SAI1 TX, TDR0 +0)"
```

---

## Task 3: Full output gate — synth → AudioOutputI2S → SAI1 TX tap (STAGE_TONE)

Extend the Task 1 gate with `AudioOutputI2S` and the `sai1-tap` chardev. This is where Tasks 1–2 first compile+link+run together; a build error points back to the offending fork file.

**Files:**
- Modify: `evkb/audiooutput_i2s_test/audiooutput_i2s_test.cpp` (add the output node + connections)
- Modify: `evkb/audiooutput_i2s_test/run_qemu_audiooutput.sh` (add the `sai1-tap` chardev + host-side tap assert)
- Create: `evkb/audiooutput_i2s_test/check_tap.py` (assert the tap carries the sine)

- [ ] **Step 1: Extend the firmware** to drive real output. Replace `audiooutput_i2s_test.cpp` with:

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"
#include "synth_sine.h"
#include "analyze_peak.h"
#include "output_i2s.h"
#include "control_wm8962.h"

// Task 3: AudioSynthWaveformSine -> AudioOutputI2S (SAI1 TX DMA) + a synth peak
// sanity. STAGE_SYNTH proves the source; STAGE_TONE is asserted host-side from
// the SAI1 TX tap file. QEMU is timer/tap-paced -> proves the graph->SAI-TX
// plumbing; the real 44.1 kHz rate + audibility are the HW item (Task 4).
AudioSynthWaveformSine sine;
AudioAnalyzePeak       peak;
AudioOutputI2S         out;
AudioConnection        pcPeak(sine, 0, peak, 0);   // sanity tap of the source
AudioConnection        pcL(sine, 0, out, 0);       // left  = sine
AudioConnection        pcR(sine, 0, out, 1);       // right = sine
AudioControlWM8962     wm;

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    AudioMemory(12);
    wm.enable();
    sine.frequency(1000.0f);
    sine.amplitude(0.5f);
    // out ctor auto-called begin(): SAI1 TX DMA is running; its isr pends
    // update_all() as the FIFO drains, so the graph self-clocks. Give it time.
    float pk = 0.0f;
    uint32_t t0 = millis();
    while (millis() - t0 < 500) {
        if (peak.available()) { float v = peak.read(); if (v > pk) pk = v; }
        yield();
    }
    bool synth_ok = pk > 0.40f && pk < 0.60f;
    Serial1.print("info synth_peak="); Serial1.println(pk, 4);
    Serial1.println(synth_ok ? "STAGE_SYNTH=PASS" : "STAGE_SYNTH=FAIL");
    // STAGE_TONE is decided host-side from the tap; emit a marker so the run
    // script knows the firmware reached steady state.
    Serial1.println("TONE_PLAYING");
}
void loop() {
    static uint32_t last = 0;
    if (millis() - last > 500) { last = millis(); Serial1.println("TONE_PLAYING"); }
}
```

- [ ] **Step 2: Add the `sai1-tap` chardev to the run script.** In `run_qemu_audiooutput.sh`, add (mirroring `evkb/i2s_audio_test/run_qemu_i2s.sh:10`) a `TAP="$DIR/tap.raw"` and pass `-chardev file,id=sai1-tap,path="$TAP"` to the `qrun`/QEMU invocation. Run QEMU long enough to accumulate samples (`sleep 5` as elsewhere).

- [ ] **Step 3: Write the host-side tap check** — `check_tap.py`:

```python
#!/usr/bin/env python3
# Assert the SAI1 TX tap carries the non-silent sine AudioOutputI2S transmitted.
import sys, struct
path = sys.argv[1]
data = open(path, "rb").read()
n = len(data) // 2
if n == 0:
    print("STAGE_TONE=FAIL (empty tap)"); sys.exit(1)
samples = struct.unpack("<%dh" % n, data[:n*2])
peak = max(abs(s) for s in samples)
# amplitude 0.5 full-scale -> ~16384; accept a wide band (QEMU FIFO/timing).
ok = peak > 4000
print("info tap_peak=%d (%.3f fs)" % (peak, peak/32767.0))
print("STAGE_TONE=PASS" if ok else "STAGE_TONE=FAIL")
sys.exit(0 if ok else 1)
```

- [ ] **Step 4: Wire the check into the run script.** After the QEMU run, have `run_qemu_audiooutput.sh` invoke `python3 check_tap.py "$TAP"` and fold `STAGE_TONE` + a final `AUDIOOUTPUT_ALL` (PASS iff `STAGE_SYNTH` in VCOM and `STAGE_TONE` from the check both pass) into the printed output.

- [ ] **Step 5: Build + run.**

Run: `cd ~/Development/rt1170/evkb/audiooutput_i2s_test && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build && ./run_qemu_audiooutput.sh 2>&1 | tail -10`
Expected: `STAGE_SYNTH=PASS`, `info tap_peak=~16000`, `STAGE_TONE=PASS`, `AUDIOOUTPUT_ALL=PASS`. If the tap is empty/silent: check the TX DMA is firing (DADDR `+0`, `TCSR=TE|BCE|FRDE`, `DMAMUX_SOURCE_SAI1_TX`) and that `update()` is queuing blocks (STAGE_SYNTH proves the source, so a silent tap isolates the fault to AudioOutputI2S/the DMA). Do NOT weaken the `peak > 4000` threshold to force a pass.

- [ ] **Step 6: Commit.**

```bash
cd ~/Development/rt1170/evkb
git add audiooutput_i2s_test/
git commit -m "audiooutput_i2s_test: STAGE_TONE gate (synth -> AudioOutputI2S -> SAI1 TX tap) green"
```

---

## Task 4: Hardware test — audible tone on J101 (PAUSE for user)

QEMU proves the graph→SAI-TX plumbing but is tap/timer-paced; the silicon proof is an audible tone. The controller drives flash+VCOM; **the user listens on J101** (headphone/line-out) — a brief bench moment, like the mic test.

**Files:** none (uses the Task 3 ELF).

- [ ] **Step 1: Flash the gate.**

```bash
LS=/Applications/LinkServer_26.6.137/LinkServer
gtimeout 90 "$LS" flash MIMXRT1176:MIMXRT1170-EVKB load ~/Development/rt1170/evkb/audiooutput_i2s_test/build/audiooutput_i2s_test.elf
```
Expected: `Finished writing Flash successfully`.

- [ ] **Step 2: Capture VCOM + confirm the firmware runs.**

Capture `/dev/cu.usbmodem*` @115200 for ~5 s (pyserial + gtimeout, per the macOS-serial-capture note). Expected: `STAGE_SYNTH=PASS` then repeating `TONE_PLAYING`. (On HW `STAGE_TONE` has no tap — it's QEMU-only; the audible tone is the proof.)

- [ ] **Step 3: PAUSE — user listens.** Ask the user to plug headphones / line-in into **J101** and confirm they hear a steady ~1 kHz tone. If measurable, sanity-check the pitch (≈1 kHz ⇒ the 44.1 kHz clock is right; a wrong rate shifts the pitch proportionally). Watch for the input-effort lesson: if silent on HW despite a green QEMU tap, suspect the DMA not actually draining on silicon (the analog of the TX-enable/alignment class of bug — though `+0` is pre-empted here); scope `SAI1_TCSR` (TE/BCE/FRDE set, running) and the DMA `CITER` advancing.

- [ ] **Step 4: Record the HW result** in the memory note (Task 5): tone heard (yes/no), measured pitch if any, any silicon-vs-QEMU divergence found.

---

## Task 5: Memory note + push

**Files:**
- Create: `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170/memory/rt1176-audiooutput-i2s.md`
- Modify: the memory `MEMORY.md` (add a one-line pointer)

- [ ] **Step 1: Write the memory note** — `rt1176-audiooutput-i2s.md` with frontmatter (`type: project`), covering: `AudioOutputI2S` playback node in the `newdigate/Audio` fork; the `begin()` `__IMXRT1176__` TX-DMA branch (SADDR=i2s_tx_buffer advancing, DADDR=&SAI1_TDR0 **+0**, DMAMUX_SOURCE_SAI1_TX, TCSR=TE|BCE|FRDE); `AudioSynthWaveformSine` ported for free via `utility/dspinst.h` `__ARM_ARCH_7EM__` (no core change); config_i2s/isr/update/codec-playback all pre-existing; the `+0` TDR alignment (TX analog of input's RDR0 +0); QEMU gate (STAGE_SYNTH + sai1-tap STAGE_TONE) + HW (tone on J101); the HW result from Task 4. Link `[[rt1176-audioinput-i2s]]`, `[[rt1176-audiostream]]`, `[[rt1176-i2s-sai]]`, `[[rt1176-edma-dmachannel]]`.

- [ ] **Step 2: Add the `MEMORY.md` pointer** — one line under the `rt1176-audioinput-i2s` entry.

- [ ] **Step 3: Push when the user asks.** Confirm the unpushed set (`cd ~/Development/Audio && git log --oneline origin/master..master` — the output_i2s branch commit; synth files were already present). On the user's go: `git push origin master`. Cores untouched (nothing to push); evkb stays local. Report the pushed range.

---

## Self-review notes (author)

- **Spec coverage:** begin() TX-DMA branch → Task 2; AudioSynthWaveformSine → Task 1; codec reuse → Task 3 firmware (`wm.enable()`); QEMU tap (STAGE_TONE) → Task 3; HW tone on J101 → Task 4; memory+push → Task 5. TDR0 `+0`, dcache no-op, DSP-intrinsic, update() platform-independence risks all addressed (dcache/intrinsic/update resolved during exploration; `+0` baked into Task 2).
- **Type consistency:** `AudioSynthWaveformSine`/`AudioOutputI2S`/`AudioAnalyzePeak`/`AudioControlWM8962`, `frequency(float)`/`amplitude(float)`, `SAI1_TDR0`, `DMAMUX_SOURCE_SAI1_TX`, `SAI_TCSR_TE|BCE|FRDE`, `ATTR_SRC/ATTR_DST` used consistently across tasks.
- **No core change** expected; if a missing DSP intrinsic surfaces, fix it in the fork's `utility/dspinst.h`, not the core.
