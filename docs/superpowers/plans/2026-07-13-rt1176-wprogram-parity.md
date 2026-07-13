# RT1176 WProgram.h Include-Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `cores/imxrt1176/WProgram.h` a byte-identical mirror of `cores/teensy4/WProgram.h`, porting every header it pulls in, so stock Teensy 4 sketches/libraries compile unmodified.

**Architecture:** Port leaf headers first (verbatim `cp` where platform-independent, adapted where they touch 1176-specific hardware), then swap in teensy4's WProgram.h verbatim as the integration point. A QEMU gate written first (red) drives the whole pass; HW flash at the end per project discipline.

**Tech Stack:** CMake + teensy-cmake-macros, qemu2 `mimxrt1170-evk` via `tools/qrun` + `tools/gate-lib.sh`, LinkServer for HW flash.

**Spec:** `docs/superpowers/specs/2026-07-13-rt1176-wprogram-parity-design.md`

---

## Context for a zero-context engineer

- **Two nested git repos, shared working tree across sessions.** Gate tests + docs live in `~/Development/rt1170/evkb` (local-only repo; commit with `git -C ~/Development/rt1170/evkb`). Core sources live in `~/Development/rt1170/evkb/cores` (separate repo, github `teensy-cores`; commit with `git -C ~/Development/rt1170/evkb/cores`). Run `git -C <repo> status -sb` before committing — another session may share the tree.
- **`cores/teensy4/` is the read-only reference tree** (upstream Teensy 4 core). `cores/imxrt1176/` is the RT1176 core being built. "Port" means copy from teensy4 and adapt only where silicon differs.
- **Gate convention:** each gate is a top-level dir in `evkb/` with `CMakeLists.txt`, `<name>.cpp`, `run_qemu_<name>.sh`, `toolchain/rt1170-evkb.toolchain.cmake` (copied from `dac_test/`), and later `HW-RESULTS.md`. Configure with the toolchain file, build, run the script, grep `=OK` markers.
- **QEMU timing:** any gate using `delay()`/IntervalTimer needs `-icount shift=auto` (couples DWT-based delay() and the PIT), per `interval_timer_test/run_qemu_intervaltimer.sh`.
- **Build system picks up new core files automatically** — sources are `GLOB_RECURSE`d (`teensy-cmake-macros/CMakeLists.include.txt:193`). No CMake edits for new `.cpp` files. Globs run at configure time; every task below reconfigures from scratch, so this never goes stale.
- **License note (user policy: no new copyleft):** teensy4's `WCharacter.h` and `WMath.cpp` are LGPL-2.1 (Wiring/Arduino lineage). Per user decision (2026-07-13), Tasks 2–3 write **clean-room MIT** versions from the documented Arduino API instead of copying — do NOT open or copy from `teensy4/WCharacter.h` / `teensy4/WMath.cpp` beyond confirming the public signatures already quoted in this plan. Everything else ported here is PJRC MIT-style; `inplace_function.h` is SG14 (permissive — verify its embedded header on copy). Pre-existing `WString.h` stays LGPL (out of scope).

---

### Task 1: QEMU gate, red (evkb repo)

**Files:**
- Create: `wprogram_parity_test/CMakeLists.txt`
- Create: `wprogram_parity_test/wprogram_parity_test.cpp`
- Create: `wprogram_parity_test/run_qemu_wprogram_parity.sh`
- Copy: `wprogram_parity_test/toolchain/rt1170-evkb.toolchain.cmake` (from `dac_test/toolchain/`)

- [ ] **Step 1.1: Create the gate directory and copy the toolchain**

```bash
cd ~/Development/rt1170/evkb
mkdir -p wprogram_parity_test/toolchain
cp dac_test/toolchain/rt1170-evkb.toolchain.cmake wprogram_parity_test/toolchain/
```

- [ ] **Step 1.2: Write `wprogram_parity_test/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(wprogram_parity_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)
# Stock, unmodified Bounce2 (MIT) — the "stock Teensy library compiles" smoke test.
import_arduino_library(Bounce2 $ENV{HOME}/Development/Bounce2/src)

teensy_add_executable(wprogram_parity_test wprogram_parity_test.cpp)
teensy_target_link_libraries(wprogram_parity_test Bounce2 cores)

target_link_libraries(wprogram_parity_test.elf stdc++)
```

