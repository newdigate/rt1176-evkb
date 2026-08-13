# RT1062 Capstone (Phase 5b) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** USB audio IN → AudioStream graph → WM8960 line out on the
MIMXRT1060-EVKB, both legs in one graph, proven by a loopback-cable round trip
you can hear.

**Architecture:** One new Audio node (`AudioInputUSBHost`, draining the capture
FIFO the duplex example already drains by hand), one new rt1062-only capstone
example, and zero driver surgery — clock ownership goes to `AudioOutputI2S` by
declaration order, and `AudioOutputUSBHost`'s pacing self-disables when it does
not own the clock. Separately, 5a's deferred `sai1-rxinject` promise lands as a
two-board port of `audio/audioinput_i2s_test`.

**Tech Stack:** QEMU (C, chardev/qdev), CMake, ARM GCC 10, POSIX sh gates,
Teensy Audio Library, USBHost_t36, LinkServer + DAPLink for silicon.

**Spec:** `docs/superpowers/specs/2026-08-13-rt1062-capstone-phase5b-design.md`

---

## Critical context for the implementer

- **Run gates directly (`./run_qemu.sh`), never `sh run_qemu.sh`** — `gate_init`
  does `exec gtimeout ... "$0"` and needs the shebang.
- **Ask the runner for gate counts** (`./tools/run-all-qemu-gates.sh -l`), never
  a bare `find`. Its trailing `(N gate(s))` line means `wc -l` is one more.
- **Zero SKIP is load-bearing.**
- **A gate never names a QEMU machine, build dir, `-serial` chain, or capture
  path.** Ask `gate-lib.sh`: `gate_qemu_machine`, `gate_build_dir`,
  `gate_console`, `gate_capture_path`. ★ **Every per-run artifact MUST go
  through `gate_capture_path`** — a bare `"$DIR/<name>"` recreates the `-j 2`
  collision fixed on 2026-08-13 (two board halves deleting each other's live
  captures).
- **Prefer polling to `sleep`.** The fixed-sleep pattern is load-sensitive and
  has cost this tree real time; `usb_audio_uac1_test/run_qemu.sh` and
  `audio/audiooutput_i2s_test/run_qemu_audiooutput.sh` both show the poll shape.
- `cores/` and `teensy-cmake-macros/` showing untracked is NORMAL.
- **Commit messages go in a scratch file and use `git commit -F`.** Inline
  heredocs with apostrophes have caused shell parse errors in this repo. Use
  `/private/tmp/claude-501/-Users-nicholasnewdigate-Development-rt1170-evkb/00f4d3b7-4dac-4289-a027-bf3f8443d2a9/scratchpad/`.
- **★ NEVER push `~/Development/qemu2`.** It is GPL-2.0 and this firmware repo
  is deliberately MIT/BSD-only. Firmware→qemu2 is fine; the reverse is not.
- **`~/Development/Audio` is a sibling git repo.** Commit changes there
  separately (same scratch-file style, same trailer) and report the SHA — the
  `evkb.cmake` pin bump is its own step in Task 7.
- **★ Hardware safety.** Never leave a serial reader attached across ANY
  LinkServer operation — it can kernel-panic the host Mac. Kill readers → flash
  → kill probe daemons → attach reader. **The MIMXRT1060-EVKB has no SW4.**
- **`rt1176:dualcore/cm4_audio_test` is a nondeterministic permitted red.** Load
  does not predict it. It is the ONLY permitted red.
- **BSD `sed` on macOS has no `\b`** — a pattern using it is a silent no-op.
- Compiler: ARM GCC 10 at `/Applications/ARM_10/bin/`.

---

### Task 0: Baseline

- [ ] **Step 1: Confirm tree and counts**

```bash
cd ~/Development/rt1170/evkb && git branch --show-current && git status --short
./tools/run-all-qemu-gates.sh -l | tail -1
./tools/run-all-qemu-gates.sh audioinput_i2s_test
```

Expected: clean tree apart from `?? cores/` and `?? teensy-cmake-macros/`;
`(87 gate(s))`; and `PASS  rt1176:audio/audioinput_i2s_test`.

- [ ] **Step 2: Record the qemu2 and Audio baselines**

```bash
git -C ~/Development/qemu2 log --oneline -1 && git -C ~/Development/qemu2 status --short | head
git -C ~/Development/Audio log --oneline -1 && git -C ~/Development/Audio status --short | head
```

Note both SHAs — if a task must be reverted, that is where to go back to.
Expected qemu2 head: `6d98ec3b27` (the 5a sai1-tap binding).

---

### Task 1: Bind `sai1-rxinject` on the RT1062 SoC (qemu2, LOCAL-ONLY)

**Files:**
- Modify: `~/Development/qemu2/hw/arm/fsl-imxrt1062.c`

This is the promise 5a deferred. The tap's RX-side mirror is a property of the
same shared `TYPE_IMXRT_SAI` model (`DEFINE_PROP_CHR("rx-inject", ...)`), and
this SoC already instantiates that model — only the binding is missing.

- [ ] **Step 1: Extend the existing SAI1 binding block**

In `hw/arm/fsl-imxrt1062.c`, find the `if (i == 0) {` block inside the SAI
realize loop (the loop containing `sysbus_realize`, at ~line 585). Replace this
comment tail and block:

```c
         * Mirrors fsl-imxrt1170.c. The RX-side mirror ("sai1-rxinject") is
         * deliberately NOT bound here: nothing on this board reads SAI RX yet,
         * and an ungated model feature is what this tree's discipline argues
         * against. Phase 5b adds it against its own gate.
         */
        if (i == 0) {
            Chardev *tapchr = qemu_chr_find("sai1-tap");
            if (tapchr) {
                qdev_prop_set_chr(DEVICE(&s->sai[0]), "tap", tapchr);
            }
        }
```

with:

```c
         * Mirrors fsl-imxrt1170.c, including the RX-side injector below.
         */
        if (i == 0) {
            Chardev *tapchr = qemu_chr_find("sai1-tap");
            if (tapchr) {
                qdev_prop_set_chr(DEVICE(&s->sai[0]), "tap", tapchr);
            }

            /*
             * Bind a dedicated RX-sample injector chardev (id "sai1-rxinject")
             * to SAI1, mirror of the tap above: a test runner streams LE
             * int16 samples in, which queue into the RX ring for the guest to
             * read back via RDR / dma_rx.  Optional, like the tap.
             *
             * Phase 5b gates this: audio/audioinput_i2s_test on rt1062 injects
             * a known waveform and asserts AudioInputI2S sees its peak.
             */
            Chardev *rxchr = qemu_chr_find("sai1-rxinject");
            if (rxchr) {
                qdev_prop_set_chr(DEVICE(&s->sai[0]), "rx-inject", rxchr);
            }
        }
```

- [ ] **Step 2: Rebuild QEMU**

```bash
cd ~/Development/qemu2/build && ninja qemu-system-arm 2>&1 | tail -5
```

Expected: a successful build. **This takes several minutes — a running build is
not a hang.** Allow a 10-minute timeout.

- [ ] **Step 3: Regression-check the rt1176 injector after the rebuild**

★ This runs the **rt1176** machine, which this task did not change, so it is a
REGRESSION CHECK on the rebuild — not verification of the new rt1062 binding.
Task 2 is where the rt1062 binding is actually proven.

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh audioinput_i2s_test
```

Expected: `PASS  rt1176:audio/audioinput_i2s_test`. If it fails, something broke
in the rebuild — report BLOCKED, do not proceed.

- [ ] **Step 4: Commit (qemu2 only — NEVER push)**

Scratch file + `git -C ~/Development/qemu2 add hw/arm/fsl-imxrt1062.c && git -C ~/Development/qemu2 commit -F <file>`:

```
hw/arm/fsl-imxrt1062: bind the sai1-rxinject chardev to SAI1

The RX-side mirror of the sai1-tap binding, deferred in Phase 5a on the
grounds that nothing on this board read SAI RX yet and an ungated model
feature is what this tree's discipline argues against. Phase 5b gates it:
audio/audioinput_i2s_test now runs on rt1062, injecting a known waveform and
asserting AudioInputI2S sees its peak.

