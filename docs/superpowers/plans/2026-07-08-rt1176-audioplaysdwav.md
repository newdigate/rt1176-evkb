# RT1176 AudioPlaySdWav (SD → J101 capstone) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Play a 16-bit/44.1 kHz WAV off the SD card through the AudioStream graph out the J101 headphone jack, by bringing up Teensy Audio's `AudioPlaySdWav` node on the RT1176 core.

**Architecture:** The node ports **verbatim** (platform-generic; relies only on the already-done `AudioStream` + `SD.h`). The only code change is a one-line core macro (`NVIC_IS_ENABLED`) the node needs to compile. A QEMU gate wires `AudioPlaySdWav → AudioOutputI2S` (+ WM8962), plays a generated WAV off a FAT card image, and asserts the captured SAI1-TX samples equal the WAV **sample-for-sample** (mono + distinct-L/R stereo). Hardware = audible playback on J101.

**Tech Stack:** C++ (Arduino/Teensyduino audio graph), Python (register generator; WAV gen + tap comparator), CMake + `arm-none-eabi-g++`, QEMU `mimxrt1170-evk` (`qrun`) with `-drive if=sd` + `-chardev id=sai1-tap`, `hdiutil`/`diskutil` (MBR/FAT16 card image), LinkServer + VCOM (hardware).

**Spec:** `docs/superpowers/specs/2026-07-08-rt1176-audioplaysdwav-design.md`

**Repos + git roots (commit to `master`/active branch; push only when the user asks):**
- **core** (`teensy-cores`): `~/Development/rt1170/evkb/cores/imxrt1176`, **git root `~/Development/rt1170/evkb/cores`** → `git -C ~/Development/rt1170/evkb/cores …`
- **gate** lives in **evkb** (local repo): `~/Development/rt1170/evkb/sd_wav_play_test/` → `git -C ~/Development/rt1170/evkb …`
- **`~/Development/Audio`** (`newdigate/Audio`): built as-is; **not modified** (except an optional cosmetic comment fix, Task 2).

**Prereqs already done + HW-verified:** the SD stack ([[rt1176-sd-usdhc]]), AudioStream + AudioOutputI2S ([[rt1176-audiostream]], [[rt1176-audiooutput-i2s]]), WM8962. The node needs no changes beyond the core macro; Task 2 is the first compile/link proof, Task 3 the behavioral gate.

---

## Task 1: Core — add `NVIC_IS_ENABLED`

`play_sd_wav.cpp` calls `NVIC_IS_ENABLED(IRQ_SOFTWARE)` in `play()`/`stop()`; the core defines `NVIC_ENABLE_IRQ`/`NVIC_DISABLE_IRQ` but not `IS_ENABLED`. `imxrt1176.h` is auto-generated — edit the generator + regenerate.

**Files:**
- Modify: `~/Development/rt1170/evkb/cores/imxrt1176/tools/gen_imxrt1176_h.py`
- Regenerate: `~/Development/rt1170/evkb/cores/imxrt1176/imxrt1176.h`

- [ ] **Step 1: Add the macro to the generator**

In `tools/gen_imxrt1176_h.py`, find the NVIC helper block (the list that defines `NVIC_ENABLE_IRQ`):
```python
          "#define NVIC_ENABLE_IRQ(n)   (NVIC_ISER((n) >> 5) = (1u << ((n) & 31)))",
          "#define NVIC_DISABLE_IRQ(n)  (NVIC_ICER((n) >> 5) = (1u << ((n) & 31)))",
```
Insert a `NVIC_IS_ENABLED` line immediately after the `NVIC_ENABLE_IRQ` line:
```python
          "#define NVIC_ENABLE_IRQ(n)   (NVIC_ISER((n) >> 5) = (1u << ((n) & 31)))",
          "#define NVIC_IS_ENABLED(n)   (NVIC_ISER((n) >> 5) & (1u << ((n) & 31)))",
          "#define NVIC_DISABLE_IRQ(n)  (NVIC_ICER((n) >> 5) = (1u << ((n) & 31)))",
```

