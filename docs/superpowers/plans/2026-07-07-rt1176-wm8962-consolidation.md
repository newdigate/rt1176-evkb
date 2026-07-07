# RT1176 WM8962 Codec-Driver Consolidation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move `WM8962Codec` out of the core (`cores/imxrt1176/wm8962.{h,cpp}`) into the Audio fork's single `control_wm8962.{h,cpp}`, consolidated with `AudioControlWM8962`, so the Audio repo owns its codec control (and the ADDCTL3 de-dup falls out via a public `writeReg`).

**Architecture:** Pure relocation + accessor-visibility change + de-dup — **byte-identical I2C behavior everywhere**, so the three existing QEMU gates are the regression check. Ordered to keep every task's build green: decouple `i2s_audio_test` first (anonymous-namespace copy, so it doesn't collide with the core's still-present class), then move+delete atomically.

**Tech Stack:** C++ (Teensy Audio structure), MIMXRT1176-EVKB, `Wire`/`TwoWire` (LPI2C5 = Wire2), QEMU `mimxrt1170-evk` gates.

---

## Repos & conventions

- **Core** `~/Development/rt1170/evkb/cores/imxrt1176` (teensy-cores, github) — loses `wm8962.{h,cpp}`; keeps `AudioStream.{h,cpp}`.
- **Fork** `~/Development/Audio` (newdigate/Audio, github) — `control_wm8962.{h,cpp}` gains the consolidated driver.
- **Gates** `~/Development/rt1170/evkb/{i2s_audio_test,audioinput_i2s_test,audiooutput_i2s_test}` (local).
- Commit to `master` in each repo. **Push only when the user asks** (Task 3). QEMU via `evkb/tools/qrun`.

## Source references (read these; the moves are verbatim)

- `cores/imxrt1176/wm8962.h` — `class WM8962Codec` (lines 5-14): `public: begin()`; `private: bus, addr, writeReg, readReg, modifyReg, pollSeqDone`. Plus `extern WM8962Codec Codec;` (line 15).
- `cores/imxrt1176/wm8962.cpp` — `WM8962Codec Codec;` global (line 2); `writeReg/readReg/modifyReg/pollSeqDone` (lines 4-36); the `INIT_PRE/INIT_POST/SEQ/VOLUME/ROUTE` static tables (lines 70-112); `begin()` (lines 114-159).
- `~/Development/Audio/control_wm8962.{h,cpp}` — `AudioControlWM8962` (methods only, no members); `enable()` calls the **global** `Codec.begin(Wire2, 0x1A)` then hand-rolls the ADDCTL3 write (control_wm8962.cpp lines 50-57).
- `evkb/i2s_audio_test/i2s_audio_test.cpp` — `#include "wm8962.h"` (line 4); `Codec.begin(Wire2)` (line 42, `STAGE_C`).

---

## Task 1: Decouple `i2s_audio_test` from the core codec (inline 48 kHz init)

Give the raw-`I2SClass` test its own self-contained WM8962 init so deleting the core `wm8962` (Task 2) doesn't break it. Use an **anonymous namespace** (internal linkage) so it does NOT collide with the core `WM8962Codec` that the core glob still compiles into this gate during Task 1.

**Files:**
- Modify: `evkb/i2s_audio_test/i2s_audio_test.cpp`

