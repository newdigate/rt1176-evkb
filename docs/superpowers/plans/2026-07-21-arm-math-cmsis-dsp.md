# CMSIS-DSP (`arm_math`) Manifest Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Provide `arm_math.h` + CMSIS-DSP to RT1176 firmware as a pinned manifest library, proven by a standalone known-answer gate and a real Audio `synth_sine → filter_fir → analyze_fft256` chain (QEMU + EVKB hardware).

**Architecture:** Two new pinned fetches in `evkb.cmake` (ARM-software/CMSIS-DSP v1.17.1 + ARM-software/CMSIS_6 v6.3.0 headers-only) and one `import_evkb_cmsis_dsp()` macro that builds a `CMSIS-DSP` static-library target from the per-group amalgamation sources with the shared `teensy_flags`. Two new gate examples prove it end-to-end. No Audio-fork code changes — `filter_fir.cpp`/`analyze_fft256.cpp` compile as-is once `arm_math.h` resolves.

**Tech Stack:** CMake 3.24 / teensy-cmake-macros, ARM GCC 10, qemu2 `mimxrt1170-evk` via `tools/qrun`, LinkServer for HW.

**Spec:** `docs/superpowers/specs/2026-07-21-arm-math-cmsis-dsp-design.md`

**Repos touched:** `evkb` only (plus `~/Development/Audio` docs in Task 7). All paths below are relative to `~/Development/rt1170/evkb/` unless absolute.

**Key background for a zero-context engineer:**
- The two-gate rule: every capability needs a QEMU gate (`./run_qemu.sh`, NEVER `sh run_qemu.sh` — it re-execs under gtimeout) AND a hardware run on the EVKB. Silicon wins.
- CMSIS-DSP ships BOTH amalgamated per-group sources (`Source/<Group>/<Group>.c`, which `#include` the individual files) AND the individual files. Compiling `Source/*/*.c` double-defines every symbol. Compile ONLY the amalgams.
- Consumers of `<arm_math.h>` transitively include `cmsis_compiler.h` (CMSIS-Core). CMSIS-Core's include dir must therefore be PUBLIC on the target, but nothing from it is compiled and it must NOT be added to `cores` or any non-CMSIS target.
- Audio graph updates are driven manually with `NVIC_SET_PENDING(IRQ_SOFTWARE)` (see `examples/audio/audiostream_test/`) — no I2S/DMA needed, so no `-icount` and zero QEMU model changes.
- The serial console on hardware: macOS `cat` resets the baud — always use `tools/rt1170-console.py`. Start the reader BEFORE `LinkServer run`.

---

### Task 1: `arm_math_test` gate skeleton (red)

Create the standalone known-answer gate. It must FAIL to configure (CMSIS-DSP not in the manifest yet) — that's the failing test for Task 2.

**Files:**
- Create: `examples/framework/arm_math_test/CMakeLists.txt`
- Create: `examples/framework/arm_math_test/arm_math_test.cpp`
- Create: `examples/framework/arm_math_test/run_qemu.sh` (mode 755)
- Create: `examples/framework/arm_math_test/toolchain/rt1170-evkb.toolchain.cmake` (copy)

- [ ] **Step 1: Create the directory and copy the toolchain file**

```bash
cd ~/Development/rt1170/evkb/examples/framework
mkdir -p arm_math_test/toolchain
cp ../audio/audiostream_test/toolchain/rt1170-evkb.toolchain.cmake arm_math_test/toolchain/
ls arm_math_test/toolchain/    # must show rt1170-evkb.toolchain.cmake
```