Same shape as the tap: a property of the shared TYPE_IMXRT_SAI model, which
this SoC already instantiates, bound before realize and only when the runner
supplies the chardev.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
```

---

### Task 2: Make `audioinput_i2s_test` two-board

**Files:**
- Modify: `examples/audio/audioinput_i2s_test/audioinput_i2s_test.cpp`
- Modify: `examples/audio/audioinput_i2s_test/CMakeLists.txt`
- Modify: `examples/audio/audioinput_i2s_test/run_qemu_audioinput.sh`
- Create: `examples/audio/audioinput_i2s_test/toolchain/rt1062-evkb.toolchain.cmake`
- Create: `examples/audio/audioinput_i2s_test/boards`

- [ ] **Step 1: Add the console alias and swap the codec class**

In `audioinput_i2s_test.cpp`, replace this line:

```cpp
#include "control_wm8962.h"
```

with:

```cpp
// The console is LPUART1 on BOTH boards. The two cores just name it
// differently: cores/imxrt1176 calls LPUART1 `Serial1`, while cores/teensy4
// follows the Teensy pin-0/1 convention and calls LPUART6 `Serial1` and LPUART1
// `Serial6`. Naming it once here is what keeps QEMU and silicon reading the same
// wire -- on the MIMXRT1060-EVKB, LPUART1 (GPIO_AD_B0_12/13) is the DAPLink VCOM,
// whereas LPUART6 only reaches Arduino header pins D0/D1.
//
// The CODEC is a genuinely different chip, not a naming difference: the
// MIMXRT1170-EVKB has a WM8962, the MIMXRT1060-EVKB a WM8960. Different register
// maps, different drivers. The I2C bus differs too (LPI2C5 vs LPI2C1, both at
// 0x1A) but needs no guard here -- control_wm8962.cpp uses Wire2 and
// control_wm8960.cpp uses Wire, so swapping the class swaps the bus.
#if defined(ARDUINO_MIMXRT1060_EVKB)
#include "control_wm8960.h"
#define CONSOLE       Serial6
#define BOARD_CODEC_T AudioControlWM8960
#else
#include "control_wm8962.h"
#define CONSOLE       Serial1
#define BOARD_CODEC_T AudioControlWM8962
#endif
```

- [ ] **Step 2: Use the codec typedef at the declaration**

Replace:

```cpp
AudioControlWM8962 wm;
```

with:

```cpp
BOARD_CODEC_T      wm;
```

- [ ] **Step 3: Point every console call at the alias**

```bash
cd ~/Development/rt1170/evkb/examples/audio/audioinput_i2s_test
sed -i '' 's/Serial1\./CONSOLE./g' audioinput_i2s_test.cpp
grep -n 'Serial1' audioinput_i2s_test.cpp
```

**No `\b` in that pattern** — BSD `sed` on macOS does not support it and the
command silently does nothing.

There are 8 `Serial1` occurrences pre-edit and **one is bare**:
`while (!Serial1) {}`. `sed` only catches `Serial1.` with a dot, so change that
one by hand to `while (!CONSOLE) {}`. Afterwards the only `Serial1` left should
be inside the comment block and the `#define CONSOLE Serial1` line.

- [ ] **Step 4: Guard TEENSY_VERSION**

In `CMakeLists.txt`, replace line 3 (`set(TEENSY_VERSION 117 CACHE STRING "")`)
with:

```cmake
# Fallback only: the toolchain file sets this FORCE-fully, so a toolchain-driven
# build wins. A bare `cmake -B build .` with no toolchain still selects 117.
# Left unguarded this caches 117 and silently builds an RT1176 image into
# build-rt1062/, which boots the wrong QEMU machine and fails looking like a
# board or model problem rather than a build misconfiguration.
if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()
```

- [ ] **Step 5: Compile the right codec driver per board**

In `CMakeLists.txt`, remove this line from the `target_sources(...)` block:

```cmake
    ${EVKB_AUDIO_DIR}/control_wm8962.cpp
```

Then, immediately **after** the closing `)` of that `target_sources` block, add:

```cmake
# The two boards carry different codecs -- WM8962 on the MIMXRT1170-EVKB, WM8960
# on the MIMXRT1060-EVKB -- so exactly one driver is compiled. EVKB_BOARD is set
# by evkb.cmake (line 59) and is a normal CMake cache variable here.
if(EVKB_BOARD STREQUAL "rt1062")
    # output_i2s.cpp's __IMXRT1062__ path drives PLL4 through set_audioClock()
    # (utility/imxrt_hw.cpp -- self-guarded to 1052/1062, an empty TU elsewhere);
    # the __IMXRT1176__ path programs its clocks inline and never references it.
    # input_i2s.cpp calls AudioOutputI2S::config_i2s(), so this applies here too.
    target_sources(audioinput_i2s_test.elf PRIVATE
        ${EVKB_AUDIO_DIR}/control_wm8960.cpp
        ${EVKB_AUDIO_DIR}/utility/imxrt_hw.cpp
    )
else()
    target_sources(audioinput_i2s_test.elf PRIVATE ${EVKB_AUDIO_DIR}/control_wm8962.cpp)
endif()
```

- [ ] **Step 6: Add the rt1062 toolchain file**

Verbatim copy — this example sits at the same directory depth as `serial_test`,
so the internal path walk is identical:

```bash
cd ~/Development/rt1170/evkb/examples
cp serial/serial_test/toolchain/rt1062-evkb.toolchain.cmake \
   audio/audioinput_i2s_test/toolchain/rt1062-evkb.toolchain.cmake
```

- [ ] **Step 7: Declare the boards**

Create `examples/audio/audioinput_i2s_test/boards`:

```
# Boards this example is built and gated for. See
# docs/superpowers/specs/2026-08-13-rt1062-capstone-phase5b-design.md
#
# The rt1062 half is what gates qemu2's sai1-rxinject binding on
# fsl-imxrt1062 -- the RX-side promise Phase 5a deferred.
rt1176
rt1062
```

- [ ] **Step 8: Build both boards**

```bash
cd ~/Development/rt1170/evkb/examples/audio/audioinput_i2s_test
cmake --build build 2>&1 | tail -3
cmake -B build-rt1062 -DEVKB_BOARD=rt1062 \
      -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1062-evkb.toolchain.cmake
cmake --build build-rt1062 2>&1 | tail -3
```

Expected: both produce a `.elf`. If the rt1062 build fails on a chip-specific
assumption in `input_i2s.cpp`, fix it in the Audio library at
`~/Development/Audio` (where the `__IMXRT1176__` / `__IMXRT1062__` pattern
already exists), **not** in this example, and report exactly what you changed.
If such a fix grows beyond a few localized guards, STOP and report BLOCKED.

- [ ] **Step 9: Prove the rt1062 image is genuinely RT1062, and clear of ITCM page 0**

```bash
/Applications/ARM_10/bin/arm-none-eabi-readelf -h build-rt1062/audioinput_i2s_test.elf | grep Entry
/Applications/ARM_10/bin/arm-none-eabi-readelf -h build/audioinput_i2s_test.elf | grep Entry
/Applications/ARM_10/bin/arm-none-eabi-nm -n build-rt1062/audioinput_i2s_test.elf \
  | grep -vE "_stext|_itcm_block|teensy_model|flexram_bank" \
  | awk '$2 ~ /^[TtWw]$/ {print "first code @ 0x"$1" "$3; exit}'
```

Expected: `0x60001000` for rt1062, `0x30001000` for rt1176, and first code at
`0x00000400` (the NULL-trap MPU page hole from cores `5bcae78`). If the entry
points match, the toolchain file was ignored — recheck Step 4. If first code is
at `0x20`, the ELF predates the hole — `rm` it and rebuild.

- [ ] **Step 10: Prove the right codec linked into each image**

```bash
for b in build build-rt1062; do
  printf "%-14s " "$b"
  /Applications/ARM_10/bin/arm-none-eabi-nm $b/audioinput_i2s_test.elf \
    | grep -ciE "wm8960" | tr '\n' ' '
  /Applications/ARM_10/bin/arm-none-eabi-nm $b/audioinput_i2s_test.elf \
    | grep -ciE "wm8962"
done
```

Expected, as `<wm8960-count> <wm8962-count>`: `build` → `0 <non-zero>`;
`build-rt1062` → `<non-zero> 0`.

- [ ] **Step 11: Take the gate to two boards**

In `run_qemu_audioinput.sh`, replace this line:

```sh
VCOM="$DIR/vcom.uart"; DBG="$DIR/audioinput.dbg"; INJ="$DIR/inject.raw"
```

with:

```sh
VCOM=$(gate_capture_path "$DIR" vcom.uart)
DBG=$(gate_capture_path "$DIR" audioinput.dbg)
INJ=$(gate_capture_path "$DIR" inject.raw)
```

★ All three go through `gate_capture_path` — the injector file and its fifo are
per-run artifacts too, and two board halves racing on one fifo is the same
collision class the capture paths just had.

Then replace this line:

```sh
    -display none -serial file:"$VCOM" \
```

with:

```sh
    -display none $(gate_console "$VCOM") \
```

- [ ] **Step 12: Replace the fixed sleep with a poll**

Adding a second board doubles this gate's exposure to the fixed-sleep flake that
cost `audiooutput_i2s_test` two reds on 2026-08-13. Unlike that gate, this one's
assertions are purely console-side, so a single-condition poll is correct.

Replace:

```sh
P=$!; gate_pid $P
sleep 5; gate_reap $P
```

with:

```sh
P=$!; gate_pid $P
# Poll for the verdict token rather than guessing a duration -- a fixed sleep
# makes the gate load-sensitive, which has cost this tree real time.
#
# ★ Poll the LAST token this gate asserts, not the first interesting one. The
# firmware prints STAGE_PEAK= and then AUDIOINPUT_ALL= on the very next line,
# and gate_reap fires on the statement right after the loop -- so polling
# STAGE_PEAK= leaves a window where a tick lands between the two printlns, QEMU
# is killed before the second reaches the file, and the gate reports
# "FAIL: AUDIOINPUT_ALL" for a run that passed. Match the `=` so a FAIL verdict
# ends the wait too and is reported by name below, rather than spinning to the
# cap and being blamed on the timeout. Break early if QEMU has died, so an
# instant crash is reported as a missing capture instead of burning the cap.
for _ in $(seq 1 80); do
    [ -f "$VCOM" ] && grep -q "AUDIOINPUT_ALL=" "$VCOM" 2>/dev/null && break
    kill -0 "$P" 2>/dev/null || break
    sleep 0.25
done
gate_reap $P
```

- [ ] **Step 13: Run both boards**

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh audioinput_i2s_test
```

Expected: `PASS  rt1176:audio/audioinput_i2s_test` and
`PASS  rt1062:audio/audioinput_i2s_test`.

**If the rt1062 half fails, report the actual output before changing anything.**
Two failures have specific meanings:
- `info peak=0.0000` / `STAGE_PEAK=FAIL` → Task 1's rxinject binding did not
  take, or the injector fifo is not reaching the model. Check `vcom.uart` and
  the `.dbg` under `build-rt1062/`.
- "no UART capture" → check the ELF layout per Task 2 Step 9 before anything
  else.

- [ ] **Step 14: Confirm the count moved 87 → 88**

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh -l | tail -1
```

Expected: `(88 gate(s))`.

- [ ] **Step 15: Commit**

Scratch file + `git commit -F`:

```
audioinput_i2s_test: gate the SAI RX path on both boards

Ports the example to rt1062 by the established recipe -- CONSOLE alias,
WM8960 behind ARDUINO_MIMXRT1060_EVKB, codec-per-board CMake with
utility/imxrt_hw.cpp for the __IMXRT1062__ clock path, toolchain file, boards
sidecar -- and takes the gate to gate_console. Sweep 87 -> 88.

This is what gates qemu2's sai1-rxinject binding on fsl-imxrt1062, the RX-side
promise Phase 5a deferred on the grounds that an ungated model feature is what
this tree's discipline argues against. A fresh clone therefore sees the rt1062
half red, same GPL-firewall situation as usb_descriptor_survey and the tap.

Two hardening changes while here: all per-run artifacts (capture, debug log,
AND the injector file/fifo) go through gate_capture_path, so the two board
halves cannot delete each other's under -j 2; and the fixed sleep 5 becomes a
poll on the STAGE_PEAK= verdict, since adding a second board doubles this
gate's exposure to the load flake that reddened audiooutput twice.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
```

Stage exactly: the `.cpp`, `CMakeLists.txt`, `run_qemu_audioinput.sh`, `boards`,
and the toolchain file.

---

### Task 3: `AudioInputUSBHost` — the capture FIFO as a graph source

**Files:**
- Create: `~/Development/Audio/input_usbhost.h`
- Create: `~/Development/Audio/input_usbhost.cpp`

USB capture has never fed the AudioStream graph — Stage C's echo mode drained
the FIFO by hand inside the sketch. This is that drain, as a node.

- [ ] **Step 1: Write the header**

Create `~/Development/Audio/input_usbhost.h`:

```cpp
/* Audio Library for Teensy 3.X / i.MX RT
 * Copyright (c) 2026 Nicholas Newdigate
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice, development funding notice, and this
 * permission notice shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

// Audio graph SOURCE fed by a USB Audio Class device's capture (IN) stream,
// via USBHost_t36's USBAudioOut driver.
//
// Mirror of AudioOutputUSBHost, with one deliberate asymmetry: this node does
// NOT own the graph's clock and cannot. The sink node can pace the graph off
// FIFO occupancy because it decides when to produce; a source has no such
// lever -- the device sends what its converter produces, whenever it produces
// it, and the graph must be running to collect it. So some other node owns the
// clock (AudioOutputI2S, in the capstone) and this one takes whatever the
// capture FIFO holds when its update() is called.
//
// That means the two clocks are genuinely independent and there is no
// resampler between them: the adapter's converter runs on the adapter's
// crystal, the SAI on the board's. This project has measured that offset five
// times on the bench device -- about -86 ppm -- which at 44.1 kHz is roughly 4
// frames per second of drift. The FIFO absorbs it until it does not, and then
// one update() finds fewer than a block's worth of samples: underruns()
// counts that, and the short read is zero-filled so the shortfall is silence
// rather than stale audio. About one such block every quarter of a second at
// -86 ppm is not audible as pitch error; it is a very occasional tick.
//
// If a future design needs that tick gone, the fix is the fork's Resampler
// between this node and the sink, not a change here.

#ifndef input_usbhost_h_
#define input_usbhost_h_

#include "Arduino.h"
#include "AudioStream.h"
#include <USBHost_t36.h>

class AudioInputUSBHost : public AudioStream
{
public:
	AudioInputUSBHost(USBAudioOut &usb);
	virtual void update(void);

	// update()s that found less than a full block in the capture FIFO. The
	// tail of such a block is zero-filled. Non-zero and slowly rising is
	// normal -- it is the device's crystal drifting against the graph's
	// clock. Rising at the update rate (~344/s at 44.1 kHz) means no capture
	// data is arriving AT ALL, which is what a device with no input
	// interface looks like, and is the expected state in QEMU.
	uint32_t underruns(void) const { return short_reads; }

	// Blocks dropped because the audio memory pool was empty when update()
	// ran. Non-zero means AudioMemory() is undersized for this graph.
	uint32_t dropped(void) const { return blocks_dropped; }

private:
	USBAudioOut &audio;
	uint32_t short_reads;
	uint32_t blocks_dropped;
};

#endif
```

- [ ] **Step 2: Write the implementation**

Create `~/Development/Audio/input_usbhost.cpp` (same 22-line MIT header as the
`.h` above — copy it verbatim, then):

```cpp
#include <Arduino.h>
#include "input_usbhost.h"

AudioInputUSBHost::AudioInputUSBHost(USBAudioOut &usb)
	: AudioStream(0, NULL), audio(usb), short_reads(0), blocks_dropped(0)
{
	// Normalise the wire geometry to at most stereo by construction, the way
	// AudioOutputUSBHost pins the rate in its constructor. The driver unpacks
	// whatever the device's alt actually carries (the bench dongle captures
	// 1ch/16; uac_pack16 handles the wire format underneath), and this caps
	// what read() hands back so update()'s stack buffer cannot be overrun by
	// a device that captures more channels than a stereo graph can use.
	//
	// This is a REQUEST. captureChannels() reads back what was negotiated,
	// which for a mono device is 1 -- update() fans that to both outputs.
	usb.captureChannels(2);
}

void AudioInputUSBHost::update(void)
{
	audio_block_t *left = allocate();
	if (left == NULL) { blocks_dropped++; return; }
	audio_block_t *right = allocate();
	if (right == NULL) { release(left); blocks_dropped++; return; }

	// Clamped defensively as well as by the constructor's request: a driver
	// that ever reported more would otherwise overrun `buf`.
	uint8_t ch = audio.captureChannels();
	if (ch == 0) ch = 1;
	if (ch > 2) ch = 2;

	const uint32_t want = (uint32_t)AUDIO_BLOCK_SAMPLES * ch;
	int16_t buf[AUDIO_BLOCK_SAMPLES * 2];
	const uint32_t got = audio.read(buf, want);

	// A short read is the drift (or, in QEMU, a device with no input at all).
	// Zero-fill the tail rather than leaving the previous pass's samples in
	// place: silence is an honest gap, stale audio is a click that also lies
	// about what arrived.
	if (got < want) {
		short_reads++;
		for (uint32_t i = got; i < want; i++) buf[i] = 0;
	}

	// Capture -> stereo. One channel is fanned to both (the dongle's case);
	// two are taken as L,R.
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		const int16_t l = buf[i * ch];
		left->data[i]  = l;
		right->data[i] = (ch >= 2) ? buf[i * ch + 1] : l;
	}

	transmit(left, 0);
	transmit(right, 1);
	release(left);
	release(right);
}
```

- [ ] **Step 3: Commit to the Audio fork**

Scratch file + `git -C ~/Development/Audio add input_usbhost.h input_usbhost.cpp && git -C ~/Development/Audio commit -F <file>`:

```
input_usbhost: a graph source fed by USB capture

USB capture has never fed the AudioStream graph. Stage C's echo mode drained
USBAudioOut's capture FIFO by hand inside the sketch and wrote it straight
back to the OUT side; this is that drain as a node, so captured audio can
reach any sink -- in Phase 5b's capstone, the MIMXRT1060-EVKB's WM8960.

Deliberately does NOT own the graph clock, and cannot: a sink can pace the
graph off FIFO occupancy because it decides when to produce, but a source has
no such lever. Some other node owns the clock and this one takes what the FIFO
holds. The two crystals are therefore independent with no resampler between
them -- about -86 ppm on the bench device, ~4 frames/s at 44.1 kHz -- so a
short read is zero-filled and counted rather than papered over.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
```