- [ ] **Step 1.3: Write `wprogram_parity_test/wprogram_parity_test.cpp`**

```cpp
// QEMU/HW gate for the WProgram.h include-parity pass.
// The point: ONLY <Arduino.h> from the core — everything exercised below must
// arrive transitively, exactly as on a stock Teensy 4 build.
#include <Arduino.h>
#include <Bounce2.h>   // stock library smoke test (unmodified, MIT)

// Negative checks: CDC-only descriptors must keep every usb_* gate closed.
#ifdef KEYBOARD_INTERFACE
#error "KEYBOARD_INTERFACE leaked into a CDC-only build"
#endif
#ifdef MIDI_INTERFACE
#error "MIDI_INTERFACE leaked into a CDC-only build"
#endif
#ifdef MTP_INTERFACE
#error "MTP_INTERFACE leaked into a CDC-only build"
#endif

static volatile uint32_t itimer_ticks;
static IntervalTimer itimer;   // no #include "IntervalTimer.h" — the NativeEthernet regression
static elapsedMillis emillis;  // ditto elapsedMillis.h

static void itimer_isr() { itimer_ticks++; }

static void check(bool ok, const char *tag)
{
	Serial1.print(tag);
	Serial1.println(ok ? "=OK" : "=FAIL");
}

void setup()
{
	Serial1.begin(115200);
	Serial1.println("WPROGRAM PARITY GATE");

	// WCharacter.h
	check(isAlpha('A') && !isAlpha('1') && isDigit('7') && toUpperCase('a') == 'A', "WCHAR");

	// WString via Arduino.h (worked before this pass; kept as a regression check)
	String s("hello");
	s += " world";
	s.toUpperCase();
	check(s == "HELLO WORLD" && s.length() == 11, "STRING");

	// WMath.cpp: makeWord + reproducible avr-libc PRNG
	check(makeWord(0x12, 0x34) == 0x1234, "WORD");
	randomSeed(42);
	long r1 = random(100);
	randomSeed(42);
	long r2 = random(100);
	long r3 = random(50, 60);
	check(r1 == r2 && r1 >= 0 && r1 < 100 && r3 >= 50 && r3 < 60, "RAND");

	// elapsedMillis
	emillis = 0;
	delay(25);
	check(emillis >= 20 && emillis <= 100, "EMILLIS");

	// IntervalTimer via Arduino.h alone
	itimer_ticks = 0;
	itimer.begin(itimer_isr, 1000); // 1 kHz
	delay(50);
	itimer.end();
	check(itimer_ticks >= 35 && itimer_ticks <= 70, "ITIMER");

	// pulseIn: quiet-pin timeout path (valid in QEMU and on HW — D4 is
	// undriven at this point, so a jumpered D5 still reads a quiet LOW)
	pinMode(5, INPUT_PULLDOWN);
	check(pulseIn(5, HIGH, 20000) == 0, "PULSE_TIMEOUT");

	// pulseIn: real measurement — meaningful on HW only (jumper D4 <-> D5).
	// In QEMU the unjumpered input reads 0, so this prints PULSE_HW=0; the
	// gate script does not assert on it. HW-RESULTS.md records ~500us
	// (half-period of the 1 kHz tone), accepted range 400..600.
	tone(4, 1000);
	delay(10);
	uint32_t width = pulseIn(5, HIGH, 50000);
	noTone(4);
	Serial1.print("PULSE_HW=");
	Serial1.println(width);

	// CrashReport stub
	check(!CrashReport, "CRASHREPORT_BOOL");
	Serial1.print(CrashReport);          // Print::print(const Printable&)
	CrashReport.clear();                 // no-op, must link
	CrashReportClass::breadcrumb(1, 0x1176); // no-op, must link

	// Stock library smoke: Bounce2, unmodified
	Bounce b;
	b.attach(6, INPUT_PULLUP);
	b.interval(5);
	b.update();
	check(true, "BOUNCE");

	// usb_serial.h surface via Arduino.h (Serial == USB CDC)
	Serial.begin(9600);
	check(true, "USBSERIAL");

	Serial1.println("GATE=DONE");
}

void loop() {}
```