- [ ] **Step 1: Add the inline codec copy.** In `i2s_audio_test.cpp`, delete `#include "wm8962.h"` (line 4). Immediately after the existing includes, add an anonymous-namespace copy of the codec driver — **transcribed verbatim** from `cores/imxrt1176/wm8962.{h,cpp}** (the class body from `wm8962.h:5-14` plus the method bodies + static tables from `wm8962.cpp:4-159`), but as a self-contained unit with **no** `extern`/global `Codec`:

```cpp
namespace {
// Self-contained 48 kHz WM8962 init, copied from the (about-to-move) core
// WM8962Codec so this raw-I2SClass test stays independent of the Audio library.
// MUST be the FULL HW-verified sequence -- a reduced subset is silent on HW
// (see the rt1176-i2s-sai note). Anonymous namespace => no clash with the core
// WM8962Codec still compiled via the core glob during this task.
class LocalWM8962 {
public:
    bool begin(TwoWire &b, uint8_t a = 0x1A);
private:
    TwoWire *bus; uint8_t addr;
    bool writeReg(uint16_t reg, uint16_t val);
    bool readReg(uint16_t reg, uint16_t *val);
    bool modifyReg(uint16_t reg, uint16_t mask, uint16_t val);
    bool pollSeqDone();
};
bool LocalWM8962::writeReg(uint16_t reg, uint16_t val) {
    bus->beginTransmission(addr);
    bus->write((uint8_t)0x00); bus->write((uint8_t)reg);
    bus->write((uint8_t)(val >> 8)); bus->write((uint8_t)(val & 0xFF));
    return bus->endTransmission() == 0;
}
bool LocalWM8962::readReg(uint16_t reg, uint16_t *val) {
    bus->beginTransmission(addr);
    bus->write((uint8_t)0x00); bus->write((uint8_t)reg);
    if (bus->endTransmission(false) != 0) return false;
    if (bus->requestFrom(addr, (uint8_t)2) != 2) return false;
    uint8_t hi = bus->read(), lo = bus->read();
    *val = ((uint16_t)hi << 8) | lo; return true;
}
bool LocalWM8962::modifyReg(uint16_t reg, uint16_t mask, uint16_t val) {
    uint16_t regVal; if (!readReg(reg, &regVal)) return false;
    regVal &= (uint16_t)~mask; regVal |= val; return writeReg(reg, regVal);
}
bool LocalWM8962::pollSeqDone() {
    for (int i = 0; i < 100000; i++) {
        uint16_t s; if (!readReg(0x5D, &s)) return false;
        if ((s & 0x1) == 0) return true;
    }
    return false;
}
const uint16_t INIT_PRE[][2]  = { {0x0F,0x6243}, {0x81,0x0000} };
const uint16_t INIT_POST[][2] = { {0x08,0x09E4}, {0x19,0x01FE}, {0x1A,0x01E0} };
const uint16_t SEQ[] = {0x80, 0x92, 0xE8};
const uint16_t VOLUME[][2] = {
    {0x15,0x01C0},{0x16,0x01C0},{0x0A,0x01C0},{0x0B,0x01C0},{0x28,0x01FF},
    {0x29,0x01FF},{0x00,0x013F},{0x01,0x013F},{0x02,0x016B},{0x03,0x016B} };
const uint16_t ROUTE[][2] = {
    {0x25,0x0018},{0x26,0x0012},{0x22,0x0009},{0x69,0x0000},{0x6A,0x0000},
    {0x64,0x0000},{0x65,0x0000},{0x1F,0x0003} };
bool LocalWM8962::begin(TwoWire &b, uint8_t a) {
    bus = &b; addr = a;
    for (auto &e : INIT_PRE) if (!writeReg(e[0], e[1])) return false;
    if (!modifyReg(0x9B, 0x0001, 0x0000)) return false;
    for (auto &e : INIT_POST) if (!writeReg(e[0], e[1])) return false;
    for (uint16_t id : SEQ) {
        if (!writeReg(0x57, 0x0020)) return false;
        if (!writeReg(0x5A, id))     return false;
        if (!pollSeqDone())          return false;
    }
    if (!modifyReg(0x04, 0x0600, 0x0000)) return false;
    if (!modifyReg(0x08, 0x0020, 0x0020)) return false;
    for (auto &e : ROUTE) if (!writeReg(e[0], e[1])) return false;
    if (!modifyReg(0x07, 0x0013, 0x0002)) return false;
    for (auto &e : VOLUME) if (!writeReg(e[0], e[1])) return false;
    if (!modifyReg(0x07, 0x000C, 0x0000)) return false;
    if (!writeReg(0x1B, 0x0010)) return false;   // ADDCTL3: 48 kHz
    if (!writeReg(0x38, 0x000A)) return false;   // CLK4: ratio 512
    return true;
}
} // anonymous namespace
```

- [ ] **Step 2: Call the local init.** In `setup()`, replace `bool ok = Codec.begin(Wire2);` (line 42) with:

```cpp
    static LocalWM8962 localCodec;
    bool ok = localCodec.begin(Wire2);
```
(Leave the surrounding `Wire2.begin();` and the `STAGE_C_PASS/FAIL` print unchanged.)

- [ ] **Step 3: Build.**

Run: `cd ~/Development/rt1170/evkb/i2s_audio_test && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build`
Expected: links `i2s_audio_test.elf`, no errors (the core `wm8962.cpp` is still compiled via the glob but now unused by this TU — that's fine).

- [ ] **Step 4: Run the QEMU gate — verify no regression.**

Run: `./run_qemu_i2s.sh 2>&1 | tail -8` (or the gate's run script)
Expected: `STAGE_A_PASS`, `STAGE_B_DONE`, and the host-side tap assertion PASS **exactly as before this task** — the I2S-TX tap (the load-bearing check) is unaffected. `STAGE_C` prints whatever it printed before (the WM8962 I2C is unmodeled in QEMU; the inline init issues the identical I2C writes, so `STAGE_C`'s pass/fail is unchanged from the pre-task baseline — capture that baseline first with the current binary if unsure).

- [ ] **Step 5: Commit.**

```bash
cd ~/Development/rt1170/evkb
git add i2s_audio_test/i2s_audio_test.cpp
git commit -m "i2s_audio_test: inline self-contained 48kHz WM8962 init (drop core wm8962 dep)"
```

---

## Task 2: Consolidate `WM8962Codec` into the fork + delete the core copy (atomic)

Move the driver into `control_wm8962.{h,cpp}` (writeReg public, held as a member), de-dup `enable()`'s ADDCTL3 write, and delete the core files — all in one task so no gate ever sees two `WM8962Codec` definitions or a dangling `#include "wm8962.h"`.

