# `Audio.h` Full RT1176 Library Build (Phase A step 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** make `#include <Audio.h>` compile and link on the RT1176 CM7 by building the entire Audio fork as one library (all 87 `.cpp` + `utility/`), proven by a gate that includes the umbrella header and runs a known-answer chain.

**Architecture:** `import_evkb_library(Audio utility)` already GLOBs every source (`import_arduino_library`, CONFIGURE_DEPENDS); the work is (1) add the one missing dep (SerialFlash) to the manifest, (2) a `import_evkb_audio_full()` helper that puts every dep's headers (CMSIS-DSP shim+Include, Wire, SD, SerialFlash) on the Audio lib's compile path, (3) a discovery-then-sweep to make all 87 `.cpp` compile clean, (4) a gate. The compile-sweep task's size is genuinely unknown until the first full build (spec §7) — Task 2 inventories, Task 3 fixes.

**Tech Stack:** CMake/teensy-cmake-macros (globbing `import_arduino_library`, `teensy_flags` INTERFACE include target, linker RESCAN group), ARM GCC 10, qemu2 via `tools/qrun`, LinkServer.

**Spec:** `docs/superpowers/specs/2026-07-22-audio-h-full-build-design.md`

**Established facts (verified 2026-07-22):**
- `import_evkb_library(NAME [subdirs])` → `import_arduino_library(NAME <resolved-dir> [subdirs])` which `file(GLOB … CONFIGURE_DEPENDS)` all `*.cpp`/`*.c`/`*.S` at the lib root + each named subdir, creates an `${NAME}` library target, and adds the lib root (+ subdirs) to `teensy_flags` INTERFACE (global include path). All teensy libs are wrapped in a linker RESCAN group so link order/circular deps resolve.
- Audio fork: 87 `.cpp` + 6 `.c` at root, `utility/` has 2 more sources. `Audio.h` pulls 81 node headers.
- The only external dep not in the manifest is **SerialFlash** (`newdigate/SerialFlash`, MIT, HEAD `2b6f24168c1ca97af1138c4a5b10255b39c4ad0b`, 2 sources). Wire/SD/SdFat are manifest libs; CMSIS-DSP has `import_evkb_cmsis_dsp()`.
- **CMSIS gotcha:** `import_evkb_cmsis_dsp()` puts the arm_math shim + Include dirs on the `CMSIS-DSP` *target* (PUBLIC), NOT on `teensy_flags`. When the Audio lib globs-compiles its `arm_math.h`-using nodes (fft256/1024, filter_fir, filter_ladder, effect_flange, synth_tonesweep), those dirs MUST be on the Audio lib's compile path — the shim dir FIRST (it intercepts the core's `A0`-`A2` `#define` / IRQ-inline collisions, see [[rt1176-cmsis-dsp-arm-math]]). The helper adds them to `teensy_flags` so every globbed Audio TU sees them.
- Manifest Audio pin is currently `f5e47306…`; bump after the fork changes land.
- Board/gate conventions: gate runners use gate-lib.sh + `tools/qrun`, poll-loop (never fixed sleep), `./run_qemu.sh` (re-execs under gtimeout); HW = LinkServer + `tools/rt1170-console.py` reader-first on `/dev/cu.usbmodem5DQ2DDHVWO5EI3`.

**Repos touched:** evkb (evkb.cmake, tools, gate, docs), Audio (node compile fixes + docs), SerialFlash (push only, if unpushed).

---

### Task 1: SerialFlash → manifest

**Files:** Modify `evkb.cmake` (manifest block ~line 59); Modify `tools/license-audit.sh` (REPOS ~line 18).

- [ ] **Step 1: Confirm SerialFlash is pushed at the pinned SHA**

```bash
git -C ~/Development/SerialFlash rev-parse HEAD
git -C ~/Development/SerialFlash status -sb | head -1
git -C ~/Development/SerialFlash remote -v | head -1
```

Expected: HEAD `2b6f24168c1ca97af1138c4a5b10255b39c4ad0b`, remote `newdigate/SerialFlash`. If `status` shows unpushed commits, `git -C ~/Development/SerialFlash push` and re-read HEAD (use the pushed SHA below). If the working tree is dirty, STOP and report (the pin must be a clean pushed commit).