- [ ] **Step 1.4: Write `wprogram_parity_test/run_qemu_wprogram_parity.sh`** (then `chmod +x` it)

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/wprogram_parity_test.elf"; OUT="$DIR/wp.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/wp.dbg" &
P=$!; gate_pid $P; sleep 20; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
for k in WCHAR STRING WORD RAND EMILLIS ITIMER PULSE_TIMEOUT CRASHREPORT_BOOL BOUNCE USBSERIAL; do
  grep -q "$k=OK" "$OUT" || { echo "FAIL: $k"; exit 1; }
done
grep -q "not yet supported on IMXRT1176" "$OUT" || { echo "FAIL: CrashReport stub print"; exit 1; }
grep -q "GATE=DONE" "$OUT" || { echo "FAIL: completion"; exit 1; }
echo "PASS: WProgram.h include-parity gate"
```

- [ ] **Step 1.5: Configure + build, verify it FAILS (red)**

```bash
cd ~/Development/rt1170/evkb/wprogram_parity_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build
```

Expected: **compile FAILURE** — at minimum `isAlpha`/`toUpperCase` undeclared (no WCharacter.h) and `CrashReport` undeclared. If it compiles, the gate is not testing anything — stop and fix the sketch.

- [ ] **Step 1.6: Commit (evkb repo)**

```bash
git -C ~/Development/rt1170/evkb add wprogram_parity_test
git -C ~/Development/rt1170/evkb commit -m "test: WProgram.h include-parity QEMU gate (red - trimmed WProgram)"
```

---

### Task 2: Leaf headers — WCharacter.h (clean-room), inplace_function.h + avr/interrupt.h (cp) (cores repo)

**Files:**
- Create: `cores/imxrt1176/WCharacter.h` (CLEAN-ROOM MIT — do not copy from teensy4)
- Create: `cores/imxrt1176/inplace_function.h` (cp from teensy4 — SG14, permissive)
- Create: `cores/imxrt1176/avr/interrupt.h` (cp from teensy4 — PJRC)

- [ ] **Step 2.1: Copy the two permissive headers verbatim**

```bash
cd ~/Development/rt1170/evkb/cores
cp teensy4/inplace_function.h imxrt1176/inplace_function.h
cp teensy4/avr/interrupt.h imxrt1176/avr/interrupt.h
diff teensy4/inplace_function.h imxrt1176/inplace_function.h && \
diff teensy4/avr/interrupt.h imxrt1176/avr/interrupt.h && echo COPIES-OK
```

Expected: `COPIES-OK`.

- [ ] **Step 2.2: Check `inplace_function.h`'s embedded license header is permissive**

```bash
head -30 ~/Development/rt1170/evkb/cores/imxrt1176/inplace_function.h
```

Expected: SG14/Boost-style permissive text. If it shows GPL/LGPL, STOP and surface to the user (per license policy).

- [ ] **Step 2.3: Write `cores/imxrt1176/WCharacter.h` — clean-room MIT**

Written from the documented Arduino character API (ctype wrappers). Do NOT read or copy `teensy4/WCharacter.h` (it is LGPL). Use this content exactly:

```cpp
/* WCharacter.h - Arduino character classification / conversion API.
 *
 * Clean-room MIT implementation for the imxrt1176 core: written from the
 * documented Arduino API surface (thin wrappers over <ctype.h>), not derived
 * from the LGPL Wiring/Arduino WCharacter.h.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <ctype.h>

/* ctype.h functions have undefined behaviour for arguments outside
 * unsigned char / EOF; the Arduino API takes plain (possibly signed) chars,
 * so every wrapper casts through unsigned char first. */