Report the SHA — Task 7 bumps the pin.

---

### Task 4: The capstone example, and what QEMU actually reaches

**Files:**
- Create: `examples/usb/usb_audio_capstone_test/usb_audio_capstone_test.cpp`
- Create: `examples/usb/usb_audio_capstone_test/CMakeLists.txt`
- Create: `examples/usb/usb_audio_capstone_test/boards`
- Create: `examples/usb/usb_audio_capstone_test/toolchain/rt1062-evkb.toolchain.cmake`

★ **This task ends by MEASURING what QEMU reaches. It does not write gate
assertions — Task 5 does, from the measurement.** Phase 4 wrote an assertion
(`SITD PASS`) from second-hand evidence and it was unreachable; that cost a
correction commit. Do not repeat it.

- [ ] **Step 1: Write the sketch**

Create `examples/usb/usb_audio_capstone_test/usb_audio_capstone_test.cpp`:

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "USBHost_t36.h"
#include "utility/imxrt_usbhs.h"   // USBHS_PORTSC1, for port= in the heartbeat
#include "AudioStream.h"
#include "synth_sine.h"
#include "analyze_peak.h"
#include "output_i2s.h"
#include "output_usbhost.h"
#include "input_usbhost.h"
#include "control_wm8960.h"

// THE CAPSTONE. Both audio directions, in one graph, on one board:
//
//   sine 1 kHz ---> AudioOutputUSBHost ---> USB OUT ---> adapter DAC
//                                                            |
//                                                    [loopback cable]
//                                                            v
//   WM8960 <--- AudioOutputI2S <--- AudioInputUSBHost <--- adapter ADC
//
// The MIMXRT1060-EVKB can do what the RT1176 capstone could not: it has a
// working on-board codec on the SAME board as the USB host port. So the tone
// this firmware plays out over USB comes back in through the adapter's own
// microphone input and leaves through the board's own line out. Hearing it is
// end-to-end proof of both directions plus the graph in between.
//
// rt1062-only (see the `boards` sidecar): the RT1176 already has its own
// capstone in dualcore/cm4_graph_usb_capstone.
//
// ★ QEMU CANNOT PROVE THE ROUND TRIP. Its usb-audio model is playback-only
// (audio_be_open_out and nothing else) and isochronous data does not flow
// against it at all. There, `usbIn` finds an empty FIFO forever and feeds the
// codec silence. That is the expected emulated outcome, and the gate asserts
// it precisely -- see run_qemu.sh. Silicon is the sole proof audio moves.

// LPUART1 -- the EVKB's DAPLink VCOM. cores/teensy4 follows the Teensy
// pin-0/1 convention and gives the name Serial1 to LPUART6, which on this
// board only reaches Arduino header pins D0/D1. There is no rt1176 arm here
// because this example is rt1062-only.
#define CONSOLE Serial6

// The bench adapter (GeneralPlus 1B3F:2008) captures 1ch/16 at 44100 or 48000
// -- read off the wire by usb/usb_descriptor_survey on this very board. The
// driver refuses to approximate a format the device never offered, so these
// must match an alt exactly. 44100 is what the graph runs at
// (AUDIO_SAMPLE_RATE_EXACT), which is also what AudioOutputUSBHost pins the
// OUT side to in its constructor, so both directions use the device's one
// converter clock.
#ifndef CAPSTONE_IN_RATE_HZ
#define CAPSTONE_IN_RATE_HZ 44100
#endif
#ifndef CAPSTONE_IN_CHANNELS
#define CAPSTONE_IN_CHANNELS 1
#endif
#ifndef CAPSTONE_IN_BITS
#define CAPSTONE_IN_BITS 16
#endif

USBHost              myusb;
DMAMEM USBHub        hub1(myusb);
DMAMEM USBAudioOut   audioOut(myusb);

// ★ DECLARATION ORDER IS LOAD-BEARING, AND THIS IS THE WHOLE CLOCKING DESIGN.
//
// AudioStream::update_setup() is first-caller-wins (`if (update_scheduled)
// return false;`), and globals in one translation unit are constructed in
// declaration order. So AudioOutputI2S, declared FIRST, owns the graph's
// clock -- its SAI TX DMA interrupt paces update_all(), which is the same
// arrangement audio/audiooutput_i2s_test proved on this board in Phase 5a.
//
// AudioOutputUSBHost, declared after, gets `false` back from update_setup()
// and its frame_consumed() callback returns immediately on that -- so its
// FIFO-occupancy pacing self-disables and it degrades to a plain graph-paced
// FIFO writer. No change to that driver was needed to make this work; the
// guard was already there. Its dropped() counter then reports the drift: the
// USB FIFO gains about 7.6 samples/s against roughly 3300 samples of headroom
// above its target (USB_AUDIO_FIFO_SAMPLES 4096 - FIFO_TARGET_SAMPLES 768),
// so expect one counted drop every seven minutes or so, not zero and not many.
//
// SWAP THESE TWO LINES AND THE GRAPH RUNS ON THE WRONG CLOCK: USB would pace
// it, the SAI would starve, and the symptom would be at the codec, three nodes
// away from the cause.
AudioOutputI2S         i2sOut;
AudioSynthWaveformSine sine;
AudioOutputUSBHost     usbOut(audioOut);
AudioInputUSBHost      usbIn(audioOut);
AudioAnalyzePeak       inPeak;
AudioControlWM8960     codec;

AudioConnection pcOutL(sine,  0, usbOut, 0);   // OUT leg: tone to the device
AudioConnection pcOutR(sine,  0, usbOut, 1);
AudioConnection pcInL (usbIn, 0, i2sOut, 0);   // IN leg: what came back
AudioConnection pcInR (usbIn, 1, i2sOut, 1);
AudioConnection pcPeak(usbIn, 0, inPeak, 0);   // ...and how loud it was

static const char *driver_names[] = { "Hub", "Audio" };
static const unsigned NDRIVERS = 2;
static USBDriver *drivers[] = { &hub1, &audioOut };
static bool driver_active[NDRIVERS] = { false, false };

static uint32_t seq, last_beat_ms;
static bool     announced_ready;

void setup() {
    CONSOLE.begin(115200);
    while (!CONSOLE) {}
    CONSOLE.println("CAPSTONE: start");

    // Both legs plus the peak analyser; the IN leg allocates two blocks per
    // update and the I2S sink holds its own, so this is deliberately generous.
    AudioMemory(40);

    codec.enable();
    codec.volume(0.8f);
    sine.frequency(1000.0f);
    sine.amplitude(0.5f);

    // Turning the input path on at all. req_in_channels stays 0 until this is
    // called, and every input step -- the alt search in claim(), the extra
    // control requests, the IN ring -- is gated on it.
    audioOut.formatIn(CAPSTONE_IN_RATE_HZ, CAPSTONE_IN_CHANNELS, CAPSTONE_IN_BITS);

    myusb.begin();
    CONSOLE.println("CAPSTONE: host started, waiting for device");
    last_beat_ms = millis();
}

void loop() {
    myusb.Task();

    for (unsigned i = 0; i < NDRIVERS; i++) {
        bool now_active = (bool)*drivers[i];
        if (now_active == driver_active[i]) continue;
        driver_active[i] = now_active;
        if (now_active) {
            CONSOLE.printf("CAPSTONE: + %s vid=%04X pid=%04X\n",
                           driver_names[i], drivers[i]->idVendor(),
                           drivers[i]->idProduct());
        } else {
            CONSOLE.printf("CAPSTONE: - %s detached\n", driver_names[i]);
        }
    }

    if (!announced_ready && audioOut.ready()) {
        announced_ready = true;
        // alt= is a NEGOTIATION RESULT -- the driver matched the device's
        // declared alternate settings and selected one. rate= and ch= are NOT:
        // USBAudioOut::rate()/channels() return req_rate/req_channels, the
        // values AudioOutputUSBHost's constructor asked for, so printing them
        // is the firmware quoting itself. They are here to make the console
        // readable, and the gate does not assert them for that reason.
        // (There is no bits() getter; the request is 16 by construction.)
        CONSOLE.printf("CAPSTONE: OUT READY alt=%d rate=%lu ch=%d\n",
                       audioOut.alternateSetting(),
                       (unsigned long)audioOut.rate(),
                       audioOut.channels());
        // Input is selected by control requests that run AFTER the output
        // ones, so readyIn() can still be false here. The heartbeat's in=
        // field is what reports whether it ever became true.
        CONSOLE.println(audioOut.readyIn() ? "CAPSTONE: IN READY"
                                           : "CAPSTONE: IN not ready yet");
    }

    if (millis() - last_beat_ms >= 1000) {
        last_beat_ms += 1000;
        // in_peak is the loudness of what came BACK from the device. On
        // silicon with the loopback cable it tracks the tone; in QEMU, where
        // no capture data can exist, it is 0.0000 and underruns climbs at the
        // graph's update rate. Those two together are the emulated signature.
        float pk = inPeak.available() ? inPeak.read() : 0.0f;
        CONSOLE.printf("CAPSTONE: HEARTBEAT seq=%lu up=%lus out=%s in=%s "
                       "in_peak=%.4f out_drop=%lu in_under=%lu port=%08lX\n",
                       (unsigned long)++seq,
                       (unsigned long)(millis() / 1000),
                       audioOut.ready()   ? "ready" : "none",
                       audioOut.readyIn() ? "ready" : "none",
                       pk,
                       (unsigned long)usbOut.dropped(),
                       (unsigned long)usbIn.underruns(),
                       (unsigned long)USBHS_PORTSC1);
    }
}
```

- [ ] **Step 2: Write the CMakeLists**

Create `examples/usb/usb_audio_capstone_test/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(usb_audio_capstone_test)