- [ ] **Step 2: Add the manifest entry**

In `evkb.cmake`, directly after the `_evkb_lib(SD …)` line (~line 59), add:

```cmake
_evkb_lib(SerialFlash    ${_dev}/SerialFlash          https://github.com/newdigate/SerialFlash     2b6f24168c1ca97af1138c4a5b10255b39c4ad0b .)
```

(Replace the SHA with the pushed HEAD from Step 1 if it differed.)

- [ ] **Step 3: Extend the license-audit REPOS sweep**

In `tools/license-audit.sh`, append `$HOME/Development/SerialFlash` to the `REPOS=` list (the last continued line currently ends `… $HOME/Development/CMSIS-DSP $HOME/Development/CMSIS_6`). Add it there. (SerialFlash is MIT — verified header text — so the Part-1 sweep passes; the Part-2 depfile audit will cover `play_serialflash_raw` once the full build compiles it.)

- [ ] **Step 4: Verify the pin resolves**

```bash
cd ~/Development/rt1170/evkb/examples/framework/arm_math_test
cmake -B build-sfcheck -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake >/tmp/sf.log 2>&1
grep -i "SerialFlash" /tmp/sf.log || echo "(SerialFlash not referenced by this gate — expected; the manifest entry just needs to parse)"
cmake --build build-sfcheck >/dev/null 2>&1 && echo "CONFIG+BUILD OK (manifest still parses)" ; rm -rf build-sfcheck
```