static inline bool isAlpha(int c)             { return isalpha((unsigned char)c) != 0; }
static inline bool isAlphaNumeric(int c)      { return isalnum((unsigned char)c) != 0; }
static inline bool isAscii(int c)             { return c >= 0 && c <= 127; }
static inline bool isControl(int c)           { return iscntrl((unsigned char)c) != 0; }
static inline bool isDigit(int c)             { return isdigit((unsigned char)c) != 0; }
static inline bool isGraph(int c)             { return isgraph((unsigned char)c) != 0; }
static inline bool isHexadecimalDigit(int c)  { return isxdigit((unsigned char)c) != 0; }
static inline bool isLowerCase(int c)         { return islower((unsigned char)c) != 0; }
static inline bool isPrintable(int c)         { return isprint((unsigned char)c) != 0; }
static inline bool isPunct(int c)             { return ispunct((unsigned char)c) != 0; }
static inline bool isSpace(int c)             { return isspace((unsigned char)c) != 0; }
static inline bool isUpperCase(int c)         { return isupper((unsigned char)c) != 0; }
static inline bool isWhitespace(int c)        { return c == ' ' || c == '\t'; }

static inline int toAscii(int c)              { return c & 0x7f; }
static inline int toLowerCase(int c)          { return tolower((unsigned char)c); }
static inline int toUpperCase(int c)          { return toupper((unsigned char)c); }
```

- [ ] **Step 2.4: Commit (cores repo)**

```bash
git -C ~/Development/rt1170/evkb/cores add imxrt1176/WCharacter.h imxrt1176/inplace_function.h imxrt1176/avr/interrupt.h
git -C ~/Development/rt1170/evkb/cores commit -m "feat: WCharacter.h (clean-room MIT) + inplace_function.h, avr/interrupt.h from teensy4 (WProgram parity)"
```

---

### Task 3: WMath.cpp — clean-room MIT (cores repo)

**Files:**
- Create: `cores/imxrt1176/WMath.cpp` (CLEAN-ROOM MIT — do not copy from teensy4; teensy4's is LGPL)

This fixes a latent link error: `random()`/`randomSeed()`/`makeWord()` are declared in our WProgram.h today but implemented nowhere. The PRNG is xorshift32 (Marsaglia, "Xorshift RNGs", JSS 2003 — a public-domain algorithm), NOT the avr-libc Park–Miller code the LGPL original uses. The declared API (matching WProgram.h's prototypes, which come from the MIT PJRC WProgram.h): `void randomSeed(uint32_t)`, `void srandom(unsigned int)`, `int32_t random(void)`, `uint32_t random(uint32_t howbig)`, `int32_t random(int32_t howsmall, int32_t howbig)`, `uint16_t makeWord(uint16_t)`, `uint16_t makeWord(byte h, byte l)`.

- [ ] **Step 3.1: Write `cores/imxrt1176/WMath.cpp`**

Use the same MIT header block as WCharacter.h (Step 2.3), then:

```cpp
#include <stdint.h>

/* xorshift32 (Marsaglia 2003). State must never be zero; randomSeed(0) is
 * ignored (same contract as Arduino: a zero seed leaves the sequence alone). */
static uint32_t rng_state = 0x11760001u;

void randomSeed(uint32_t newseed)
{
	if (newseed != 0) rng_state = newseed;
}

void srandom(unsigned int newseed)
{
	if (newseed != 0) rng_state = newseed;
}

int32_t random(void)
{
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	/* Arduino contract: non-negative result. */
	return (int32_t)(x >> 1);
}

uint32_t random(uint32_t howbig)
{
	if (howbig == 0) return 0;
	return (uint32_t)random() % howbig;
}

int32_t random(int32_t howsmall, int32_t howbig)
{
	if (howsmall >= howbig) return howsmall;
	return (int32_t)random((uint32_t)(howbig - howsmall)) + howsmall;
}

uint16_t makeWord(uint16_t w)
{
	return w;
}