# Fallback only: the toolchain file sets this FORCE-fully, so a toolchain-driven
# build wins. A bare `cmake -B build .` with no toolchain still selects 117.
# Left unguarded this caches 117 and silently builds an RT1176 image into
# build-rt1062/, which boots the wrong QEMU machine and fails looking like a
# board or model problem rather than a build misconfiguration.
if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(EEPROM EVKB_EEPROM_DIR)
evkb_library_dir(SPI EVKB_SPI_DIR)
evkb_library_dir(SdFat EVKB_SDFAT_DIR)
evkb_library_dir(USBHost_t36 EVKB_USBHOST_T36_DIR)
evkb_library_dir(Audio EVKB_AUDIO_DIR)
import_evkb_library(Wire)

teensy_add_executable(usb_audio_capstone_test usb_audio_capstone_test.cpp)
teensy_target_link_libraries(usb_audio_capstone_test cores Wire)

# --- USBHost_t36 (UAC1 duplex) ------------------------------------------------
# Transport core plus the UAC1 driver, the same set usb_audio_uac1_test and
# usb_audio_duplex_test compile. usb_audio_feedback / usb_audio2_parse /
# usb_audio_ctrl are not optional even for a UAC1-only sketch: usb_audio.cpp
# references all three unconditionally.
set(USBHOST ${EVKB_USBHOST_T36_DIR})
target_sources(usb_audio_capstone_test.elf PRIVATE
    ${USBHOST}/ehci.cpp ${USBHOST}/ehci_iso.cpp ${USBHOST}/enumeration.cpp ${USBHOST}/hub.cpp
    ${USBHOST}/memory.cpp ${USBHOST}/print.cpp
    ${USBHOST}/usb_audio.cpp ${USBHOST}/usb_audio_parse.cpp
    ${USBHOST}/usb_audio_fifo.cpp ${USBHOST}/usb_audio_feedback.cpp
    ${USBHOST}/usb_audio2_parse.cpp ${USBHOST}/usb_audio_ctrl.cpp)

# --- Audio graph --------------------------------------------------------------
# Both legs: the sine source and USB sink (as usb_audio_graph_test), plus the
# USB source and I2S sink with its codec (as audiooutput_i2s_test). This is the
# first example to compile all four together.
#
# output_i2s.cpp's isr() calls into memcpy_tointerleave{LR,L,R} (memcpy_audio.S,
# guarded __ARM_ARCH_7EM__ -- defined for -mcpu=cortex-m7 too) which
# import_arduino_library's own .S handling never sees here because these Audio
# sources are added directly, not via that helper's ROOT glob. Mirror its trick
# (LANGUAGE C + assembler-with-cpp) so the plain C compiler assembles it.
#
# utility/imxrt_hw.cpp is unconditional here, unlike the two-board audio
# examples: this example is rt1062-only, and output_i2s.cpp's __IMXRT1062__
# path drives PLL4 through its set_audioClock().
target_sources(usb_audio_capstone_test.elf PRIVATE
    ${EVKB_AUDIO_DIR}/synth_sine.cpp
    ${EVKB_AUDIO_DIR}/data_waveforms.c
    ${EVKB_AUDIO_DIR}/analyze_peak.cpp
    ${EVKB_AUDIO_DIR}/output_usbhost.cpp
    ${EVKB_AUDIO_DIR}/input_usbhost.cpp
    ${EVKB_AUDIO_DIR}/output_i2s.cpp
    ${EVKB_AUDIO_DIR}/control_wm8960.cpp
    ${EVKB_AUDIO_DIR}/utility/imxrt_hw.cpp
    ${EVKB_AUDIO_DIR}/memcpy_audio.S)
set_source_files_properties(${EVKB_AUDIO_DIR}/memcpy_audio.S PROPERTIES
    LANGUAGE C
    COMPILE_OPTIONS "-x;assembler-with-cpp")

# USBHost_t36.h unconditionally includes <FS.h> (from the core) and <SdFat.h>.
# Include dirs only -- no SdFat/SPI sources are compiled or linked, since only
# the excluded MassStorageDriver.cpp references those symbols.
target_include_directories(usb_audio_capstone_test.elf PRIVATE
    ${USBHOST} ${USBHOST}/utility
    ${EVKB_AUDIO_DIR}
    ${EVKB_SDFAT_DIR}/src
    ${EVKB_SPI_DIR}
    ${EVKB_EEPROM_DIR})

target_link_libraries(usb_audio_capstone_test.elf stdc++)
target_link_libraries(usb_audio_capstone_test.elf m)
```

- [ ] **Step 3: Add the boards sidecar and toolchain**

Create `examples/usb/usb_audio_capstone_test/boards`:

```
# Boards this example is built and gated for. See
# docs/superpowers/specs/2026-08-13-rt1062-capstone-phase5b-design.md
#
# rt1062 ONLY. The capstone is the MIMXRT1060-EVKB's showcase -- a working
# on-board codec on the same board as the USB host port, which the RT1176-EVKB
# does not have. The RT1176 has its own capstone in
# dualcore/cm4_graph_usb_capstone.
rt1062
```

```bash
cd ~/Development/rt1170/evkb/examples
mkdir -p usb/usb_audio_capstone_test/toolchain
cp serial/serial_test/toolchain/rt1062-evkb.toolchain.cmake \
   usb/usb_audio_capstone_test/toolchain/rt1062-evkb.toolchain.cmake
```

- [ ] **Step 4: Build**

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_capstone_test
cmake -B build-rt1062 -DEVKB_BOARD=rt1062 \
      -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1062-evkb.toolchain.cmake
cmake --build build-rt1062 2>&1 | tail -3
```

Expected: `usb_audio_capstone_test.elf`. A link error naming
`AudioInputUSBHost` means Task 3's `input_usbhost.cpp` is not in the source list
above; a link error naming `set_audioClock` means `utility/imxrt_hw.cpp` is not.

- [ ] **Step 5: Verify the image and the clock owner**

```bash
/Applications/ARM_10/bin/arm-none-eabi-readelf -h build-rt1062/usb_audio_capstone_test.elf | grep Entry
/Applications/ARM_10/bin/arm-none-eabi-nm -n build-rt1062/usb_audio_capstone_test.elf \
  | grep -vE "_stext|_itcm_block|teensy_model|flexram_bank" \
  | awk '$2 ~ /^[TtWw]$/ {print "first code @ 0x"$1" "$3; exit}'
/Applications/ARM_10/bin/arm-none-eabi-nm build-rt1062/usb_audio_capstone_test.elf \
  | grep -icE "AudioInputUSBHost|AudioOutputUSBHost|AudioOutputI2S"
```

Expected: entry `0x60001000`; first code at `0x00000400`; a non-zero count for
the three node classes.

- [ ] **Step 6: ★ MEASURE what QEMU reaches — do not assume**

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_capstone_test
rm -f /tmp/cap-vcom.txt /tmp/cap-tap.raw /tmp/cap-dbg.log
gtimeout 20 ~/Development/qemu2/build/qemu-system-arm \
  -M mimxrt1060-evk -global fsl-imxrt1062.boot-ivt=on \
  -kernel build-rt1062/usb_audio_capstone_test.elf \
  -display none -serial file:/tmp/cap-vcom.txt \
  -chardev file,id=sai1-tap,path=/tmp/cap-tap.raw \
  -audiodev none,id=snd0 \
  -device usb-audio,bus=usbhost.0,port=1,audiodev=snd0 \
  -d guest_errors -D /tmp/cap-dbg.log >/dev/null 2>&1