**Files:**
- Modify: `~/Development/Audio/control_wm8962.h`
- Modify: `~/Development/Audio/control_wm8962.cpp`
- Delete: `cores/imxrt1176/wm8962.h`, `cores/imxrt1176/wm8962.cpp`

- [ ] **Step 1: Rewrite `control_wm8962.h`** — self-contained, both classes, `writeReg` public, `AudioControlWM8962` holds a `WM8962Codec` member. Replace the header body (keep the license banner) so the includes + declarations are:

```cpp
#ifndef control_wm8962_h_
#define control_wm8962_h_

#include <stdint.h>
#include "Wire.h"
#include "AudioControl.h"

// Low-level WM8962 I2C driver (moved from cores/imxrt1176/wm8962.h). 48k/16-bit
// I2S slave, HP out. writeReg is public so AudioControlWM8962 (and future nodes)
// can issue single register writes (e.g. the 44.1 kHz ADDCTL3 override).
class WM8962Codec {
public:
    bool begin(TwoWire &bus, uint8_t addr = 0x1A);
    bool writeReg(uint16_t reg, uint16_t val);   // now public
private:
    TwoWire *bus; uint8_t addr;
    bool readReg(uint16_t reg, uint16_t *val);
    bool modifyReg(uint16_t reg, uint16_t mask, uint16_t val);
    bool pollSeqDone();
};

// AudioControl wrapper around WM8962Codec for the audio graph. enable() runs the
// full record+playback init at 44.1 kHz (mic on Input3 / right channel).
class AudioControlWM8962 : public AudioControl
{
public:
    bool enable(void);
    bool disable(void) { return true; }
    bool volume(float n) { return true; }
    bool inputLevel(float n) { return true; }
    bool inputSelect(int n) { return true; }
private:
    WM8962Codec codec;
};

#endif
```

- [ ] **Step 2: Move the driver bodies into `control_wm8962.cpp` + de-dup `enable()`.** In `control_wm8962.cpp`: drop `#include "wm8962.h"`; keep `#include <Arduino.h>`, `#include "control_wm8962.h"`, `#include "Wire.h"` and the `WM8962_I2C_ADDR`/`WM8962_ADDCTL3`/`WM8962_ADDCTL3_44100HZ` defines. Paste the `WM8962Codec` method implementations + the `INIT_PRE/INIT_POST/SEQ/VOLUME/ROUTE` static tables + `begin()` **verbatim from `cores/imxrt1176/wm8962.cpp` lines 4-159** (i.e. everything EXCEPT the `#include "wm8962.h"` line 1 and the `WM8962Codec Codec;` global on line 2 — the global is dropped; the class name stays `WM8962Codec`). Then replace `enable()` with:

```cpp
bool AudioControlWM8962::enable(void)
{
	Wire2.begin();
	if (!codec.begin(Wire2, WM8962_I2C_ADDR)) {
		return false; // no WM8962 responding at 0x1A
	}
	// Override the 48 kHz ADDCTL3 that begin() wrote with the 44.1 kHz code,
	// via the now-public register-write accessor (no hand-rolled I2C duplication).
	if (!codec.writeReg(WM8962_ADDCTL3, WM8962_ADDCTL3_44100HZ)) {
		return false;
	}
	return true;
}
```

- [ ] **Step 3: Delete the core copy.**

```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
git rm wm8962.h wm8962.cpp
```

- [ ] **Step 4: Confirm the gate CMakes don't name the core `wm8962`.** The audio gates compile the core via glob (so the deleted file simply drops out) + the fork's `control_wm8962.cpp` explicitly. Verify none of the three gate `CMakeLists.txt` reference `wm8962.cpp` by name:

Run: `grep -rn 'wm8962' ~/Development/rt1170/evkb/{i2s_audio_test,audioinput_i2s_test,audiooutput_i2s_test}/CMakeLists.txt || echo "clean"`
Expected: `clean` (the audio gates reference `control_wm8962.cpp`, not `wm8962.cpp`).

- [ ] **Step 5: Rebuild + run BOTH audio gates.**