- [ ] **Step 2: Regenerate + verify**

Run:
```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176 && python3 tools/gen_imxrt1176_h.py
grep -n "define NVIC_IS_ENABLED" imxrt1176.h
git -C ~/Development/rt1170/evkb/cores diff --stat imxrt1176/imxrt1176.h
```
Expected: generator exits 0; the grep shows exactly one `#define NVIC_IS_ENABLED(n) (NVIC_ISER((n) >> 5) & (1u << ((n) & 31)))`; the diff touches only `imxrt1176.h` with a single added line. (Confirm `git -C ~/Development/rt1170/evkb/cores status --short` shows only the generator + header modified — the tree is shared; stage only those two.)

- [ ] **Step 3: Commit**

```bash
git -C ~/Development/rt1170/evkb/cores add imxrt1176/tools/gen_imxrt1176_h.py imxrt1176/imxrt1176.h
git -C ~/Development/rt1170/evkb/cores commit -m "$(cat <<'EOF'
imxrt1176: add NVIC_IS_ENABLED

AudioPlaySdWav's play()/stop() call NVIC_IS_ENABLED(IRQ_SOFTWARE) to save/restore
the audio software-IRQ around file open/close; the core had NVIC_ENABLE/DISABLE
but not IS_ENABLED. One-line generator addition next to the existing NVIC macros.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Gate scaffolding — firmware + CMakeLists + build (compile/link proof)

This validates that `play_sd_wav.cpp` builds + links against the RT1176 core (needs Task 1's `NVIC_IS_ENABLED` and the `SPI` link for `spi_interrupt.h`). No run yet.

**Files:**
- Create: `~/Development/rt1170/evkb/sd_wav_play_test/sd_wav_play_test.cpp`
- Create: `~/Development/rt1170/evkb/sd_wav_play_test/CMakeLists.txt`
- Create: `~/Development/rt1170/evkb/sd_wav_play_test/.gitignore`

- [ ] **Step 1: Write the firmware (`sd_wav_play_test.cpp`)**

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include <SD.h>
#include "AudioStream.h"
#include "play_sd_wav.h"
#include "output_i2s.h"
#include "control_wm8962.h"

// AudioPlaySdWav (0 in / 2 out) -> AudioOutputI2S (SAI1 TX DMA) -> WM8962 -> J101.
// The SAI1 TX DMA isr pends update_all(), which runs AudioPlaySdWav::update() ->
// the blocking 512-byte SD read -> transmit. In QEMU every SAI1_TDR0 write is
// mirrored to the sai1-tap chardev; check_tap.py asserts it equals TEST.WAV.
AudioPlaySdWav     playWav;
AudioOutputI2S     out;
AudioControlWM8962 wm;                          // I2C/Wire2 codec (HW fidelity)
AudioConnection    cL(playWav, 0, out, 0);      // left
AudioConnection    cR(playWav, 1, out, 1);      // right

void setup() {
  Serial1.begin(115200);
  while (!Serial1) {}
  AudioMemory(30);
  wm.enable();

  if (!SD.begin(BUILTIN_SDCARD)) {
    Serial1.println("SD_WAV_MOUNT=FAIL");
    Serial1.println("SD_WAV_PLAY=FAIL");
    Serial1.println("SD_WAV_DONE=FAIL");
    return;
  }
  Serial1.println("SD_WAV_MOUNT=PASS");

  bool started = playWav.play("TEST.WAV");
  Serial1.print("SD_WAV_PLAY="); Serial1.println(started ? "PASS" : "FAIL");
  if (!started) { Serial1.println("SD_WAV_DONE=FAIL"); return; }

  // Wait for playback to finish. NOTE: right after play() the node is in a
  // header-PARSE state (8-12), for which isPlaying() (state<8) is FALSE -- so
  // do NOT loop on isPlaying(). isStopped() (state==14/STATE_STOP) is only set
  // at EOF (or parse failure), so it correctly spans parse+play until done.
  uint32_t t0 = millis();
  while (!playWav.isStopped() && (millis() - t0) < 15000) {
    yield();
  }
  Serial1.print("SD_WAV_DONE=");
  Serial1.println(playWav.isStopped() ? "PASS" : "FAIL(timeout)");
  Serial1.print("info positionMillis="); Serial1.println(playWav.positionMillis());
  Serial1.print("info lengthMillis=");   Serial1.println(playWav.lengthMillis());
}
void loop() {}
```