echo "---- console ----"; cat /tmp/cap-vcom.txt
echo "---- tap ----"; ls -l /tmp/cap-tap.raw
python3 -c "
import struct
d = open('/tmp/cap-tap.raw','rb').read()
n = len(d)//2
s = struct.unpack('<%dh' % n, d[:n*2]) if n else ()
print('bytes', len(d), 'samples', n, 'peak', max((abs(x) for x in s), default=0))"
```

**Record the actual output and report it.** The specific question this answers,
which nothing in the tree has established: **does `formatIn()` against a device
with NO input interface break the OUT claim?** QEMU's `usb-audio` is
playback-only, so the input alt search must fail. Three outcomes, all
informative:

1. `OUT READY` printed, `in=none` in the heartbeat → the driver tolerates it.
   The gate asserts the OUT control plane plus the silent tap.
2. No `OUT READY` at all → `formatIn()` broke enumeration for a playback-only
   device. That is a **driver defect worth fixing in USBHost_t36** (a device
   that cannot capture should refuse the input path, not the whole claim).
   Report it; the fix belongs in the library, and the gate must not paper over
   it by dropping `formatIn()` from the sketch.
3. The tap is empty rather than silent → the graph is not running; check the
   clock-owner ordering in Step 1 before anything else.

Also note the tap's peak: it should be exactly **0** (a live graph pushing real
silence), and its size should be hundreds of KB.

---

### Task 5: The capstone gate

**Files:**
- Modify: `examples/audio/audiooutput_i2s_test/check_tap.py`
- Create: `examples/usb/usb_audio_capstone_test/run_qemu.sh`

- [ ] **Step 1: Teach `check_tap.py` to assert silence**

The capstone needs the inverse assertion: the tap grew at rate AND every sample
is zero. Rather than a second script, extend the existing one. Replace the whole
body of `examples/audio/audiooutput_i2s_test/check_tap.py` with:

```python
#!/usr/bin/env python3
# Assert what the SAI1 TX tap carries.
#
#   check_tap.py FILE                  -- non-silent: peak > 4000 (the tone)
#   check_tap.py --expect-silence FILE -- silent AT RATE: peak == 0 and the
#                                         tap grew past --min-bytes
#
# The silence mode is NOT the trivial inverse. An empty tap is also "not loud",
# and an empty tap means the graph never ran -- so the mode asserts a MINIMUM
# SIZE as well as a zero peak. That pair distinguishes "the graph is running
# and there is genuinely nothing to play" (usb_audio_capstone_test in QEMU,
# where the emulated usb-audio device cannot capture) from "the graph is dead",
# which would otherwise look identical.
import sys, struct

args = sys.argv[1:]
expect_silence = False
min_bytes = 131072          # ~1 s of tap at the observed QEMU drain rate
if "--expect-silence" in args:
    expect_silence = True
    args.remove("--expect-silence")
if "--min-bytes" in args:
    i = args.index("--min-bytes")
    min_bytes = int(args[i + 1])
    del args[i:i + 2]
path = args[0]

data = open(path, "rb").read()
n = len(data) // 2
if n == 0:
    print("STAGE_TONE=FAIL (empty tap)"); sys.exit(1)
samples = struct.unpack("<%dh" % n, data[:n*2])
peak = max(abs(s) for s in samples)
print("info tap_peak=%d (%.3f fs) bytes=%d" % (peak, peak/32767.0, len(data)))

if expect_silence:
    if len(data) < min_bytes:
        print("STAGE_SILENCE=FAIL (tap only %d bytes, want >= %d -- graph not running "
              "at rate)" % (len(data), min_bytes))
        sys.exit(1)
    ok = peak == 0
    print("STAGE_SILENCE=PASS" if ok else
          "STAGE_SILENCE=FAIL (expected pure silence, saw peak=%d)" % peak)
    sys.exit(0 if ok else 1)

# amplitude 0.5 full-scale -> ~16384; accept a wide band (QEMU FIFO/timing).
ok = peak > 4000
print("STAGE_TONE=PASS" if ok else "STAGE_TONE=FAIL")
sys.exit(0 if ok else 1)
```

- [ ] **Step 2: Prove the new mode is not vacuous**

Both directions, using the existing gate's own tap as a non-silent fixture:

```bash
cd ~/Development/rt1170/evkb/examples/audio/audiooutput_i2s_test
./run_qemu_audiooutput.sh >/dev/null 2>&1        # regenerates build/tap.raw
python3 check_tap.py build/tap.raw; echo "tone-mode rc=$?"
python3 check_tap.py --expect-silence build/tap.raw; echo "loud-tap rc=$? (want 1)"
head -c 200000 /dev/zero > /tmp/silence.raw
python3 check_tap.py --expect-silence /tmp/silence.raw; echo "silent-big rc=$? (want 0)"
head -c 1000 /dev/zero > /tmp/tiny.raw
python3 check_tap.py --expect-silence /tmp/tiny.raw; echo "silent-tiny rc=$? (want 1)"
```

Expected: `0`, `1`, `0`, `1` respectively. The third and fourth are the ones
that matter — they prove the mode requires size as well as silence.

- [ ] **Step 3: Confirm the audiooutput gate still passes**

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh audiooutput_i2s_test
```

Expected: both halves PASS. The default (no-flag) path must be unchanged.

- [ ] **Step 4: Write the gate**

Create `examples/usb/usb_audio_capstone_test/run_qemu.sh` (`chmod +x` it).
★ **Adjust the assertion list to what Task 4 Step 6 actually measured** — the
tokens below assume outcome 1 (the driver tolerates `formatIn()` against a
playback-only device). If it measured otherwise, report before writing.

```sh
#!/bin/sh
# QEMU gate for usb_audio_capstone_test -- the capstone's control plane and
# graph plumbing, on rt1062.
#
# WHAT THIS PROVES: the host stack enumerates QEMU's emulated usb-audio device,
# claims it, selects the streaming alternate setting, and that the audio graph
# is RUNNING -- the SAI1 TX tap grows at rate under a live AudioOutputI2S.
#
# It deliberately does NOT re-assert the descriptor topology: usb_audio_uac1_test
# gates that parse in full, on both boards, against QEMU's declared alt/ep/rate
# line. Duplicating it here would add a second thing to update when the model
# changes without adding coverage.
#
# ★ WHAT IT CANNOT PROVE: the round trip. QEMU's usb-audio model is
# PLAYBACK-ONLY (audio_be_open_out and nothing else) and isochronous data does
# not flow against it in either direction. So no captured audio can ever exist
# here: AudioInputUSBHost finds an empty FIFO on every update, zero-fills, and
# feeds the codec silence.
#
# That is why the tap assertion is INVERTED here relative to
# audio/audiooutput_i2s_test: --expect-silence demands peak == 0 AND a minimum
# size. The pair is the point. Peak 0 alone is also what a dead graph produces;
# the size requirement is what says the graph is alive and genuinely has
# nothing to play. in_under climbing in the heartbeat says the same thing from
# the firmware's side.
#
# Silicon is the sole proof audio moves: transcript_hw_evkb.txt, with a
# loopback cable from the adapter's headphone out to its own microphone in.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/usb_audio_capstone_test.elf"
OUT=$(gate_capture_path "$DIR" capstone.uart)
DBG=$(gate_capture_path "$DIR" capstone.dbg)
TAP=$(gate_capture_path "$DIR" tap.raw)
rm -f "$OUT" "$DBG" "$TAP"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -chardev file,id=sai1-tap,path="$TAP" \
    -audiodev none,id=snd0 \
    -device usb-audio,bus=usbhost.0,port=1,audiodev=snd0 \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# Wait on the run's two liveness signals rather than a fixed sleep, the same
# shape audio/audiooutput_i2s_test uses: the console token arrives well before
# the tap has accumulated enough for the size check below, so polling the token
# alone would reap early and fail on size.
TAP_MIN_BYTES=131072
WAIT_CAP=60
waited=0
while :; do
    if grep -q "HEARTBEAT seq=3 " "$OUT" 2>/dev/null; then
        sz=$(wc -c < "$TAP" 2>/dev/null | tr -d ' ')
        if [ "${sz:-0}" -ge "$TAP_MIN_BYTES" ]; then break; fi
    fi
    if ! kill -0 "$P" 2>/dev/null; then break; fi
    if [ "$waited" -ge "$WAIT_CAP" ]; then
        echo "note: liveness wait capped at ${WAIT_CAP}s (token or tap never arrived)"
        break
    fi
    sleep 1; waited=$((waited + 1))
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; head -20 "$OUT"

grep -q "CAPSTONE: start" "$OUT" || { echo "FAIL: banner"; exit 1; }
# External oracle: 46F4:0002 is QEMU's usb-audio identity, which the firmware
# has no knowledge of, so agreeing with it is not the firmware agreeing with
# itself.
grep -q "CAPSTONE: + Audio vid=46F4 pid=0002" "$OUT" \
    || { echo "FAIL: the emulated usb-audio device was not claimed"; exit 1; }
# alt=1 is a NEGOTIATION RESULT: the driver matched the device's declared
# alternate settings and picked the streaming one. Deliberately NOT asserted on
# the same line: rate= and ch=, which USBAudioOut echoes back from the REQUEST
# (rate() returns req_rate), so asserting them would only prove the sketch
# agrees with itself.
grep -q "CAPSTONE: OUT READY alt=1 " "$OUT" \
    || { echo "FAIL: OUT streaming alternate setting not selected"; exit 1; }
grep -q "HEARTBEAT seq=3 " "$OUT" \
    || { echo "FAIL: loop did not survive to a third heartbeat"; exit 1; }

# The emulated signature: no capture data can exist, so the IN side never goes
# ready and the input node underruns on every update. A ZERO in_under would
# mean the node is not being clocked at all.
grep -q "in=none" "$OUT" || { echo "FAIL: expected in=none against a playback-only model"; exit 1; }
grep -qE "in_under=[1-9][0-9]*" "$OUT" \
    || { echo "FAIL: in_under never advanced -- the input node is not being clocked"; exit 1; }

echo "==== TAP ===="
python3 "$EVKB/examples/audio/audiooutput_i2s_test/check_tap.py" \
    --expect-silence --min-bytes "$TAP_MIN_BYTES" "$TAP" \
    || { echo "FAIL: STAGE_SILENCE"; exit 1; }

echo "PASS: CAPSTONE_CONTROL_PLANE_AND_GRAPH"
```