uint16_t makeWord(unsigned char h, unsigned char l)
{
	return (uint16_t)((h << 8) | l);
}
```

- [ ] **Step 3.2: Commit (cores repo)**

```bash
git -C ~/Development/rt1170/evkb/cores add imxrt1176/WMath.cpp
git -C ~/Development/rt1170/evkb/cores commit -m "feat: WMath.cpp clean-room MIT (xorshift32) - random/randomSeed/makeWord were declared but unimplemented"
```

---

### Task 4: pulseIn (cores repo)

**Files:**
- Modify: `cores/imxrt1176/digital.c` (append after the last function)
- Modify: `cores/imxrt1176/wiring.h` (add C declaration, mirroring `teensy4/wiring.h:221`)

`pulseIn` is declared in WProgram.h but unimplemented. The teensy4 version (`teensy4/digital.c:275-333`) walks `digital_pin_to_info_PGM` with raw `reg + 2` pointer offsets; our `digital.c` has its own pin table (`digital_pin_to_info[]`, entries `{gpio, bit, ...}`) and a `GPIO_PSR(base)` accessor — so this is an **adapted** port, same control flow, our table.

- [ ] **Step 4.1: Find the anchor points**

```bash
grep -n "digital_pin_info_struct\|GPIO_PSR\|CORE_NUM_DIGITAL" ~/Development/rt1170/evkb/cores/imxrt1176/digital.c | head
grep -n "extern \"C\"\|micros" ~/Development/rt1170/evkb/cores/imxrt1176/wiring.h | head
```

Confirm: the pin table is `digital_pin_to_info[]` of `struct digital_pin_info_struct`, `GPIO_PSR()` exists, and wiring.h has an `extern "C"` block declaring `micros()`.

- [ ] **Step 4.2: Append to `cores/imxrt1176/digital.c`**

```c
/* pulseIn: teensy4 digital.c port, adapted to our pin table (digital_pin_to_info
 * entries carry a GPIO base + bit instead of teensy4's raw reg pointers; the
 * PSR read replaces teensy4's *(p->reg + 2)). Same control flow otherwise. */
uint32_t pulseIn_high(uint8_t pin, uint32_t timeout)
{
	const struct digital_pin_info_struct *p = digital_pin_to_info + pin;
	const uint32_t mask = 1u << p->bit;
	uint32_t usec_start, usec_stop;

	// wait for any previous pulse to end
	usec_start = micros();
	while (GPIO_PSR(p->gpio) & mask) {
		if (micros() - usec_start > timeout) return 0;
	}
	// wait for the pulse to start
	usec_start = micros();
	while (!(GPIO_PSR(p->gpio) & mask)) {
		if (micros() - usec_start > timeout) return 0;
	}
	usec_start = micros();
	// wait for the pulse to stop
	while (GPIO_PSR(p->gpio) & mask) {
		if (micros() - usec_start > timeout) return 0;
	}
	usec_stop = micros();
	return usec_stop - usec_start;
}

uint32_t pulseIn_low(uint8_t pin, uint32_t timeout)
{
	const struct digital_pin_info_struct *p = digital_pin_to_info + pin;
	const uint32_t mask = 1u << p->bit;
	uint32_t usec_start, usec_stop;

	// wait for any previous pulse to end
	usec_start = micros();
	while (!(GPIO_PSR(p->gpio) & mask)) {
		if (micros() - usec_start > timeout) return 0;
	}
	// wait for the pulse to start
	usec_start = micros();
	while (GPIO_PSR(p->gpio) & mask) {
		if (micros() - usec_start > timeout) return 0;
	}
	usec_start = micros();
	// wait for the pulse to stop
	while (!(GPIO_PSR(p->gpio) & mask)) {
		if (micros() - usec_start > timeout) return 0;
	}
	usec_stop = micros();
	return usec_stop - usec_start;
}