- [ ] **Step 2: Write the CMakeLists (`CMakeLists.txt`)**

Combines the SD gates' inlined toolchain + SdFat/SD/SPI `src`-enumeration with the audio gates' Wire + cherry-picked Audio-node `target_sources`.
```cmake
cmake_minimum_required(VERSION 3.24)

# --- Toolchain bootstrap (before project()) — inlined (like SdFat/tests/*) so a
# plain `cmake -B build -S .` cross-compiles for the RT1176 M7 with no extra
# flags. Mirrors evkb/*/toolchain/rt1170-evkb.toolchain.cmake. ---
set(TEENSY_VERSION 117 CACHE STRING "RT1176 / MIMXRT1170-EVKB" FORCE)
set(CPU_CORE_SPEED 996000000 CACHE STRING "RT1176 M7 core clock" FORCE)
set(EVKB ${CMAKE_CURRENT_LIST_DIR}/..)
set(COREPATH "${EVKB}/cores/imxrt1176/" CACHE STRING "imxrt1176 core path" FORCE)
set(CMAKE_EXE_LINKER_FLAGS "--specs=nano.specs" CACHE INTERNAL "")
set(COMPILERPATH "/Applications/ARM_10/bin/")
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")
set(CMAKE_C_COMPILER ${COMPILERPATH}arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${COMPILERPATH}arm-none-eabi-g++)
set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_C_COMPILER} <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
# -----------------------------------------------------------------------------

project(sd_wav_play_test)

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${EVKB}/teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${EVKB}/cores/imxrt1176)
import_arduino_library(Wire  $ENV{HOME}/Development/Wire)
import_arduino_library(SPI   $ENV{HOME}/Development/SPI)
# SdFat is a 1.5-format lib: enumerate every src subdir that carries sources (src
# first for the include root). src/SpiDriver is required because SDClass::begin()
# references the SPI-SD path (runtime if, not strippable), even though the gate
# uses only SDIO. (Same as SdFat/tests/sd_fs_test.)
import_arduino_library(SdFat $ENV{HOME}/Development/SdFat
    src src/common src/SdCard src/FatLib src/ExFatLib src/FsLib
    src/iostream src/DigitalIO src/SpiDriver)
import_arduino_library(SD    $ENV{HOME}/Development/PaulS_SD src)

teensy_add_executable(sd_wav_play_test sd_wav_play_test.cpp)
teensy_target_link_libraries(sd_wav_play_test cores Wire SPI SdFat SD)

# Audio nodes are cherry-picked (the evkb gates do NOT glob the Audio fork).
# memcpy_audio.S needs the LANGUAGE C + assembler-with-cpp trick (as in
# audiooutput_i2s_test) since it's added directly, not via import_arduino_library.
target_sources(sd_wav_play_test.elf PRIVATE
    sd_wav_play_test.cpp
    $ENV{HOME}/Development/Audio/play_sd_wav.cpp
    $ENV{HOME}/Development/Audio/output_i2s.cpp
    $ENV{HOME}/Development/Audio/control_wm8962.cpp
    $ENV{HOME}/Development/Audio/memcpy_audio.S
)
set_source_files_properties($ENV{HOME}/Development/Audio/memcpy_audio.S PROPERTIES
    LANGUAGE C
    COMPILE_OPTIONS "-x;assembler-with-cpp"
)
target_include_directories(sd_wav_play_test.elf PRIVATE $ENV{HOME}/Development/Audio)
target_link_libraries(sd_wav_play_test.elf stdc++)
target_link_libraries(sd_wav_play_test.elf m)
```