Expected: configure+build still succeed (the new manifest line doesn't break existing gates). If configure errors on the `_evkb_lib(SerialFlash …)` line, fix the syntax (match the column layout of the neighbors).

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add evkb.cmake tools/license-audit.sh
git commit -m "chore: SerialFlash manifest pin + license-audit REPOS (needed for the full Audio build)"
```

End the commit message body with: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

### Task 2: `import_evkb_audio_full()` helper + first full compile (discovery)

This task WRITES the full-build helper and RUNS the first whole-fork compile. Its deliverable is the **error inventory** — it is EXPECTED to fail with a list of compile errors, which Task 3 fixes. Do not fix nodes here; capture and categorize.

**Files:** Modify `evkb.cmake` (helpers, after `import_evkb_library`); Create `examples/audio/audio_h_test/CMakeLists.txt` + `audio_h_test.cpp` + `toolchain/` (scaffold).

- [ ] **Step 1: Write `import_evkb_audio_full()` in evkb.cmake**

After the `import_evkb_library` macro (before the `--- the imxrt1176 core ---` section), add:

```cmake
# import_evkb_audio_full(): build the ENTIRE Audio fork as one `Audio` library
# target for the CM7, with every dependency's headers on the global compile
# path so `#include <Audio.h>` resolves and every node .cpp compiles. Unlike the
# cherry-picking gates, this globs all 87 sources (import_arduino_library) — so
# every node must compile clean on RT1176 (empty-on-1176 nodes -> empty objects;
# --gc-sections drops the unused ones at link).
macro(import_evkb_audio_full)
    if(NOT TARGET Audio)
        # 1. CMSIS-DSP: build the lib AND put the shim+Include dirs on teensy_flags
        #    so the Audio nodes that #include <arm_math.h> compile. The shim dir
        #    MUST be first (intercepts the core's A0-A2/IRQ-inline collisions).
        import_evkb_cmsis_dsp()
        evkb_cmsis_dsp_cm4_sources(_evkb_audio_cmsis_srcs _evkb_audio_cmsis_incs)
        # (evkb_cmsis_dsp_cm4_sources returns the SAME include dir list the CM4
        #  path uses: shim, DSP/Include, DSP/PrivateInclude, Core/Include — the
        #  shim is first. We reuse only the include list, not the sources.)
        teensy_set_dynamic_properties()   # ensure teensy_flags exists
        foreach(_inc ${_evkb_audio_cmsis_incs})
            target_include_directories(teensy_flags INTERFACE "${_inc}")
        endforeach()
        # 2. The peripheral-lib deps Audio's headers reach (Wire/SD/SdFat/
        #    SerialFlash). import_evkb_library adds each lib root to teensy_flags
        #    (global) and creates its target; the linker RESCAN group resolves
        #    cross-refs regardless of order.
        import_evkb_library(Wire)
        import_evkb_library(SdFat)
        import_evkb_library(SD)
        import_evkb_library(SerialFlash)
        # 3. The whole Audio fork (root globs 87 .cpp/6 .c; utility/ adds 2).
        import_evkb_library(Audio utility)
    endif()
endmacro()
```

Note for the implementer: confirm `evkb_cmsis_dsp_cm4_sources`'s OUT_INCS list order (shim first) by reading its definition; if its shim is not first, add the shim dir explicitly before the loop. If `import_evkb_library(SerialFlash)` errors because SerialFlash's layout globs a non-compiling file, note it for Task 3 (it's part of the sweep).

- [ ] **Step 2: Scaffold the gate (so there's a consumer to drive the build)**

```bash
cd ~/Development/rt1170/evkb/examples/audio
mkdir -p audio_h_test/toolchain
cp filter_fir_test/toolchain/rt1170-evkb.toolchain.cmake audio_h_test/toolchain/
```

`examples/audio/audio_h_test/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(audio_h_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
import_evkb_audio_full()

teensy_add_executable(audio_h_test audio_h_test.cpp)
teensy_target_link_libraries(audio_h_test cores Audio Wire SdFat SD SerialFlash CMSIS-DSP)
target_link_libraries(audio_h_test.elf stdc++ m)
```

`examples/audio/audio_h_test/audio_h_test.cpp` (discovery stub — just prove the header aggregates compile; the real chain lands in Task 4):

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include <Audio.h>

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    Serial1.println("AUDIOH-GATE v1");
    Serial1.println("AUDIOH-DONE");
}
void loop() {}
```

- [ ] **Step 3: Run the first full compile — capture EVERY error**

```bash
cd ~/Development/rt1170/evkb/examples/audio/audio_h_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake 2>&1 | tee /tmp/audioh_cfg.log
# -k keeps building after errors so we inventory ALL failing files, not just the first:
cmake --build build -- -k 2>&1 | tee /tmp/audioh_build.log
grep -E "error:|fatal error:" /tmp/audioh_build.log | sed 's/:[0-9]*:[0-9]*:/:/' | sort | uniq -c | sort -rn | tee /tmp/audioh_errors.txt
echo "=== failing files ==="; grep -oE "Audio/[a-zA-Z0-9_]+\.(cpp|c)" /tmp/audioh_build.log | sort -u
```

Expected: configure succeeds (the helper wires the deps); the build produces a set of compile errors. Capture `/tmp/audioh_errors.txt` (categorized error list) and the failing-files list — these ARE this task's deliverable.

- [ ] **Step 4: Categorize the inventory**

Write `examples/audio/audio_h_test/COMPILE_SWEEP.md` — a checklist grouping every error from Step 3 by category, with the file and the fix approach. Use these categories (from the spec §2):
- **Stale/external include** (e.g. `X.h: No such file`): resolve via manifest dep already added, or strip if unused (NOTE-comment).
- **Kinetis/1062-only ref outside a guard** (unknown type/macro in an empty-on-1176 node): guard the offending include/ref so the node compiles to an empty object.
- **Header collision** (redefinition/ambiguity from 81 headers in one TU): minimal fix (shim/rename/guard).
- **Other**: describe.

Each line: `- [ ] <file>:<sym> — <category> — <planned fix>`. This file drives Task 3 and is the honest record of the sweep's true size.

- [ ] **Step 5: Commit the helper + scaffold + inventory (still RED)**

```bash
cd ~/Development/rt1170/evkb
git add evkb.cmake examples/audio/audio_h_test/CMakeLists.txt examples/audio/audio_h_test/audio_h_test.cpp examples/audio/audio_h_test/toolchain examples/audio/audio_h_test/COMPILE_SWEEP.md
git commit -m "wip: import_evkb_audio_full() + audio_h_test scaffold + full-build error inventory (red)"
```

End the commit body with the Co-Authored-By trailer. Report the inventory (categories + counts) — the coordinator uses it to scope Task 3.

---

### Task 3: Compile sweep — make all 87 `.cpp` build clean (green)

Work `COMPILE_SWEEP.md` to zero errors, in the Audio fork, using the fix patterns below. Re-run the Step-3 build after each batch; check off items. The task is done when `cmake --build build` (no `-k`) succeeds with zero errors.

**Files:** Modify Audio fork nodes as the inventory dictates; update `examples/audio/audio_h_test/COMPILE_SWEEP.md` (check off).

**Fix patterns (worked examples — apply the matching one per inventory item):**

- [ ] **Pattern A — stale/unused external include** (the `SerialFlash.h`-in-`synth_wavetable.cpp` precedent). If a `.cpp`/`.h` includes a header it doesn't actually use, strip it with the fork's NOTE convention:

```c
// NOTE: upstream includes <Foo.h> here, but this file uses no Foo symbol --
// stripped so the node doesn't require the Foo library.
```

If it DOES use the symbol and the dep is a manifest lib (Wire/SD/SerialFlash), the include resolves once `import_evkb_audio_full()` put that lib on the path — no edit needed; the error means the dep wasn't on the path (fix the helper, not the node).

- [ ] **Pattern B — Kinetis/1062-only ref outside the guard.** A node whose body is `#if defined(KINETISK)…`/`__IMXRT1062__` guarded but whose FILE-LEVEL includes or type refs sit outside the guard and use Kinetis/1062 types absent on 1176. Move the offending include/ref inside the guard, or widen the guard to cover it, so the file compiles to an empty object on 1176. Example shape:

```c
// before: unconditional include of a Kinetis-only header at file scope
// after:
#if defined(KINETISK) || defined(__IMXRT1062__)
#include "kinetis_only_thing.h"
... node body ...
#endif   // whole node empty on RT1176
```

Do NOT add RT1176 functionality to these nodes — the goal is "compiles empty, uninstantiable," not "ported."

- [ ] **Pattern C — header collision** (redefinition/ambiguity when 81 headers meet in one TU). Prefer the least-invasive fix: a missing include guard, a `#undef` before a re-`#define`, or (if it mirrors the CMSIS `A0`-`A2` clash) the same push_macro/undef shim technique ([[rt1176-cmsis-dsp-arm-math]]). Document the collision + fix in `COMPILE_SWEEP.md`.

- [ ] **Step: iterate to green**

```bash
cd ~/Development/rt1170/evkb/examples/audio/audio_h_test
cmake --build build -- -k 2>&1 | grep -E "error:|fatal error:" | sort -u   # re-inventory after each batch
# ... fix next batch in ~/Development/Audio ...
```

Repeat until:

```bash
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build 2>&1 | tail -3
```

Expected: clean build, `audio_h_test.elf` produced. Report the ELF size (`/Applications/ARM_10/bin/arm-none-eabi-size build/audio_h_test.elf`) — `--gc-sections` should keep it bounded despite the whole lib being available; a large size means an empty node dragged const data (a finding).

- [ ] **Constraint check — no ported-node regressions.** Any node the sweep touched that is ALSO exercised by an existing gate must not change behavior. After green, before committing, re-run the QEMU gates whose nodes you edited (at minimum `filter_fir_test`, `guard_sweep_test`, `audiostream_test` if their nodes were touched):

```bash
for g in filter_fir_test guard_sweep_test audiostream_test; do
  (cd ~/Development/rt1170/evkb/examples/audio/$g && cmake --build build >/dev/null 2>&1 && ./run_qemu*.sh 2>&1 | tail -1)
done
```

Expected: each prints its PASS line. A regression here means a sweep edit changed a ported node — fix before committing.

- [ ] **Step: Commit the sweep** (Audio fork + the checked-off inventory)

```bash
cd ~/Development/Audio
git add -A
git commit -m "fix: every node compiles for RT1176 — full Audio library builds (Audio.h aggregate)"
cd ~/Development/rt1170/evkb
git add examples/audio/audio_h_test/COMPILE_SWEEP.md
git commit -m "docs: audio_h_test compile-sweep inventory (all resolved)"
```

Both commit bodies end with the Co-Authored-By trailer. Do not push yet (Task 7 pushes + bumps the pin).

---

### Task 4: `audio_h_test` gate — the known-answer chain (QEMU green)

Replace the discovery stub with the modest multi-family chain and assertions.

**Files:** Modify `examples/audio/audio_h_test/audio_h_test.cpp`; Create `examples/audio/audio_h_test/run_qemu.sh` (755).

- [ ] **Step 1: Write the gate source**

Chain: `synth_sine → filter_fir (boxcar low-pass) → mixer → analyze_fft256 + analyze_peak`. Reuses the HW-verified boxcar known-answer (bin 6 at 1033.59375 Hz, from filter_fir_test) but instantiated purely through `<Audio.h>` types, plus a `mixer` stage to exercise a third family. `examples/audio/audio_h_test/audio_h_test.cpp`:

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include <Audio.h>
#include <math.h>

// Proves the FULL Audio library aggregates: every type below comes only from
// <Audio.h>. Chain: sine -> FIR low-pass -> mixer -> FFT256 (+peak tap).
static AudioSynthWaveformSine sine1;
static AudioFilterFIR         fir1;
static AudioMixer4            mix1;
static AudioAnalyzeFFT256     fft1;
static AudioAnalyzePeak       peak1;
static AudioConnection c1(sine1, 0, fir1, 0);
static AudioConnection c2(fir1, 0, mix1, 0);
static AudioConnection c3(mix1, 0, fft1, 0);
static AudioConnection c4(mix1, 0, peak1, 0);

// No I/O node in this graph -> nothing calls the protected update_setup() that
// arms IRQ_SOFTWARE. Same GraphClock pattern as filter_fir_test/audiostream_test.
struct GraphClock : public AudioStream {
    GraphClock() : AudioStream(0, NULL) { update_setup(); }
    void update(void) override {}
};
static GraphClock clock1;

static const short lp_coeffs[8] = {4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096};

static void pump(int n) {
    for (int i = 0; i < n; i++) { NVIC_SET_PENDING(IRQ_SOFTWARE); delayMicroseconds(200); }
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    Serial1.println("AUDIOH-GATE v1");
    AudioMemory(30);
    sine1.amplitude(0.9f);
    sine1.frequency(1033.59375f);   // exactly bin 6 (6*44100/256)
    mix1.gain(0, 1.0f);
    fir1.begin(lp_coeffs, 8);

    pump(48);
    (void)fft1.available();
    float pb = -1.0f;
    for (int i = 0; i < 400; i++) { pump(2); if (fft1.available()) { pb = fft1.read(6); break; } }
    float pk = peak1.available() ? peak1.read() : -1.0f;

    Serial1.print("AUDIOH: fft_bin6="); Serial1.println(pb, 4);
    Serial1.print("AUDIOH: peak=");     Serial1.println(pk, 4);
    bool pass = (pb > 0.30f && pb < 0.55f) && (pk > 0.7f && pk < 0.95f);
    Serial1.println(pass ? "AUDIOH_CHAIN=PASS" : "AUDIOH_CHAIN=FAIL");
    Serial1.println("AUDIOH-DONE");
}
void loop() {}
```

(Thresholds mirror filter_fir_test's HW-verified boxcar: passband read ≈ 0.42, peak ≈ 0.85. If `AudioMixer4`'s type name differs in this fork, grep `mixer.h` for the class name and match it.)

- [ ] **Step 2: Write `run_qemu.sh`** (poll-loop, then `chmod +x`)

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/audio_h_test.elf"; OUT="$DIR/audio_h.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/audio_h.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do [ -f "$OUT" ] && grep -q "AUDIOH-DONE" "$OUT" 2>/dev/null && break; sleep 0.25; done
kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "AUDIOH-GATE v1"  "$OUT" || { echo "FAIL: banner"; exit 1; }
grep -q "AUDIOH_CHAIN=PASS" "$OUT" || { echo "FAIL: chain"; exit 1; }
echo "PASS: AUDIOH_CHAIN"
```

- [ ] **Step 3: Build + run**

```bash
cd ~/Development/rt1170/evkb/examples/audio/audio_h_test
cmake --build build && ./run_qemu.sh
```

Expected: `AUDIOH: fft_bin6=` ≈ 0.42, `peak=` ≈ 0.85, `PASS: AUDIOH_CHAIN`. If `fft_bin6=-1.0000`, the graph produced no FFT output — check `AudioMemory(30)` and the `GraphClock` (compare with filter_fir_test). If a type name doesn't resolve, that's a Task-3 miss (a header didn't actually compile) — go back and fix.

- [ ] **Step 4: Save transcript + commit**

```bash
cp audio_h.uart transcript_qemu.txt
cd ~/Development/rt1170/evkb
git add examples/audio/audio_h_test
git commit -m "test: audio_h_test — #include <Audio.h> full-library chain, QEMU green"
```

Co-Authored-By trailer.

---

### Task 5: Hardware verification

- [ ] **Step 1: Flash + capture** (board rules: pkill daemons; reader first; never `cat`)

```bash
pkill LinkServer; pkill redlinkserv; sleep 1
cd ~/Development/rt1170/evkb/examples/audio/audio_h_test
python3 ~/Development/rt1170/evkb/tools/rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > transcript_hw_evkb.txt 2>&1 &
sleep 1
/Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/audio_h_test.elf
sleep 8
pkill -f rt1170-console.py; pkill LinkServer; pkill redlinkserv
```

- [ ] **Step 2: Verify**

```bash
grep "AUDIOH_CHAIN=PASS" transcript_hw_evkb.txt && grep "AUDIOH: fft_bin6=" transcript_hw_evkb.txt
```

Expected: PASS, with `fft_bin6`/`peak` numerically identical to QEMU (integer-derived q15 chain). A divergence is a real finding — report both values, no threshold changes.

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/audio/audio_h_test/transcript_hw_evkb.txt
git commit -m "test: audio_h_test HW-verified on EVKB (transcript)"
```

Co-Authored-By trailer.

---

### Task 6: Regressions + license audit

- [ ] **Step 1: Full regression sweep** (the compile-sweep edited the Audio fork; every Audio gate must stay green). Rebuild each from scratch if stale:

```bash
cd ~/Development/rt1170/evkb
for g in examples/audio/filter_fir_test examples/audio/guard_sweep_test examples/audio/audiostream_test examples/audio/sd_wav_play_test examples/audio/i2s_int_test examples/dualcore/cm4_fft_test examples/dualcore/cm4_audio_test; do
  echo "== $g =="; (cd "$g" && cmake --build build >/dev/null 2>&1; ls run_qemu*.sh | head -1 | xargs -I{} sh {} 2>&1 | tail -1)
done
```

Expected: each prints its PASS line (FIR_ALL, GUARD_SWEEP_ALL, AUDIOSTREAM_ALL, the sd_wav PASS, I2SINT=PASS, FFT_CM4=PASS, the audio-DET PASS). Any FAIL = a sweep edit regressed a ported node — root-cause and fix before proceeding.

- [ ] **Step 2: License audit** (now covers EVERY Audio file via the full-build depfiles + SerialFlash)

```bash
~/Development/rt1170/evkb/tools/license-audit.sh 2>&1 | tail -4
```

Expected: `LICENSE-AUDIT: PASS`. Add `audio_h_test` to the GATES list in `tools/license-audit.sh` first (so its depfiles — the whole Audio lib — are walked): append `examples/audio/audio_h_test:audio_h_test`. If any newly-compiled Audio file trips the copyleft sweep, inspect it verbatim (do not blanket-allowlist) — a non-permissive file compiled into firmware is a stop-the-line finding.

- [ ] **Step 3: Commit the audit change**

```bash
cd ~/Development/rt1170/evkb
git add tools/license-audit.sh
git commit -m "chore: license-audit walks audio_h_test (full Audio lib) + SerialFlash"
```

Co-Authored-By trailer.

---

### Task 7: Push, pin bump, FORCE_FETCH proof, docs

- [ ] **Step 1: Docs — Audio status doc** (`~/Development/Audio/docs/rt1170-evkb-status.md`):
  - `Audio.h (master include)` row: 🟡 → ✅, note `full library builds for RT1176; HW-verified via evkb audio_h_test`.
  - Legend: no 🟡 component rows remain — reword the 🟡 line (or note Phase A complete).
  - The "🟠 hardware I/O nodes … compile empty" note: add that they now compile clean (empty objects) as part of the full build, but remain uninstantiable (no RT1176 port) — using one is a link error, not a compile error.
  - Changelog blockquote (2026-07-22): the full Audio library builds; `#include <Audio.h>` works; **Phase A COMPLETE**.
  - Commit Audio: `docs: Audio.h builds fully on RT1176 — Phase A complete`.

- [ ] **Step 2: Push Audio, bump the pin**

```bash
cd ~/Development/Audio && git push && git rev-parse HEAD
```

In `evkb.cmake`, set the `_evkb_lib(Audio …)` pin to the new pushed HEAD (full 40 chars, replacing `f5e47306…`).

- [ ] **Step 3: FORCE_FETCH proof** (fresh-user: fetches Audio + SerialFlash + CMSIS at the pins and builds the whole lib)

```bash
cd ~/Development/rt1170/evkb/examples/audio/audio_h_test
cmake -B build-ff -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake -DEVKB_FORCE_FETCH=ON 2>&1 | grep -iE "fetching (Audio|SerialFlash|CMSIS)" 
cmake --build build-ff 2>&1 | tail -3
~/Development/rt1170/evkb/tools/qrun -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on \
  -kernel build-ff/audio_h_test.elf -display none -serial file:ff.uart -d guest_errors -D ff.dbg &
P=$!; for _ in $(seq 1 40); do grep -q AUDIOH-DONE ff.uart 2>/dev/null && break; sleep 0.25; done; kill $P 2>/dev/null; wait $P 2>/dev/null
grep "AUDIOH_CHAIN=PASS" ff.uart && rm -rf build-ff ff.uart ff.dbg
```

Expected: configure logs fetching Audio @ the new SHA + SerialFlash @ `2b6f241` + CMSIS; the whole lib builds; QEMU boots green. The grep must hit before cleanup.

- [ ] **Step 4: Commit the pin bump**

```bash
cd ~/Development/rt1170/evkb
git add evkb.cmake
git commit -m "chore: bump Audio pin — full RT1176 library build (Audio.h, Phase A complete)"
```

Co-Authored-By trailer.

- [ ] **Step 5: evkb docs + push**
  - `examples/README.md`: add `audio_h_test` to the audio row.
  - The Audio status doc (Step 1) is the canonical Phase-A record; the `cm4-roadmap.md` is CM4-scoped — leave it. (Memory-note updates are the coordinator's job, not this task.)
  - Commit `docs: examples index gains audio_h_test`, then `git push` evkb.
  - Report `git status -sb` for evkb + Audio + SerialFlash (no ahead markers).

---

## Verification (whole plan)

1. `import_evkb_audio_full()` compiles all 87 Audio `.cpp` clean (Task 3 green; `COMPILE_SWEEP.md` all checked; ELF size reported).
2. `audio_h_test`: `#include <Audio.h>` chain green in QEMU AND HW-verified (transcripts committed, matching).
3. All prior Audio gates + CM4 audio gates still green (Task 6).
4. `license-audit.sh` PASS, now walking the entire Audio library + SerialFlash.
5. Audio + SerialFlash pushed; evkb.cmake pins == pushed heads; `-DEVKB_FORCE_FETCH=ON` builds the whole lib green.
6. Status doc: `Audio.h` ✅, Phase A COMPLETE, no 🟡 component rows remain.

## Notes on the discovery structure

Tasks 2-3 are deliberately discover-then-fix: the exact node edits are unknown until the first full compile (spec §7), so Task 2 produces `COMPILE_SWEEP.md` (the real inventory) and Task 3 works it with the three documented fix patterns. The coordinator should read Task 2's reported inventory before dispatching Task 3, and may split Task 3 into batches if the count is large.