- [ ] **Step 5: Run it**

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh usb_audio_capstone_test
./tools/run-all-qemu-gates.sh -l | tail -1
```

Expected: `PASS  rt1062:usb/usb_audio_capstone_test` and `(89 gate(s))`.

- [ ] **Step 6: Commit**

Scratch file + `git commit -F`. Stage the example directory (`.cpp`,
`CMakeLists.txt`, `boards`, `toolchain/`, `run_qemu.sh`) and
`examples/audio/audiooutput_i2s_test/check_tap.py`:

```
usb_audio_capstone_test: both audio directions in one graph

The capstone the board axis was aiming at: sine -> AudioOutputUSBHost -> USB
OUT, and USB IN -> AudioInputUSBHost -> AudioOutputI2S -> WM8960, on the one
board that has a codec and a host port together. With a loopback cable on the
adapter, the tone that leaves over USB comes back through its microphone input
and out the board's line out.

Clock ownership is by DECLARATION ORDER and no driver changed: update_setup()
is first-caller-wins, so AudioOutputI2S (declared first) paces the graph off
its SAI DMA, and AudioOutputUSBHost's frame_consumed() already returns early
when it does not own the clock. Its dropped() counter becomes the drift
instrument -- about one block every seven minutes at the bench device's
-86 ppm.

The gate asserts the control plane and that the graph RUNS, and says plainly
that it cannot assert the round trip: QEMU's usb-audio model is playback-only,
so no captured audio can exist there. Hence check_tap.py's new
--expect-silence mode, which demands peak == 0 AND a minimum tap size -- peak
0 alone is also what a dead graph produces, and the size is what distinguishes
a live graph with nothing to play. Sweep 88 -> 89.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
```

---

### Task 6: Silicon — the loopback round trip (INLINE, hardware)

**Files:**
- Create: `examples/usb/usb_audio_capstone_test/transcript_hw_evkb.txt`

★ **Read the hardware-safety rules in "Critical context" before this task.**
★ **This task needs the user at the bench.** Ask them to fit the loopback cable
(3.5 mm male-male, adapter headphone-out → adapter mic-in), confirm the adapter
is in **J47** (the host port; J48 is the device port and will not work), and put
a speaker on the board's line out.

- [ ] **Step 1: Free the VCOM and clear stale probe daemons**

```bash
pkill -9 -f "rt1170-console" 2>/dev/null
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
sleep 1; ls /dev/cu.usbmodem*; lsof /dev/cu.usbmodem* 2>/dev/null || echo "VCOM free"
```

Expected: `VCOM free`.

- [ ] **Step 2: Flash**

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_capstone_test
gtimeout 180 /Applications/LinkServer_26.6.137/LinkServer flash \
  MIMXRT1062:MIMXRT1060-EVKB load build-rt1062/usb_audio_capstone_test.elf 2>&1 | tail -3
```

Expected: `Finished writing Flash successfully` then `Starting execution using
system reset`.

- [ ] **Step 3: Drop the probe and read the console**

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 2
cd ~/Development/rt1170/evkb
gtimeout 40 python3 -u tools/rt1170-console.py /dev/cu.usbmodem14544402 115200 \
  > /tmp/capstone-hw.txt 2>/dev/null; true
pkill -9 -f "rt1170-console" 2>/dev/null
tail -20 /tmp/capstone-hw.txt
```

**Do not reset with the reader attached** — that is the kernel-panic path, and
this board has no SW4. The heartbeat prints every second, so a late attach still
captures plenty; the `CAPSTONE: start` / `OUT READY` lines may be missed, which
is expected and should be stated in the transcript rather than worked around.

- [ ] **Step 4: Confirm the bar**

**Listen.** The 1 kHz tone should be coming out of the board's line out, having
travelled USB OUT → adapter DAC → cable → adapter ADC → USB IN → graph → I2S.
Ask the user to confirm. Also read the numbers:

```bash
grep -c "HEARTBEAT" /tmp/capstone-hw.txt
grep -oE "in=(ready|none)" /tmp/capstone-hw.txt | sort | uniq -c
grep -oE "in_peak=[0-9.]+" /tmp/capstone-hw.txt | tail -5
grep -oE "out_drop=[0-9]+ in_under=[0-9]+" /tmp/capstone-hw.txt | tail -3
```

What each says:
- `in=ready` → the device's input interface was selected. If it stays `none`,
  the adapter's IN alt did not match `CAPSTONE_IN_*`; re-read the format from
  `usb/usb_descriptor_survey`'s transcript for this device and report rather
  than guessing.
- `in_peak` non-zero and steady → captured audio is reaching the graph. Near
  zero with `in=ready` means the cable or the adapter's input gain, not the
  firmware — say so plainly.
- `out_drop` / `in_under` → the drift instruments. Single-digit-per-minute
  growth is expected; runaway growth is a finding worth recording.

- [ ] **Step 4b: Bench-check the AudioInputI2S TX-clock fix (60 seconds, same board)**

★ **Added from Task 2's review.** Task 2 added `I2S1_TCSR |= I2S_TCSR_TE |
I2S_TCSR_BCE` to `AudioInputI2S::begin()` under `ARDUINO_MIMXRT1060_EVKB`,
because that board's `config_i2s()` makes RX synchronous to TX and RX therefore
gets no bit clock unless the transmitter is enabled. **QEMU cannot verify it**
— its SAI model gates RX on the RCSR bits alone — so the rt1062 gate passes
either way, and the fix currently rests on the identical already-fixed case in
the `__IMXRT1176__` branch. This is the only step that can turn it from argued
into measured, and the board is already on the bench:

```bash
pkill -9 -f "rt1170-console" 2>/dev/null
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 1
cd ~/Development/rt1170/evkb/examples/audio/audioinput_i2s_test
gtimeout 180 /Applications/LinkServer_26.6.137/LinkServer flash \
  MIMXRT1062:MIMXRT1060-EVKB load build-rt1062/audioinput_i2s_test.elf 2>&1 | tail -3
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 2
cd ~/Development/rt1170/evkb
gtimeout 25 python3 -u tools/rt1170-console.py /dev/cu.usbmodem14544402 115200 \
  > /tmp/audioin-hw.txt 2>/dev/null; true
pkill -9 -f "rt1170-console" 2>/dev/null
grep -oE "MIC peak=[0-9.()a-z ]+" /tmp/audioin-hw.txt | tail -10
```

Ask the user to make noise near the board (speak, tap it) during the capture.

- `MIC peak=` values that MOVE with sound → the fix works; RX is clocked and
  the codec's ADC data is reaching the graph.
- `MIC peak=0.0000` flat, or `(no blocks)` → RX is not receiving. Report it;
  do NOT patch around it, and do not let the green QEMU gate stand as evidence.
- If the MIMXRT1060-EVKB turns out to have no microphone wired to the WM8960 on
  this board revision, say so plainly and record the fix as **reasoned from the
  1176 precedent but unverified** in both the transcript and the KBG entry.
  That is an honest outcome; a silent assumption is not.

Then re-flash the capstone (Task 6 Step 2) before continuing, since this
overwrote it.

- [ ] **Step 5: Soak for the drift numbers**

```bash
cd ~/Development/rt1170/evkb
gtimeout 180 python3 -u tools/rt1170-console.py /dev/cu.usbmodem14544402 115200 \
  > /tmp/capstone-soak.txt 2>/dev/null; true
pkill -9 -f "rt1170-console" 2>/dev/null
head -2 /tmp/capstone-soak.txt; tail -2 /tmp/capstone-soak.txt
```

Three minutes is enough to see whether `out_drop` grows at roughly the predicted
one-per-seven-minutes rate or runs away.

- [ ] **Step 6: Write the transcript**

Create `transcript_hw_evkb.txt` on this skeleton, pasting the real capture in
place of the bracketed parts and deleting any line that did not happen:

```
Hardware transcript -- usb_audio_capstone_test on the MIMXRT1060-EVKB (rt1062).