- [ ] **Step 3: Write `.gitignore`**

```
build/
*.img
*.raw
*.uart
*.dbg
*.wav
```

- [ ] **Step 4: Build**

Run:
```bash
cd ~/Development/rt1170/evkb/sd_wav_play_test
rm -rf build && cmake -B build -S . >/tmp/wav_cfg.log 2>&1 && cmake --build build 2>&1 | tail -25
ls -la build/sd_wav_play_test.elf
```
Expected: `build/sd_wav_play_test.elf` is produced. This proves `play_sd_wav.cpp` compiles (Task 1's `NVIC_IS_ENABLED` resolves) and links (`AudioStartUsingSPI` → `SPI` resolves). If it fails: a `NVIC_IS_ENABLED` error ⇒ Task 1 not applied; an `AudioStartUsingSPI`/`SPI`/`usingInterrupt` undefined ⇒ SPI not linked (check the `SPI` import); an SdFat `<SdFat.h>` not-found or empty-lib ⇒ the `src`-subdir enumeration is wrong. If configure fails, read `/tmp/wav_cfg.log` (toolchain/COREPATH).

- [ ] **Step 5 (optional cosmetic): fix the stale comment in the Audio fork**

`~/Development/Audio/control_wm8962.cpp:6` says `enable()` "reuses the core's HW-verified `WM8962Codec::begin()` (see `cores/imxrt1176/wm8962.{h,cpp}`)" — that core file was deleted in the 2026-07-07 consolidation. If fixing, edit the comment to note the driver now lives in `control_wm8962.cpp` itself, and commit to the Audio repo (`git -C ~/Development/Audio add src/control_wm8962.cpp && git -C ~/Development/Audio commit -m "control_wm8962: fix stale comment referencing the deleted core wm8962.{h,cpp}"`). Skip if minimizing churn — it's non-functional.

- [ ] **Step 6: Commit the gate scaffolding (evkb repo)**

```bash
git -C ~/Development/rt1170/evkb add sd_wav_play_test/sd_wav_play_test.cpp sd_wav_play_test/CMakeLists.txt sd_wav_play_test/.gitignore
git -C ~/Development/rt1170/evkb commit -m "$(cat <<'EOF'
sd_wav_play_test: gate scaffolding (AudioPlaySdWav -> AudioOutputI2S)

Firmware graph playWav -> out (+WM8962), SD.begin + play("TEST.WAV"), waits on
isStopped() (NOT isPlaying() -- false during header parse). CMakeLists unions the
SD gates' inlined toolchain + SdFat/SD/SPI src-enumeration with the audio gates'
Wire + cherry-picked Audio-node target_sources. Compile/link proof; run in Task 3.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: WAV gen + card seeding + sample-exact gate (behavioral red→green)

Generate deterministic mono + distinct-L/R stereo WAVs, seed each onto an MBR/FAT16 card image, play in QEMU with the SAI1 tap, and assert the tap equals the WAV sample-for-sample.

**Files:**
- Create: `~/Development/rt1170/evkb/sd_wav_play_test/gen_wav.py`
- Create: `~/Development/rt1170/evkb/sd_wav_play_test/check_tap.py`
- Create: `~/Development/rt1170/evkb/sd_wav_play_test/run_qemu_sd_wav.sh`

- [ ] **Step 1: Write `gen_wav.py`**

```python
#!/usr/bin/env python3
# Generate a deterministic canonical 16-bit PCM 44.1 kHz WAV for the gate.
#   gen_wav.py mono   out.wav   -> mono, 440 Hz cosine
#   gen_wav.py stereo out.wav   -> stereo, L=440 Hz R=880 Hz (distinct -> a
#                                  channel swap fails the sample-exact check)
# cosine starts at +AMP (non-zero) so check_tap.py can unambiguously find the
# audio start against leading silence. python `wave` emits WAVE_FORMAT_PCM (tag
# 1, not EXTENSIBLE), exactly what AudioPlaySdWav requires.
import sys, math, wave, struct
kind, out = sys.argv[1], sys.argv[2]
RATE = 44100
N = int(RATE * 0.20)          # 200 ms -> ~8820 frames (short, fully captured)
AMP = 12000                   # safely inside int16, non-trivial amplitude
def cosine(freq):
    return [int(AMP * math.cos(2*math.pi*freq*i/RATE)) for i in range(N)]
w = wave.open(out, "wb")
w.setsampwidth(2); w.setframerate(RATE)
if kind == "mono":
    w.setnchannels(1)
    w.writeframes(b"".join(struct.pack("<h", s) for s in cosine(440)))
else:
    w.setnchannels(2)
    L, R = cosine(440), cosine(880)
    w.writeframes(b"".join(struct.pack("<hh", l, r) for l, r in zip(L, R)))
w.close()
print("wrote %s (%s, %d frames)" % (out, kind, N))
```

- [ ] **Step 2: Write `check_tap.py` (the sample-exact comparator)**

```python
#!/usr/bin/env python3
# Sample-exact compare of the SAI1 TX tap against the source WAV.
#   check_tap.py <tap.raw> <src.wav> <mono|stereo>
# tap.raw = interleaved LE int16 (SAI1_TDR0 writes: L,R,L,R,...). AudioOutputI2S
# always emits stereo frames; a mono source is duplicated to both channels by
# AudioPlaySdWav, so tap L==R==wav. The played audio appears after leading
# boot/header-parse silence + a fixed pipeline latency -> find the offset (first
# frame that begins a K-run exactly matching the WAV start), then assert the
# whole WAV matches. No scaling exists in the play->TDR0 path, so the match is
# exact.
import sys, struct, wave
tap_path, wav_path, kind = sys.argv[1], sys.argv[2], sys.argv[3]

w = wave.open(wav_path, "rb")
ch, sw, nf = w.getnchannels(), w.getsampwidth(), w.getnframes()
assert sw == 2, "WAV must be 16-bit"
raw = w.readframes(nf)
if ch == 1:
    m = list(struct.unpack("<%dh" % nf, raw)); wavL, wavR = m, m
else:
    inter = struct.unpack("<%dh" % (nf*2), raw)
    wavL, wavR = list(inter[0::2]), list(inter[1::2])

tb = open(tap_path, "rb").read()
tn = len(tb)//2
tap = struct.unpack("<%dh" % tn, tb[:tn*2])
tapL, tapR = tap[0::2], tap[1::2]

K = 64
def run_ok(off):
    if off + len(wavL) > len(tapL): return False
    return all(tapL[off+i] == wavL[i] and tapR[off+i] == wavR[i]
               for i in range(min(K, len(wavL))))
offset = -1
for f in range(len(tapL) - K):
    if tapL[f] == wavL[0] and tapR[f] == wavR[0] and run_ok(f):
        offset = f; break
if offset < 0:
    print("WAV_SAMPLES_EXACT=FAIL (WAV start not found in tap; tap frames=%d)" % len(tapL))
    sys.exit(1)

mism = sum(1 for i in range(len(wavL))
           if tapL[offset+i] != wavL[i] or tapR[offset+i] != wavR[i])
print("info %s tap_offset=%d frames=%d mismatches=%d" % (kind, offset, len(wavL), mism))
print("WAV_SAMPLES_EXACT=PASS" if mism == 0 else "WAV_SAMPLES_EXACT=FAIL")
sys.exit(0 if mism == 0 else 1)
```

- [ ] **Step 3: Write the runner `run_qemu_sd_wav.sh` (chmod +x)**

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/sd_wav_play_test.elf"
[ -f "$ELF" ] || { echo "FAIL: no ELF ($ELF) — build first"; exit 1; }

run_one() {   # $1 = mono|stereo
    KIND="$1"
    WAV="$DIR/$KIND.wav"; IMG="$DIR/card_$KIND.img"
    VCOM="$DIR/$KIND.uart"; TAP="$DIR/$KIND.raw"; DBG="$DIR/$KIND.dbg"
    rm -f "$WAV" "$IMG" "$VCOM" "$TAP" "$DBG"
    python3 "$DIR/gen_wav.py" "$KIND" "$WAV"
    # MBR/FAT16 card image with TEST.WAV seeded (the sd_fs_test recipe + a copy).
    mkfile -n 512m "$IMG"
    DISK=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$IMG" | head -1 | awk '{print $1}')
    [ -n "$DISK" ] || { echo "FAIL($KIND): attach"; return 1; }
    diskutil partitionDisk "$DISK" 1 MBR "MS-DOS FAT16" RTWAV 100% >/dev/null \
        || { hdiutil detach "$DISK" >/dev/null 2>&1 || true; echo "FAIL($KIND): partition"; return 1; }
    MNT=$(diskutil info "${DISK}s1" | sed -n 's/.*Mount Point: *//p')
    cp "$WAV" "$MNT/TEST.WAV"; sync
    diskutil eject "$DISK" >/dev/null
    # run: SD image (USDHC1) + SAI1 TX tap
    "$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
        -display none -serial file:"$VCOM" \
        -drive if=sd,format=raw,file="$IMG" \
        -chardev file,id=sai1-tap,path="$TAP" \
        -d guest_errors,unimp -D "$DBG" &
    P=$!; gate_pid $P; sleep 8; kill $P 2>/dev/null; wait $P 2>/dev/null || true
    echo "==== $KIND VCOM ===="; cat "$VCOM" 2>/dev/null || true
    grep -q "SD_WAV_MOUNT=PASS" "$VCOM" || { echo "FAIL($KIND): mount"; return 1; }
    grep -q "SD_WAV_PLAY=PASS"  "$VCOM" || { echo "FAIL($KIND): play";  return 1; }
    grep -q "SD_WAV_DONE=PASS"  "$VCOM" || { echo "FAIL($KIND): done";  return 1; }
    echo "==== $KIND TAP (sample-exact) ===="
    python3 "$DIR/check_tap.py" "$TAP" "$WAV" "$KIND" || { echo "FAIL($KIND): sample-exact"; return 1; }
}

run_one mono   || exit 1
run_one stereo || exit 1
echo "SD_WAV_ALL=PASS"
```
Then: `chmod +x ~/Development/rt1170/evkb/sd_wav_play_test/run_qemu_sd_wav.sh`

- [ ] **Step 4: Run the gate**

Run: `~/Development/rt1170/evkb/sd_wav_play_test/run_qemu_sd_wav.sh`
Expected final line: `SD_WAV_ALL=PASS`, with each kind printing `SD_WAV_MOUNT/PLAY/DONE=PASS` and `WAV_SAMPLES_EXACT=PASS` (`mismatches=0`).

Debugging (use systematic-debugging):
- `SD_WAV_MOUNT=FAIL` → card image didn't mount (check the `diskutil partitionDisk`/`cp`/`eject` steps produced a FAT with `TEST.WAV`; SdFat needs MBR partition 1).
- `SD_WAV_PLAY=FAIL` → `play()` returned false (open failed — filename must be `TEST.WAV`) or the WAV header was rejected (must be canonical 16-bit PCM 44.1k; `gen_wav.py` produces that).
- `SD_WAV_DONE=FAIL(timeout)` → playback didn't reach EOF in 15 s of `millis()` — likely the graph isn't self-clocking; check `<KIND>.dbg` for `LOG_UNIMP` and confirm `AudioOutputI2S` began the SAI1 TX DMA.
- `WAV_SAMPLES_EXACT=FAIL (WAV start not found)` → the audio never reached the tap (graph/SAI issue) — inspect whether the tap is all-zero.
- `WAV_SAMPLES_EXACT=FAIL` with a nonzero `mismatches` **for stereo only** → a channel swap or deinterleave/off-by-one in `consume()` (the fragile L/R carry) — the exact point of the gate. If mismatches cluster only at the very start/end, it's an alignment artifact — widen `K` or inspect the offset, don't assume a firmware bug.

- [ ] **Step 5: Commit (evkb repo)**

```bash
git -C ~/Development/rt1170/evkb add sd_wav_play_test/gen_wav.py sd_wav_play_test/check_tap.py sd_wav_play_test/run_qemu_sd_wav.sh
git -C ~/Development/rt1170/evkb commit -m "$(cat <<'EOF'
sd_wav_play_test: sample-exact SAI1-tap gate (mono + stereo)

gen_wav.py makes canonical 16-bit/44.1k WAVs (mono 440 Hz; stereo L=440/R=880 so
a channel swap fails). Runner seeds each as TEST.WAV on an MBR/FAT16 card image,
runs QEMU with -drive if=sd + -chardev sai1-tap, and check_tap.py asserts the
captured SAI1_TDR0 samples equal the WAV sample-for-sample. SD_WAV_ALL=PASS.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Hardware verification (the capstone payoff)

Play a real WAV out J101 on the EVKB. Controller drives flash + VCOM; the user inserts a card and **listens**.

- [ ] **Step 1: Put a WAV on a µSD**

Generate a longer, recognizable 16-bit/44.1k WAV (a few seconds — a tone sweep or a short clip): e.g. `python3 sd_wav_play_test/gen_wav.py stereo /tmp/test.wav` (short) or `ffmpeg -i <source> -ar 44100 -ac 2 -sample_fmt s16 -f wav TEST.WAV` for real audio. Copy it as **`TEST.WAV`** (uppercase 8.3) onto a **FAT32/FAT16-formatted** µSD, and insert it into J15. (Ask the user to do the copy + insert.)

- [ ] **Step 2: Flash + capture**

```bash
cd ~/Development/rt1170/evkb/sd_wav_play_test && cmake --build build
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load "$(pwd)/build/sd_wav_play_test.elf"
```
Capture VCOM (`/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200, pyserial + gtimeout per [[macos-serial-capture]]) around the flash. Expected: `SD_WAV_MOUNT=PASS`, `SD_WAV_PLAY=PASS`, `SD_WAV_DONE=PASS`, and `info lengthMillis=` ≈ the WAV duration.