uint32_t pulseIn(uint8_t pin, uint8_t state, uint32_t timeout)
{
	if (pin >= CORE_NUM_DIGITAL) return 0;
	if (state) return pulseIn_high(pin, timeout);
	return pulseIn_low(pin, timeout);
}
```

If Step 4.1 showed different struct/table/macro names, use those — the shape is what matters. If the input path needs the pin muxed first, note that `pulseIn` (like teensy4's) assumes the caller did `pinMode(pin, INPUT*)` — do not add mux code here.

- [ ] **Step 4.3: Declare in `cores/imxrt1176/wiring.h`, inside the `extern "C"` block, mirroring teensy4/wiring.h:221's position (next to the other timing/digital prototypes)**

```c
uint32_t pulseIn(uint8_t pin, uint8_t state, uint32_t timeout);
```

(The C-linkage declaration here is what lets WProgram.h's C++ redeclaration-with-default-argument bind to the C implementation — same mechanism as teensy4.)

- [ ] **Step 4.4: Compile-check via any existing gate (blink is fastest)**

```bash
cd ~/Development/rt1170/evkb/blink
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../dac_test/toolchain/rt1170-evkb.toolchain.cmake . 2>/dev/null || cmake -B build .
cmake --build build
```

Expected: PASS (pulseIn compiles; nothing calls it yet). If blink has its own toolchain conventions, use whatever its existing build dir used (`grep TOOLCHAIN blink/build/CMakeCache.txt`).

- [ ] **Step 4.5: Commit (cores repo)**

```bash
git -C ~/Development/rt1170/evkb/cores add imxrt1176/digital.c imxrt1176/wiring.h
git -C ~/Development/rt1170/evkb/cores commit -m "feat: pulseIn - teensy4 port adapted to the 1176 pin table (was declared, unimplemented)"
```

---

### Task 5: Self-gating usb_* headers + MTP_Teensy.h (cores repo)

**Files:**
- Create: `cores/imxrt1176/usb_seremu.h`, `usb_keyboard.h`, `usb_mouse.h`, `usb_joystick.h`, `usb_midi.h`, `usb_rawhid.h`, `usb_flightsim.h`, `usb_audio.h`, `usb_touch.h`, `MTP_Teensy.h` (all cp from teensy4)

Every one of these gates its entire body on a `*_INTERFACE` define from `usb_desc.h`; the only pre-gate includes are `usb_desc.h` (all) and `keylayouts.h` (usb_keyboard.h) — both already in our core. Our CDC-only `usb_desc.h` defines just `CDC_STATUS_INTERFACE`/`CDC_DATA_INTERFACE`, so all ten headers compile to nothing. `MTP_Storage.h` is NOT ported — its include sits inside `MTP_Teensy.h`'s `MTP_INTERFACE` gate.

- [ ] **Step 5.1: Copy all ten verbatim and verify**

```bash
cd ~/Development/rt1170/evkb/cores
for f in usb_seremu.h usb_keyboard.h usb_mouse.h usb_joystick.h usb_midi.h \
         usb_rawhid.h usb_flightsim.h usb_audio.h usb_touch.h MTP_Teensy.h; do
  cp teensy4/$f imxrt1176/$f
  diff teensy4/$f imxrt1176/$f || { echo "DIFF-FAIL: $f"; exit 1; }
done
echo COPIES-OK
```

- [ ] **Step 5.2: Standalone compile probe (headers must be inert under CDC-only descriptors)**

```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
cat > /tmp/usb_hdr_probe.cpp <<'EOF'
#include "usb_seremu.h"
#include "usb_keyboard.h"
#include "usb_mouse.h"
#include "usb_joystick.h"
#include "usb_midi.h"
#include "usb_rawhid.h"
#include "usb_flightsim.h"
#include "usb_audio.h"
#include "usb_touch.h"
#include "MTP_Teensy.h"
int main() { return 0; }
EOF
/Applications/ARM_10/bin/arm-none-eabi-g++ -c -I. -D__IMXRT1176__ -mcpu=cortex-m7 \
  -o /tmp/usb_hdr_probe.o /tmp/usb_hdr_probe.cpp && echo PROBE-OK
```

Expected: `PROBE-OK`. A failure means a header references something pre-gate that our `usb_desc.h` lacks — add the missing **inert define** to `cores/imxrt1176/usb_desc.h` (never edit the ported header).

- [ ] **Step 5.3: Commit (cores repo)**

```bash
git -C ~/Development/rt1170/evkb/cores add imxrt1176/usb_*.h imxrt1176/MTP_Teensy.h
git -C ~/Development/rt1170/evkb/cores commit -m "feat: self-gating usb_* + MTP_Teensy headers verbatim from teensy4 - inert under CDC-only descriptors"
```

---

### Task 6: CrashReport stub (cores repo)

**Files:**
- Create: `cores/imxrt1176/CrashReport.h` (adapted — teensy4's inline `breadcrumb()` writes to a hard-coded RT1062 DTCM row `0x2027FFC0` that does not exist on our FlexRAM layout)
- Create: `cores/imxrt1176/CrashReport.cpp` (stub, NOT the teensy4 one)

- [ ] **Step 6.1: Write `cores/imxrt1176/CrashReport.h`**

Keep teensy4's PJRC license header (copy lines 1–29 of `teensy4/CrashReport.h`), then:

```cpp
#pragma once