Run: `cd ~/Development/rt1170/evkb/audioinput_i2s_test && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build && ./run_qemu_audioinput.sh 2>&1 | tail -5`
Expected: `STAGE_PEAK=PASS`, `AUDIOINPUT_ALL=PASS` (unchanged).

Run: `cd ~/Development/rt1170/evkb/audiooutput_i2s_test && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build && ./run_qemu_audiooutput.sh 2>&1 | tail -8`
Expected: `STAGE_SYNTH=PASS`, `tap_peak=16383`, `STAGE_TONE=PASS`, `AUDIOOUTPUT_ALL=PASS` (unchanged).

- [ ] **Step 6: Also rebuild `i2s_audio_test`** (its build now excludes the core `wm8962` from the glob) to confirm the deletion didn't break it:

Run: `cd ~/Development/rt1170/evkb/i2s_audio_test && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build && ./run_qemu_i2s.sh 2>&1 | tail -5`
Expected: builds clean; gate PASSes as in Task 1 Step 4.

- [ ] **Step 7: Commit (two repos).**

```bash
cd ~/Development/Audio
git add control_wm8962.h control_wm8962.cpp
git commit -m "control_wm8962: consolidate WM8962Codec into the node (writeReg public, dedup ADDCTL3)"
cd ~/Development/rt1170/evkb/cores/imxrt1176
git commit -m "wm8962: remove core codec driver (moved to newdigate/Audio control_wm8962)"
```

---

## Task 3: Memory notes + push

**Files:**
- Modify: memory notes under `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170/memory/`

- [ ] **Step 1: Update the memory notes** that place `WM8962Codec` in the core or call the `writeReg`-public de-dup a *deferred core change* — it's now DONE and the driver lives in the fork. Edit:
  - `rt1176-audioinput-i2s.md` and `rt1176-audiooutput-i2s.md` — the "Deferred (Minor): make `WM8962Codec::writeReg` public" lines → note it's done (writeReg public, ADDCTL3 de-duped, driver consolidated into the fork's `control_wm8962`).
  - `rt1176-i2s-sai.md` — where it references the core `WM8962Codec` / `wm8962.cpp`, add that the driver moved to `newdigate/Audio control_wm8962.{h,cpp}` (the 48 kHz sequence is also inline-copied in `i2s_audio_test`).
  - Add a short new note `rt1176-wm8962-consolidation.md` (+ a `MEMORY.md` pointer) recording: `WM8962Codec` moved core→fork into a consolidated `control_wm8962.{h,cpp}` (writeReg public, held as a member, ADDCTL3 override de-duped); `i2s_audio_test` keeps an independent anonymous-namespace 48 kHz copy; `AudioStream` stays in the core; byte-identical I2C, all 3 QEMU gates green.

- [ ] **Step 2: Push when the user asks.** Confirm the unpushed sets, then on the user's go push both:
```bash
cd ~/Development/Audio && git log --oneline origin/master..master   # the consolidation commit
cd ~/Development/rt1170/evkb/cores/imxrt1176 && git log --oneline origin/master..master  # the wm8962 deletion (+ any prior unpushed)
```
On go: `git push origin master` in **both** `~/Development/Audio` (newdigate/Audio) and the cores tree (newdigate/teensy-cores). `evkb` stays local. Report the pushed ranges.

---

## Self-review notes (author)

- **Spec coverage:** move WM8962Codec→fork consolidated file → Task 2; writeReg public + ADDCTL3 de-dup → Task 2 Steps 1-2; delete core wm8962 → Task 2 Step 3; i2s_audio_test inline 48k init → Task 1; audio gates unaffected + regression via QEMU → Task 2 Step 5; memory + push → Task 3. AudioStream untouched (not in any task). All covered.
- **Ordering correctness:** Task 1 removes i2s_audio_test's core-`wm8962` dependency (anon-namespace copy, no name clash with the still-compiled core class) BEFORE Task 2 deletes it; Task 2 is atomic (add-to-fork + delete-core together) so no gate sees a double definition or a dangling include. Verified against the double-definition hazard.
- **Type consistency:** `WM8962Codec` (fork), `LocalWM8962` (i2s_audio_test anon copy — distinct name on purpose), `AudioControlWM8962::codec` member, `writeReg(uint16_t,uint16_t)`, `WM8962_ADDCTL3`/`WM8962_ADDCTL3_44100HZ` used consistently. The de-dup call `codec.writeReg(WM8962_ADDCTL3, WM8962_ADDCTL3_44100HZ)` matches the public signature.
- **Byte-identical check:** `enable()` old vs new issues the same I2C write (subaddr 0x00/0x1B, data 0x00/0x00) — `writeReg` uses exactly that framing (wm8962.cpp:4-11). i2s_audio_test's `LocalWM8962` is a verbatim transcription. So QEMU gates are a sound regression check.