- [ ] **Step 3: The listen test (final arbiter)**

The user confirms **audible playback out the J101 headphone jack** — the whole SD + audio effort's payoff. If markers pass but there's no/garbled sound: check the WM8962 volume/route (same path as the HW-verified `audiooutput_i2s_test` tone), confirm the WAV is 16-bit/44.1k PCM (not 48k/24-bit/Extensible), and recall the SD-read-in-ISR timing caveat (a longer/fragmented file could glitch — the QEMU gate can't prove HW timing margins). A short clean WAV should play cleanly.

- [ ] **Step 4: Record the outcome**

Update the spec status and the memory note ([[rt1176-sd-usdhc]] links here) with the HW result. Note any divergence found. Do not push unless the user asks.

---

## Notes for the implementer

- **YAGNI already applied:** no 8-bit/non-44.1k/resampling (node stubs those), no Extensible WAV, no MP3, no pause/seek gate coverage. The node is built **verbatim** — no `__IMXRT1176__` branch.
- **Commit boundaries:** core → `git -C ~/Development/rt1170/evkb/cores`; gate → `git -C ~/Development/rt1170/evkb`; the optional comment fix → `git -C ~/Development/Audio`. The evkb tree is shared across sessions — `git add` only the named files, never `-A`.
- **qemu2 is unchanged** — the SAI1 tap + SD attach already exist; `-d unimp` will surface any surprise, but none is expected.
- **The stereo sample-exact check is the point** — it catches the `consume()` L/R deinterleave/carry bugs. Do not simplify the ported node.