Recorded <date>, branch <branch>, evkb <SHA>, cores <SHA>, Audio <SHA>,
USBHost_t36 <SHA>.
Board:  MIMXRT1060-EVKB, DAPLink VCOM, console on Serial6 (LPUART1).
Device: GeneralPlus 1B3F:2008 "USB Audio Device", UAC 1.00, in J47.
        J47 is the HOST port. J48 is the device port and will not work.
Bench:  3.5 mm male-male loopback, adapter headphone-out -> adapter mic-in.
        Speaker on the board's WM8960 line out.
Graph:  sine 1 kHz -> AudioOutputUSBHost -> USB OUT -> adapter DAC
        -> [cable] -> adapter ADC -> USB IN -> AudioInputUSBHost
        -> AudioOutputI2S -> WM8960.
Image:  build-rt1062/usb_audio_capstone_test.elf -- the SAME artifact the gate
        runs.

★ THIS FILE IS THE ONLY EVIDENCE THE ROUND TRIP WORKS. QEMU's usb-audio model
is playback-only and no isochronous data flows against it, so the gate can
only prove the control plane and that the graph runs (it asserts the SAI tap
is SILENT at rate). If this transcript and the gate ever disagree, silicon
wins.

ROUND TRIP: <audible / not audible> -- 1 kHz from the board line out, having
gone out over USB and come back in through the adapter's microphone input.

==== console ====
<paste, or state which setup lines were missed and why>

Notes:
- in= reached <ready/none>; in_peak settled around <value>.
- Drift instruments over a <N>-minute soak: out_drop <start> -> <end>,
  in_under <start> -> <end>. Predicted out_drop rate is about one per seven
  minutes (the USB FIFO gains ~7.6 samples/s against ~3300 samples of headroom
  above target at the bench device's -86 ppm). <state whether it matched>
- Clock ownership: AudioOutputI2S owns it by declaration order;
  AudioOutputUSBHost ran in its non-owner mode, which this is the first
  exercise of anywhere. <state whether out_drop stayed sane>
```

- [ ] **Step 7: Commit**

Scratch file + `git commit -F`, naming the bench setup, whether the round trip
was audible, and the drift numbers actually measured.

---

### Task 7: Close the phase

**Files:**
- Modify: `evkb.cmake` (Audio pin)
- Modify: `tools/license-audit.sh` (GATES list)
- Modify: `CLAUDE.md`, `docs/KNOWN-BROKEN-GATES.md`

- [ ] **Step 1: Bump the Audio pin**

Find the Audio line in `evkb.cmake` (`grep -n "newdigate/Audio" evkb.cmake`) and
replace its SHA with the full 40-character SHA of Task 3's commit. **Push the
Audio repo first** (`git -C ~/Development/Audio push origin master`) — the pin
must reference a commit a fresh clone can fetch. (`~/Development/Audio` is
MIT-licensed and is NOT qemu2; pushing it is correct and required.)

Then rebuild the capstone from scratch to prove the pin resolves:

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_capstone_test
rm -rf build-rt1062
cmake -B build-rt1062 -DEVKB_BOARD=rt1062 \
      -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1062-evkb.toolchain.cmake
cmake --build build-rt1062 2>&1 | tail -3
```

- [ ] **Step 2: Add both new builds to the licence audit**

In `tools/license-audit.sh`, in the `GATES=` list: add
`examples/audio/audioinput_i2s_test/build-rt1062:audioinput_i2s_test \` beneath
the existing `examples/audio/audioinput_i2s_test:audioinput_i2s_test` entry, and
add `examples/usb/usb_audio_capstone_test/build-rt1062:usb_audio_capstone_test \`
in the `examples/usb/...` run, keeping alphabetical order.

★ **Verify both by name in Step 3 — the audit's drift check cannot catch a
missing one.** That check iterates `examples/*/*/run_qemu*.sh` and matches on the
example directory; no `run_qemu.sh` corresponds to a build directory, so a
missing `…/build-rt1062:` entry is invisible to it and that image's link
manifest is silently never audited.

- [ ] **Step 3: Run the audit, capturing FULL output**

```bash
cd ~/Development/rt1170/evkb && ./tools/license-audit.sh > /tmp/p5b-audit.txt 2>&1; echo "exit=$?"
tail -2 /tmp/p5b-audit.txt
grep -E "audioinput_i2s_test|usb_audio_capstone_test" /tmp/p5b-audit.txt
```

Expected: `LICENSE-AUDIT: PASS`, and all four entries walked with non-zero
dep-path counts. Do **not** pipe the audit itself through `tail` — a truncated
log cannot tell you what it covered. Allow a 6-minute timeout.

★ This has a real chance of failing: the capstone is the first image to link
`input_usbhost.cpp` and to combine the Audio and USBHost_t36 trees on rt1062. If
a copyleft header shows up, report it — do not add an `ALLOW` entry to make it
pass.

- [ ] **Step 4: Full sweep, twice**

```bash
uptime && cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh -j 2
```

Run it **twice** (the second confirms the two new gates are not load-sensitive).
Allow 10 minutes per run; run them one at a time, in the foreground.

Expected each time: `89 passed, 0 failed, 0 SKIP`, or `88 passed, 1 failed,
0 SKIP` with only `rt1176:dualcore/cm4_audio_test` red. Any other failure is a
real regression — report the gate NAMES, do not re-run the world.

- [ ] **Step 5: Update `CLAUDE.md`**

Find the sweep paragraph (search for `The sweep covers`). Change the count to
**89**, extend the leading parenthetical at the front with
`87 before Phase 5b gated usb/usb_audio_capstone_test and audio/audioinput_i2s_test's second board;`,
and update both expectation numbers to `89 passed, 0 failed, 0 SKIP` and
`88 passed, 1 failed, 0 SKIP`. Do not restructure the paragraph.

In the same file, the `usb_descriptor_survey` paragraph names the gates whose
rt1062 halves depend on LOCAL-ONLY qemu2 changes. Add `audioinput_i2s_test` to
that list — its rt1062 half now needs the `sai1-rxinject` binding.

- [ ] **Step 6: Update `docs/KNOWN-BROKEN-GATES.md`**

Read the file first and match its house style. Add a dated entry at the top of
the dated run covering:

- Phase 5b lands the capstone: sweep **87 → 89**
  (`usb/usb_audio_capstone_test` on rt1062, `audio/audioinput_i2s_test`'s second
  board). Expectation `89/0/0`, or `88/1/0` with the nondeterministic
  `cm4_audio_test`. Zero SKIP either way.
- ★ **A fresh clone sees `rt1062:audio/audioinput_i2s_test` RED**: it needs the
  LOCAL-ONLY qemu2 `sai1-rxinject` binding on `fsl-imxrt1062`. Same GPL-firewall
  situation as the tap, `usb_descriptor_survey` and `cm4_usb_irq_probe`. The
  capstone gate needs only `sai1-tap`, which Phase 5a already bound.
- ★ **What the capstone gate does NOT prove, and why its tap assertion is
  inverted.** QEMU's `usb-audio` is playback-only, so no captured audio can
  exist; `--expect-silence` demands peak == 0 **and** a minimum tap size,
  because peak 0 alone is also what a dead graph produces. The round trip is
  silicon-only evidence (`transcript_hw_evkb.txt`, loopback cable).
- Clock ownership in the capstone is by **declaration order**
  (`update_setup()` is first-caller-wins). `AudioOutputI2S` first, so it paces
  the graph; `AudioOutputUSBHost` runs in its non-owner mode — its first
  exercise anywhere — and its `dropped()` counter is the drift instrument.
  Swapping the two declarations puts the graph on the wrong clock, and the
  symptom appears at the codec, three nodes from the cause.
- The measured silicon drift numbers from Task 6.

- [ ] **Step 7: Commit**

Scratch file + `git commit -F`. Subject:
`docs: close Phase 5b -- sweep 89, the capstone round trip on silicon`.

---

## Definition of done

- [ ] `sai1-rxinject` bound on `fsl-imxrt1062` (qemu2, LOCAL-ONLY, never pushed)
- [ ] `audioinput_i2s_test` builds and gates on both boards; rt1062 entry `0x60001000`
- [ ] `AudioInputUSBHost` committed to the Audio fork and pushed; pin bumped
- [ ] Capstone builds (rt1062), both legs in one graph, `AudioOutputI2S` owns the clock
- [ ] Capstone gate green, asserting the control plane and a silent-at-rate tap
- [ ] Audible 1 kHz round trip through the loopback cable on silicon
- [ ] `transcript_hw_evkb.txt` committed, with the drift numbers
- [ ] Sweep 89 gates, zero SKIP, twice
- [ ] `LICENSE-AUDIT: PASS` with both new build dirs walked
- [ ] `CLAUDE.md` and `KNOWN-BROKEN-GATES.md` updated

## Not in this phase

Rate servo / ASRC between the two crystals, feedback-endpoint reading on the
host side, teaching QEMU's usb-audio model to capture, a two-board capstone,
the other ten `examples/audio/*` on rt1062, and any frequency-domain assertion
on the tap.