#include <Printable.h>
#include <WString.h>

// RT1176 stub: API parity with teensy4 CrashReport, no fault-handler capture
// yet. operator bool() is always false — the same "no crash report available"
// state a cleanly-booted Teensy reports — so `if (CrashReport) ...` behaves
// correctly and simply never fires. breadcrumb() is a no-op: the teensy4
// inline writes a hard-coded RT1062 DTCM cache row (0x2027FFC0) that has no
// equivalent on this FlexRAM layout.
// TODO(crashreport milestone): real implementation (fault handlers,
// breadcrumb RAM) is §3 sub-project 4 in the outstanding-work doc.
class CrashReportClass: public Printable {
public:
	virtual size_t printTo(Print& p) const;
	static void clear();
	operator bool();
	static void breadcrumb(unsigned int num, unsigned int value) {
		(void)num;
		(void)value;
	}
};

extern CrashReportClass CrashReport;
```

- [ ] **Step 6.2: Write `cores/imxrt1176/CrashReport.cpp`** (same PJRC license header)

```cpp
#include "CrashReport.h"
#include <Print.h>

size_t CrashReportClass::printTo(Print& p) const
{
	return p.println("CrashReport: not yet supported on IMXRT1176");
}

void CrashReportClass::clear()
{
}

CrashReportClass::operator bool()
{
	return false;
}

CrashReportClass CrashReport;
```

- [ ] **Step 6.3: Commit (cores repo)**

```bash
git -C ~/Development/rt1170/evkb/cores add imxrt1176/CrashReport.h imxrt1176/CrashReport.cpp
git -C ~/Development/rt1170/evkb/cores commit -m "feat: CrashReport API-parity stub - always-false bool, no-op breadcrumbs, real capture deferred"
```

---

### Task 7: WProgram.h mirror + gate to green (cores repo)

**Files:**
- Overwrite: `cores/imxrt1176/WProgram.h` (byte-identical cp from teensy4 — this is the whole point)

- [ ] **Step 7.1: Copy and prove byte-identity**

```bash
cd ~/Development/rt1170/evkb/cores
cp teensy4/WProgram.h imxrt1176/WProgram.h
diff teensy4/WProgram.h imxrt1176/WProgram.h && echo MIRROR-OK
```

Expected: `MIRROR-OK` (empty diff). From now on this diff IS the parity audit.

- [ ] **Step 7.2: Full rebuild of the gate**

```bash
cd ~/Development/rt1170/evkb/wprogram_parity_test
rm -rf build
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build
```

Expected: clean compile + link. **Failure playbook (from the spec — keep WProgram.h byte-identical):**
- Include cycle / missing decl → add the missing include or guard to the *offending header* in `imxrt1176/`, never reorder WProgram.h.
- `*_INTERFACE`-adjacent macro missing → add inert define to `imxrt1176/usb_desc.h`.
- `avr_emulation.h` breakage (it exists in our tree but was never included until now) → smallest fix inside that header, `#if defined(__IMXRT1176__)` guards acceptable there.
- Duplicate-symbol on `pulseIn`/`random` → check WMath.cpp/digital.c weren't double-globbed (they shouldn't be; `GLOB_RECURSE` sees each file once).

- [ ] **Step 7.3: Run the QEMU gate**

```bash
cd ~/Development/rt1170/evkb/wprogram_parity_test
./run_qemu_wprogram_parity.sh
```

Expected: every marker `=OK`, the CrashReport stub line, `GATE=DONE`, and final `PASS: WProgram.h include-parity gate`. `PULSE_HW=0` is expected in QEMU (input pins read 0 in the model; the real measurement is Task 8).

- [ ] **Step 7.4: Regression-run two neighbour gates** (include-surface changes can break other sketches)

```bash
cd ~/Development/rt1170/evkb/interval_timer_test && ./run_qemu_intervaltimer.sh
cd ~/Development/rt1170/evkb/dac_test && ./run_qemu_dac.sh
```