- [ ] **Step 2: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(arm_math_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
import_evkb_cmsis_dsp()

teensy_add_executable(arm_math_test arm_math_test.cpp)
teensy_target_link_libraries(arm_math_test cores)
target_link_libraries(arm_math_test.elf CMSIS-DSP stdc++)
```

- [ ] **Step 3: Write `arm_math_test.cpp`**

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include <arm_math.h>
#include <math.h>
#include <stdlib.h>

// Known-answer tests for the CMSIS-DSP manifest library. Pure CPU math:
// identical output expected in QEMU and on silicon.

// STAGE_FFT: 256-pt radix-4 complex q15 FFT of cos(2*pi*8*i/256) at 0.5 FS.
// All energy must land in bin 8 (mirror bin 248 ignored; we scan 1..127).
static q15_t fft_buf[512];
static bool stage_fft(void) {
    arm_cfft_radix4_instance_q15 inst;
    if (arm_cfft_radix4_init_q15(&inst, 256, 0, 1) != ARM_MATH_SUCCESS) {
        Serial1.println("ARM-MATH: fft init failed");
        return false;
    }
    for (int i = 0; i < 256; i++) {
        float c = cosf(2.0f * (float)M_PI * 8.0f * (float)i / 256.0f);
        fft_buf[2 * i]     = (q15_t)lrintf(c * 16384.0f);
        fft_buf[2 * i + 1] = 0;
    }
    arm_cfft_radix4_q15(&inst, fft_buf);          // in-place, output scaled 1/N
    uint32_t best = 0, best_mag = 0, second = 0;
    for (uint32_t k = 1; k < 128; k++) {
        int32_t re = fft_buf[2 * k], im = fft_buf[2 * k + 1];
        uint32_t mag = (uint32_t)(re * re + im * im);
        if (mag > best_mag) { second = best_mag; best_mag = mag; best = k; }
        else if (mag > second) { second = mag; }
    }
    Serial1.print("ARM-MATH: fft bin=");  Serial1.print(best);
    Serial1.print(" mag2=");              Serial1.print(best_mag);
    Serial1.print(" next2=");             Serial1.println(second);
    return best == 8 && best_mag > 0 && best_mag > 4 * second;
}

// STAGE_FIR: unit impulse through an 8-tap FIR echoes the coefficients.
static const q15_t fir_coeffs[8] = {1000, 2000, 3000, 4000, 4000, 3000, 2000, 1000};
static q15_t fir_state[8 + 32 - 1];
static bool stage_fir(void) {
    arm_fir_instance_q15 f;
    if (arm_fir_init_q15(&f, 8, (q15_t *)fir_coeffs, fir_state, 32) != ARM_MATH_SUCCESS) {
        Serial1.println("ARM-MATH: fir init failed");
        return false;
    }
    q15_t x[32] = {0}, y[32] = {0};
    x[0] = 32767;                                  // ~1.0 in q15
    arm_fir_fast_q15(&f, x, y, 32);
    bool ok = true;
    for (int i = 0; i < 8; i++)
        if (abs((int)y[i] - (int)fir_coeffs[i]) > 4) ok = false;
    for (int i = 8; i < 32; i++)
        if (abs((int)y[i]) > 4) ok = false;
    Serial1.print("ARM-MATH: fir y=");
    for (int i = 0; i < 8; i++) { Serial1.print((int)y[i]); Serial1.print(i < 7 ? "," : "\n"); }
    return ok;
}

// STAGE_SIN: arm_sin_q31 vs libm across one turn (q31 angle maps [0,1) -> [0,2pi)).
static bool stage_sin(void) {
    float maxerr = 0.0f;
    for (int i = 0; i < 64; i++) {
        float x = (float)i / 64.0f;
        q31_t a = (q31_t)llrintf(x * 2147483648.0f);
        float got  = (float)arm_sin_q31(a) / 2147483648.0f;
        float want = sinf(2.0f * (float)M_PI * x);
        float err  = fabsf(got - want);
        if (err > maxerr) maxerr = err;
    }
    Serial1.print("ARM-MATH: sin maxerr="); Serial1.println(maxerr, 7);
    return maxerr < 1.0e-4f;
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    bool fft = stage_fft();
    bool fir = stage_fir();
    bool sn  = stage_sin();
    Serial1.println(fft ? "STAGE_FFT=PASS" : "STAGE_FFT=FAIL");
    Serial1.println(fir ? "STAGE_FIR=PASS" : "STAGE_FIR=FAIL");
    Serial1.println(sn  ? "STAGE_SIN=PASS" : "STAGE_SIN=FAIL");
    Serial1.println((fft && fir && sn) ? "ARM_MATH_ALL=PASS" : "ARM_MATH_ALL=FAIL");
}
void loop() {}
```

- [ ] **Step 4: Write `run_qemu.sh`** (mirror of the audiostream runner)

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/arm_math_test.elf"; OUT="$DIR/arm_math.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/arm_math.dbg" &
P=$!; gate_pid $P; sleep 5; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "STAGE_FFT=PASS"    "$OUT" || { echo "FAIL: fft"; exit 1; }
grep -q "STAGE_FIR=PASS"    "$OUT" || { echo "FAIL: fir"; exit 1; }
grep -q "STAGE_SIN=PASS"    "$OUT" || { echo "FAIL: sin"; exit 1; }
grep -q "ARM_MATH_ALL=PASS" "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: ARM_MATH_ALL"
```

Then: `chmod +x run_qemu.sh`

- [ ] **Step 5: Run configure to verify it fails for the right reason**

```bash
cd ~/Development/rt1170/evkb/examples/framework/arm_math_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
```

Expected: FAIL with `Unknown CMake command "import_evkb_cmsis_dsp"` (the macro doesn't exist yet). Any OTHER error (toolchain not found, cores fetch failure) means the skeleton is wrong — fix before proceeding.

- [ ] **Step 6: Commit the red skeleton**

```bash
cd ~/Development/rt1170/evkb
git add examples/framework/arm_math_test
git commit -m "test: arm_math_test known-answer gate (red - CMSIS-DSP not in manifest yet)"
```

---

### Task 2: CMSIS-DSP in the `evkb.cmake` manifest (green)

**Files:**
- Modify: `evkb.cmake` (manifest block ~line 66, helpers end ~line 94)

- [ ] **Step 1: Add the two pinned manifest entries**

In `evkb.cmake`, directly after the `_evkb_lib(Bounce2 …)` line, add:

```cmake
# ARM upstream (not Arduino-layout; consumed via import_evkb_cmsis_dsp below).
# CMSIS-Core is a headers-only dependency of CMSIS-DSP (cmsis_compiler.h et al).
_evkb_lib(CMSIS-DSP  ${_dev}/CMSIS-DSP https://github.com/ARM-software/CMSIS-DSP 4b4fa8ff218ca5ac20bad71b653a37d93815f24b .) # v1.17.1
_evkb_lib(CMSIS-Core ${_dev}/CMSIS_6   https://github.com/ARM-software/CMSIS_6   45dab712ad84f8cbbf2b7bfc089c19088507df6f .) # v6.3.0
```

(Those SHAs are the peeled `^{}` commits of tags `v1.17.1` and `v6.3.0`, resolved 2026-07-21 via `git ls-remote --tags`.)

- [ ] **Step 2: Add the import macro**

In `evkb.cmake`, after the `import_evkb_library` macro (before the `--- the imxrt1176 core ---` section), add:

```cmake
# import_evkb_cmsis_dsp(): CMSIS-DSP as a static lib target named CMSIS-DSP.
# Not Arduino-layout, so it bypasses import_arduino_library. Compiles ONLY the
# per-group amalgamation sources — Source/<G>/<G>.c #includes the individual
# files, so globbing Source/*/*.c would double-define every symbol. CMSIS-Core's
# include dir is PUBLIC because <arm_math.h> reaches cmsis_compiler.h from every
# consumer TU; it is deliberately NOT added to cores or any other target.
macro(import_evkb_cmsis_dsp)
    if(NOT TARGET CMSIS-DSP)
        evkb_library_dir(CMSIS-DSP _evkb_cmsisdsp_dir)
        evkb_library_dir(CMSIS-Core _evkb_cmsiscore_dir)
        set(_evkb_cmsisdsp_srcs "")
        foreach(_g BasicMathFunctions BayesFunctions CommonTables
                   ComplexMathFunctions ControllerFunctions DistanceFunctions
                   FastMathFunctions FilteringFunctions InterpolationFunctions
                   MatrixFunctions QuaternionMathFunctions SVMFunctions
                   StatisticsFunctions SupportFunctions TransformFunctions
                   WindowFunctions)
            list(APPEND _evkb_cmsisdsp_srcs "${_evkb_cmsisdsp_dir}/Source/${_g}/${_g}.c")
            if(EXISTS "${_evkb_cmsisdsp_dir}/Source/${_g}/${_g}F16.c")
                list(APPEND _evkb_cmsisdsp_srcs "${_evkb_cmsisdsp_dir}/Source/${_g}/${_g}F16.c")
            endif()
        endforeach()
        add_library(CMSIS-DSP STATIC ${_evkb_cmsisdsp_srcs})
        target_include_directories(CMSIS-DSP
            PUBLIC  "${_evkb_cmsisdsp_dir}/Include"
                    "${_evkb_cmsiscore_dir}/CMSIS/Core/Include"
            PRIVATE "${_evkb_cmsisdsp_dir}/PrivateInclude")
        target_link_libraries(CMSIS-DSP PRIVATE teensy_flags)
    endif()
endmacro()
```

Note: `teensy_flags` (the INTERFACE lib carrying the per-language CM7 compile
options, including `-ffunction-sections -fdata-sections`) exists by the time any
example calls this macro — `evkb.cmake`'s own bottom section imports `cores`
first, which creates it.

- [ ] **Step 3: Configure + build the gate**

```bash
cd ~/Development/rt1170/evkb/examples/framework/arm_math_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
```

Expected: configure logs `resolve_arduino_library_auto(CMSIS-DSP): fetching https://github.com/ARM-software/CMSIS-DSP @ 4b4fa8f…` (or `local` if a checkout exists), then builds `libCMSIS-DSP.a` and links `arm_math_test.elf` clean. If a group amalgam file is missing at v1.17.1 (`Cannot find source file`), delete that group from the `foreach` list — do NOT substitute individual files.

- [ ] **Step 4: Run the QEMU gate**

```bash
./run_qemu.sh
```

Expected output ends with:
```
STAGE_FFT=PASS
STAGE_FIR=PASS
STAGE_SIN=PASS
ARM_MATH_ALL=PASS
PASS: ARM_MATH_ALL
```

If STAGE_FFT reports `bin=8` but fails the `4 * next2` margin, print the top-3 bins and reconsider the margin only with evidence — do not silently weaken the assertion.

- [ ] **Step 5: Save the QEMU transcript and commit**

```bash
cp arm_math.uart transcript_qemu.txt
cd ~/Development/rt1170/evkb
git add evkb.cmake examples/framework/arm_math_test
git commit -m "feat: CMSIS-DSP v1.17.1 pinned manifest library + green arm_math gate"
```

---

### Task 3: `arm_math_test` on hardware

- [ ] **Step 1: Clear probes and start the console reader FIRST**

```bash
pkill LinkServer; pkill redlinkserv; sleep 1
cd ~/Development/rt1170/evkb/examples/framework/arm_math_test
python3 ~/Development/rt1170/evkb/tools/rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > transcript_hw_evkb.txt 2>&1 &
```

- [ ] **Step 2: Flash + free-run**

```bash
/Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/arm_math_test.elf
sleep 5
```

- [ ] **Step 3: Verify tokens, stop the reader**

```bash
pkill -f rt1170-console.py
grep "ARM_MATH_ALL=PASS" transcript_hw_evkb.txt && grep "STAGE_FFT=PASS" transcript_hw_evkb.txt
```

(`pkill -f`, not `kill %1` — the reader was backgrounded in an earlier, separate shell invocation, so job control doesn't reach it.)

Expected: both greps hit, and the `fft bin=… mag2=…` / `fir y=…` / `sin maxerr=…` info lines are numerically identical to `transcript_qemu.txt` (integer math must match exactly; `sin maxerr` may differ in the last decimal). If the console is silent: power-cycle the board and re-run `LinkServer run` (probe may have left the core halted); see CLAUDE.md flash notes.

- [ ] **Step 4: Commit the HW transcript**

```bash
cd ~/Development/rt1170/evkb
git add examples/framework/arm_math_test/transcript_hw_evkb.txt examples/framework/arm_math_test/transcript_qemu.txt
git commit -m "test: arm_math_test HW-verified on EVKB (transcripts)"
```

---

### Task 4: `filter_fir_test` Audio-chain gate (QEMU)

First RT1176 compile of `filter_fir.cpp` + `analyze_fft256.cpp`. Graph driven by manual `IRQ_SOFTWARE` pends, exactly like `audiostream_test` — no I2S, no `-icount`.

**Numbers used below (44.1 kHz, 256-pt FFT, bin width 172.266 Hz):**
- Filter: 8-tap boxcar, every coeff q15 4096 (=0.125) — a crude low-pass with an exact null at fs/8 = 5512.5 Hz. `arm_fir_init_q15` needs an even tap count ≥ 4 ✓.
- Passband probe: 1033.59 Hz = exactly bin 6. |H| there ≈ 0.944 (−0.5 dB).
- Stopband probe: 5512.5 Hz = exactly bin 32 = the boxcar null.
- `AudioAnalyzeFFT256` averages 8 FFTs (AUDIO_BLOCK_SAMPLES=128 path) and applies a Hanning window (coherent gain 0.5): expected passband read ≈ 0.9 × 0.944 × 0.5 ≈ 0.42 → assert > 0.30. Stopband read ≈ 0 → assert < 0.04.

**Files:**
- Create: `examples/audio/filter_fir_test/CMakeLists.txt`
- Create: `examples/audio/filter_fir_test/filter_fir_test.cpp`
- Create: `examples/audio/filter_fir_test/run_qemu.sh` (755)
- Create: `examples/audio/filter_fir_test/toolchain/rt1170-evkb.toolchain.cmake` (copy)

- [ ] **Step 1: Create the directory + toolchain copy**

```bash
cd ~/Development/rt1170/evkb/examples/audio
mkdir -p filter_fir_test/toolchain
cp audiostream_test/toolchain/rt1170-evkb.toolchain.cmake filter_fir_test/toolchain/
```

- [ ] **Step 2: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(filter_fir_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
import_evkb_cmsis_dsp()
evkb_library_dir(Audio EVKB_AUDIO_DIR)

teensy_add_executable(filter_fir_test filter_fir_test.cpp)

# Audio nodes are cherry-picked (evkb gates do NOT glob the Audio fork).
# synth_sine needs data_waveforms (AudioWaveformSine); analyze_fft256 needs
# data_windows (Hanning) + utility/sqrt_integer.
target_sources(filter_fir_test.elf PRIVATE
    ${EVKB_AUDIO_DIR}/synth_sine.cpp
    ${EVKB_AUDIO_DIR}/filter_fir.cpp
    ${EVKB_AUDIO_DIR}/analyze_fft256.cpp
    ${EVKB_AUDIO_DIR}/analyze_peak.cpp
    ${EVKB_AUDIO_DIR}/data_waveforms.c
    ${EVKB_AUDIO_DIR}/data_windows.c
    ${EVKB_AUDIO_DIR}/utility/sqrt_integer.c)
target_include_directories(filter_fir_test.elf PRIVATE
    ${EVKB_AUDIO_DIR} ${EVKB_AUDIO_DIR}/utility)

teensy_target_link_libraries(filter_fir_test cores)
target_link_libraries(filter_fir_test.elf CMSIS-DSP stdc++)
```

- [ ] **Step 3: Write `filter_fir_test.cpp`**

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"
#include "synth_sine.h"
#include "filter_fir.h"
#include "analyze_fft256.h"
#include "analyze_peak.h"
#include <math.h>

// sine -> FIR low-pass -> FFT256 (+peak tap). Passband tone must survive,
// stopband tone (at the boxcar's exact null) must vanish.

static AudioSynthWaveformSine sine1;
static AudioFilterFIR         fir1;
static AudioAnalyzeFFT256     fft1;
static AudioAnalyzePeak       peak1;
static AudioConnection c1(sine1, 0, fir1, 0);
static AudioConnection c2(fir1, 0, fft1, 0);
static AudioConnection c3(fir1, 0, peak1, 0);

// 8-tap boxcar (each 0.125 in q15): exact null at fs/8 = 5512.5 Hz.
static const short lp_coeffs[8] = {4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096};

static void pump(int n) {
    for (int i = 0; i < n; i++) {
        NVIC_SET_PENDING(IRQ_SOFTWARE);   // stand in for the audio clock
        delayMicroseconds(200);
    }
}

// Set the tone, flush the filter+FFT averaging pipeline, return a fresh read.
static float measure(float freq, int bin) {
    sine1.frequency(freq);
    pump(48);                  // 24 FFTs -> 3 full 8-FFT averaging rounds settle
    (void)fft1.available();    // discard any stale output flag
    for (int i = 0; i < 400; i++) {
        pump(2);
        if (fft1.available()) return fft1.read(bin);
    }
    return -1.0f;              // graph never produced output -> hard fail
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    AudioMemory(30);
    sine1.amplitude(0.9f);
    fir1.begin(lp_coeffs, 8);

    float pb = measure(1033.59f, 6);    // bin 6
    float pk = peak1.available() ? peak1.read() : -1.0f;
    float sb = measure(5512.5f, 32);    // bin 32 = boxcar null

    Serial1.print("FIR: pb=");   Serial1.println(pb, 4);
    Serial1.print("FIR: peak="); Serial1.println(pk, 4);
    Serial1.print("FIR: sb=");   Serial1.println(sb, 4);
    float atten_db = (sb > 0.0001f && pb > 0.0f) ? 20.0f * log10f(sb / pb) : -60.0f;
    Serial1.print("FIR: atten_db="); Serial1.println(atten_db, 1);

    bool pass_pb = (pb > 0.30f);
    bool pass_sb = (sb >= 0.0f && sb < 0.04f);
    Serial1.println(pass_pb ? "STAGE_PB=PASS" : "STAGE_PB=FAIL");
    Serial1.println(pass_sb ? "STAGE_SB=PASS" : "STAGE_SB=FAIL");
    Serial1.println((pass_pb && pass_sb) ? "FIR_ALL=PASS" : "FIR_ALL=FAIL");
}
void loop() {}
```

- [ ] **Step 4: Write `run_qemu.sh`**

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/filter_fir_test.elf"; OUT="$DIR/filter_fir.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/filter_fir.dbg" &
P=$!; gate_pid $P; sleep 10; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "STAGE_PB=PASS" "$OUT" || { echo "FAIL: passband"; exit 1; }
grep -q "STAGE_SB=PASS" "$OUT" || { echo "FAIL: stopband"; exit 1; }
grep -q "FIR_ALL=PASS"  "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: FIR_ALL"
```

Then: `chmod +x run_qemu.sh` (sleep is 10 s, not 5: the ~2000 pumped blocks at 200 µs each plus FFT math need the headroom).

- [ ] **Step 5: Build**

```bash
cd ~/Development/rt1170/evkb/examples/audio/filter_fir_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
```

Expected: clean build. Known wrinkle: `analyze_fft256.cpp` includes `"sqrt_integer.h"` and `"utility/dspinst.h"` — both resolve via the two include dirs added in Step 2. If `arm_math.h` is not found, `import_evkb_cmsis_dsp()` was not called before `teensy_add_executable`.

- [ ] **Step 6: Run the QEMU gate**

```bash
./run_qemu.sh
```

Expected: `FIR: pb=` ≈ 0.42 (must be > 0.30), `FIR: sb=` < 0.04, `atten_db` ≤ −20, `PASS: FIR_ALL`. If `pb=-1.0000`: the graph never produced FFT output — check that `AudioMemory(30)` ran and `IRQ_SOFTWARE` dispatch works (compare with `audiostream_test`, which uses the identical drive).

- [ ] **Step 7: Save transcript + commit**

```bash
cp filter_fir.uart transcript_qemu.txt
cd ~/Development/rt1170/evkb
git add examples/audio/filter_fir_test
git commit -m "test: filter_fir_test — first RT1176 compile of filter_fir + analyze_fft256, QEMU green"
```

---

### Task 5: `filter_fir_test` on hardware

- [ ] **Step 1: Reader first, then flash** (same recipe as Task 3)

```bash
pkill LinkServer; pkill redlinkserv; sleep 1
cd ~/Development/rt1170/evkb/examples/audio/filter_fir_test
python3 ~/Development/rt1170/evkb/tools/rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > transcript_hw_evkb.txt 2>&1 &
sleep 1
/Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/filter_fir_test.elf
sleep 8
pkill -f rt1170-console.py
```

- [ ] **Step 2: Verify**

```bash
grep "FIR_ALL=PASS" transcript_hw_evkb.txt && grep "FIR: pb=" transcript_hw_evkb.txt
```

Expected: pass, with pb/sb numerically identical to QEMU (the whole chain is integer q15 except the final float scaling). A divergence here is a real finding — record it, don't paper over it.

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/audio/filter_fir_test/transcript_hw_evkb.txt
git commit -m "test: filter_fir_test HW-verified on EVKB (transcript)"
```

---

### Task 6: license-audit coverage

**Files:**
- Modify: `tools/license-audit.sh:18-20` (REPOS), `:60` (GATES)

- [ ] **Step 1: Extend REPOS**

Change the REPOS assignment (line 18-20) to append the two ARM checkouts (the sweep loop already skips absent dirs with `[ -d "$r" ] || continue`):

```sh
REPOS="$EVKB/cores $HOME/Development/Ethernet $HOME/Development/NativeEthernet \
$HOME/Development/SdFat $HOME/Development/SPI $HOME/Development/Wire \
$HOME/Development/Audio $HOME/Development/SD $HOME/Development/PaulS_SD \
$HOME/Development/USBHost_t36 $HOME/Development/FNET $HOME/Development/lwip \
$HOME/Development/CMSIS-DSP $HOME/Development/CMSIS_6"
```

- [ ] **Step 2: Add the new gates to the Part-2 depfile audit**

Append to the GATES list (line 60): `examples/framework/arm_math_test:arm_math_test examples/audio/filter_fir_test:filter_fir_test` — this makes every compiled CMSIS-DSP source's header prove itself permissive, wherever it was resolved from (local or CPM cache).

- [ ] **Step 3: Run the audit**

```bash
~/Development/rt1170/evkb/tools/license-audit.sh
```

Expected: `LICENSE-AUDIT: PASS`. Apache-2.0 already passes the permissive check (FNET is Apache and has always been in REPOS). If a CMSIS file trips the sweep, inspect it individually — do not blanket-allowlist.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/license-audit.sh
git commit -m "chore: license-audit covers CMSIS-DSP + CMSIS_6 and the two new gates"
```

---

### Task 7: Docs, roadmap, push

**Files:**
- Modify: `examples/README.md` (category table: framework + audio rows)
- Modify: `~/Development/Audio/docs/rt1170-evkb-status.md`
- Modify: memory `rt1176-audio-library-roadmap.md` (+ new memory note if novel traps surfaced)

- [ ] **Step 1: examples/README.md** — add `arm_math_test` to the **framework** row and `filter_fir_test` to the **audio** row of the category table.

- [ ] **Step 2: Update the Audio status doc** (`~/Development/Audio/docs/rt1170-evkb-status.md`):
  - `filter_fir` row: `🔵` → `✅` with note "CMSIS-DSP manifest lib; HW-verified via evkb filter_fir_test".
  - `analyze_fft256` row: `🔵` → `✅` same note.
  - Remaining 🔵 rows (fft1024, ladder, flange, tonesweep): change note to "unblocked — CMSIS-DSP in evkb manifest; needs its own gate".
  - `Audio.h` row: still 🟡 (guard sweep pending), drop the 🔵 half.
  - Roadmap section: mark Phase A step 1 done (date + gate names).

- [ ] **Step 3: Commit + push both repos**

```bash
cd ~/Development/rt1170/evkb && git push
cd ~/Development/Audio && git add docs/rt1170-evkb-status.md && git commit -m "docs: CMSIS-DSP landed in evkb manifest — fir+fft256 HW-verified, 🔵 tier unblocked" && git push
```

- [ ] **Step 4: Update the roadmap memory** — in `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170/memory/rt1176-audio-library-roadmap.md`, mark the CMSIS-DSP blocker resolved (pins v1.17.1 / v6.3.0, `import_evkb_cmsis_dsp()`, both gates HW-verified) and note the amalgam-only compile rule. Update the MEMORY.md index line to match.

---

## Verification (whole plan)

1. Both gates green in QEMU: `examples/framework/arm_math_test/run_qemu.sh` and `examples/audio/filter_fir_test/run_qemu.sh`.
2. Both HW transcripts committed and matching QEMU numerically.
3. `tools/license-audit.sh` → PASS.
4. A pre-existing gate still passes (regression check — `evkb.cmake` changed): `examples/audio/audiostream_test/run_qemu_audiostream.sh`.
5. Fresh-user path: `cmake -B build-ff -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake -DEVKB_FORCE_FETCH=ON` in `arm_math_test` configures + builds (proves the pinned fetch, not just a local checkout).