Expected: both still PASS. (If their `build/` dirs are stale against the new core, reconfigure the same way as in their CMakeCache.)

- [ ] **Step 7.5: Commit (cores repo), then the gate-green marker commit (evkb repo)**

```bash
git -C ~/Development/rt1170/evkb/cores add imxrt1176/WProgram.h
# plus any playbook fixes from 7.2 (usb_desc.h / avr_emulation.h / headers):
git -C ~/Development/rt1170/evkb/cores status -sb   # review before adding extras
git -C ~/Development/rt1170/evkb/cores commit -m "feat: WProgram.h byte-identical teensy4 mirror - include-surface parity"
git -C ~/Development/rt1170/evkb add wprogram_parity_test
git -C ~/Development/rt1170/evkb commit -m "test: WProgram parity gate green in QEMU (icount; Bounce2 stock-lib smoke)"
```

---

### Task 8: HW verification (evkb repo)

**Files:**
- Create: `wprogram_parity_test/HW-RESULTS.md`

**Needs the user:** a physical jumper between Arduino-header **D4 and D5** (D4 = GPIO_AD_06, D5 = GPIO_AD_05 — see `cores/imxrt1176/digital.c` pin table) for the `PULSE_HW` measurement. Ask before flashing; everything else passes without the jumper except `PULSE_HW`.

- [ ] **Step 8.1: Flash + capture.** Start the pyserial reader FIRST (`cat` resets the baud rate — never use it), then flash. `rt1170-flash.sh` uses LinkServer (`MIMXRT1176:MIMXRT1170-EVKB`, `--erase-all`) and defaults the VCOM port; it opens its own console after flashing, so when capturing to a file use the reader + flash separately:

```bash
pkill LinkServer; pkill redlinkserv   # always before each flash
gtimeout 60 /usr/local/Caskroom/miniconda/base/bin/python3 \
  ~/Development/rt1170/rt1170-console.py > ~/Development/rt1170/evkb/wprogram_parity_test/wp-hw.uart 2>&1 &
~/Development/rt1170/rt1170-flash.sh ~/Development/rt1170/evkb/wprogram_parity_test/build/wprogram_parity_test.elf
# press SW4/RESET if output doesn't start after flashing, then:
cat ~/Development/rt1170/evkb/wprogram_parity_test/wp-hw.uart
```

(The trailing `cat` only reads the capture file — reading a *file* is fine; it's `cat` on the tty device that resets baud.)

- [ ] **Step 8.2: Check the captured output**

Same markers as QEMU, plus: `PULSE_HW=<n>` with n in **400..600** (half-period of the 1 kHz tone through the D4→D5 jumper).

- [ ] **Step 8.3: Write `wprogram_parity_test/HW-RESULTS.md`**

Record: date, ELF built from which cores commit, full captured UART output, PULSE_HW value, jumper setup, any anomalies. Follow the structure of `dac_test/HW-RESULTS.md`.

- [ ] **Step 8.4: Commit (evkb repo)**

```bash
git -C ~/Development/rt1170/evkb add wprogram_parity_test/HW-RESULTS.md
git -C ~/Development/rt1170/evkb commit -m "docs: WProgram parity HW-verified (all markers OK, pulseIn ~500us via D4-D5 jumper)"
```

---

## Deviations from the spec (agreed during planning/execution)

- **Clean-room MIT WCharacter.h + WMath.cpp** (user decision 2026-07-13, during execution): teensy4's versions are LGPL Wiring lineage; Tasks 2–3 write fresh MIT implementations from the documented Arduino API instead of porting. Spec updated to match.

- **Metro** is not on disk and **Encoder** (considered as a substitute) doesn't compile for RT1176 for reasons *outside* this pass's scope — its `utility/direct_pin_read.h` needs the `portOutputRegister`/`digitalPinToBitMask` fast-pin macro family, keyed on `__IMXRT1062__`. That's a separate §3-adjacent parity gap worth its own note. The stock-library smoke is therefore **Bounce2 only**.
- Spec's "negative compile check" is implemented as `#error` tripwires in the gate sketch (fails the build if any `*_INTERFACE` leaks), which is stronger than a runtime `#ifdef` probe.
