# RT1176 EventResponder (minimal restore) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore the RT1176 core's gutted `EventResponder` to its yield-deferred + immediate paths so async peripherals (SPI DMA next) can hand a callback to run safely at `yield()` time or immediately.

**Architecture:** Adapt the kept-verbatim Teensy4 `EventResponder.h` down to the minimal surface, implement the empty `EventResponder.cpp` (yield-list only), and wire `EventResponder::runFromYield()` into the existing `yield()`. Verify with a pure-firmware QEMU gate (no device-model work) then a hardware smoke test.

**Tech Stack:** C++ (Arduino/Teensyduino core), imxrt1176 core, QEMU `mimxrt1170-evk` machine, CMake cross-build, LinkServer flash + pyserial VCOM.

**Reference (verbatim source of truth):** `~/.platformio/packages/framework-arduinoteensy/cores/teensy4/EventResponder.{h,cpp}`.

**Spec:** `evkb/docs/superpowers/specs/2026-07-06-rt1176-eventresponder-design.md`. This is **sub-project A** of the SPI-DMA-TX effort; SPI full-duplex DMA async transfer (the consumer) is a later cycle.

**Repo layout:** `cores/imxrt1176/` is a nested **teensy-cores** git repo (github `origin/master`). `evkb/` is local-only (no push). Commit to `master` in each. Push only when the user asks.

---

## Task 1: Restore the EventResponder core (`.h` + `.cpp` + `yield.cpp`)

Tightly-coupled unit — these three files compile/link together, so they are one task.

**Files:**
- Modify: `cores/imxrt1176/EventResponder.h` (adapt the kept verbatim header)
- Modify: `cores/imxrt1176/EventResponder.cpp` (replace the empty Phase-0 stub)
- Modify: `cores/imxrt1176/yield.cpp` (wire in the dispatch)

- [ ] **Step 1: Header — drop the `yield_active_check_flags` line from `attach()`**

In `cores/imxrt1176/EventResponder.h`, `attach()` currently reads:
```cpp
	void attach(EventResponderFunction function, uint8_t priority __attribute__((unused)) = 128) {
		bool irq = disableInterrupts();
		detachNoInterrupts();
		_function = function;
		_type = EventTypeYield;
		yield_active_check_flags |= YIELD_CHECK_EVENT_RESPONDER; // user setup a yield type...
		enableInterrupts(irq);
	}
```
Delete the `yield_active_check_flags |= …` line (that optimization symbol doesn't exist in this core; our `yield()` always calls `runFromYield()`, which early-returns when the list is empty). Result:
```cpp
	void attach(EventResponderFunction function, uint8_t priority __attribute__((unused)) = 128) {
		bool irq = disableInterrupts();
		detachNoInterrupts();
		_function = function;
		_type = EventTypeYield;
		enableInterrupts(irq);
	}
```

- [ ] **Step 2: Header — make `attachInterrupt()` fall back to `attachImmediate()`**

Replace the whole `attachInterrupt()` body (currently sets `EventTypeInterrupt`, `SCB_SHPR3`, `_VectorsRam[15]`) with a fallback:
```cpp
	// No PendSV software interrupt on this core: fall back to immediate, which
	// preserves the "prompt response" contract (runs in the caller's context).
	void attachInterrupt(EventResponderFunction function, uint8_t priority __attribute__((unused)) = 128) {
		attachImmediate(function);
	}
```

- [ ] **Step 3: Header — remove the machinery that minimal-scope drops**

Delete these from `EventResponder.h`:
- The extern line `extern "C" void systick_isr_with_timer_events(void);` (near the top, above `class EventResponder`).
- The two `waitForEvent(...)` method declarations.
- The `static void runFromInterrupt();` declaration.
- The two static members `static EventResponder *firstInterrupt;` and `static EventResponder *lastInterrupt;`.
- The entire `class MillisTimer { … };` (from `class MillisTimer` through its closing `};`).

Keep everything else verbatim: `attach`, `attachImmediate`, `attachThread` (alias of `attach`), `triggerEvent`, `clearEvent`, `runFromYield` (inline), `getStatus`, `getData`, `setContext`, `getContext`, `operator bool`, the protected members (`_status/_function/_data/_context/_next/_prev/_type/_triggered`), `firstYield`, `lastYield`, `runningFromYield`, and the private `disableInterrupts`/`enableInterrupts` helpers.

- [ ] **Step 4: Implement `EventResponder.cpp`**

Replace the entire body of `cores/imxrt1176/EventResponder.cpp` (currently an empty Phase-0 stub after the license header) with the yield-only implementation, adapted verbatim from the Teensy4 `.cpp`:
```cpp
#include <Arduino.h>
#include "EventResponder.h"

EventResponder * EventResponder::firstYield = nullptr;
EventResponder * EventResponder::lastYield = nullptr;
bool EventResponder::runningFromYield = false;

void EventResponder::triggerEventNotImmediate()
{
	bool irq = disableInterrupts();
	if (_triggered == false) {
		if (_type == EventTypeYield) {
			if (firstYield == nullptr) {
				_next = nullptr;
				_prev = nullptr;
				firstYield = this;
				lastYield = this;
			} else {
				_next = nullptr;
				_prev = lastYield;
				_prev->_next = this;
				lastYield = this;
			}
		}
		_triggered = true;
	}
	enableInterrupts(irq);
}

bool EventResponder::clearEvent()
{
	bool ret = false;
	bool irq = disableInterrupts();
	if (_triggered) {
		if (_type == EventTypeYield) {
			if (_prev) {
				_prev->_next = _next;
			} else {
				firstYield = _next;
			}
			if (_next) {
				_next->_prev = _prev;
			} else {
				lastYield = _prev;
			}
		}
		_triggered = false;
		ret = true;
	}
	enableInterrupts(irq);
	return ret;
}

// this detach must be called with interrupts disabled
void EventResponder::detachNoInterrupts()
{
	if (_type == EventTypeYield) {
		if (_triggered) {
			if (_prev) {
				_prev->_next = _next;
			} else {
				firstYield = _next;
			}
			if (_next) {
				_next->_prev = _prev;
			} else {
				lastYield = _prev;
			}
		}
		_type = EventTypeDetached;
	}
}
```
(Everything else from the Teensy `.cpp` — the `EventTypeInterrupt` branches, `pendablesrvreq_isr`, `runFromInterrupt`, `MillisTimer`, `systick_isr` — is intentionally omitted per the minimal scope.)

- [ ] **Step 5: Wire `runFromYield()` into `yield.cpp`**

`cores/imxrt1176/yield.cpp` currently is:
```cpp
#include "HardwareSerial.h"

extern void serialEvent1(void) __attribute__((weak));

extern "C" void yield(void) {
    static uint8_t running = 0;
    if (running) return;            // guard against re-entrancy (delay()->yield()->serialEvent1()->delay()...)
    running = 1;
    if (&serialEvent1 && Serial1.available()) serialEvent1();
    running = 0;
}
```
Add the EventResponder include and dispatch call:
```cpp
#include "HardwareSerial.h"
#include "EventResponder.h"

extern void serialEvent1(void) __attribute__((weak));

extern "C" void yield(void) {
    static uint8_t running = 0;
    if (running) return;            // guard against re-entrancy (delay()->yield()->serialEvent1()->delay()...)
    running = 1;
    if (&serialEvent1 && Serial1.available()) serialEvent1();
    EventResponder::runFromYield();
    running = 0;
}
```

- [ ] **Step 6: Verify the core compiles + links (rebuild an existing gate)**

The core is compiled per-gate. Rebuild the SPI gate (which now links the restored `EventResponder.cpp` + the modified `yield.cpp`) to confirm no compile/link errors:
```bash
cd ~/Development/rt1170/evkb/spi_loopback_test && rm -rf build \
  && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . >/dev/null \
  && cmake --build build 2>&1 | tail -3
```
Expected: builds to `spi_loopback_test.elf` with no errors (no "undefined reference to `EventResponder::firstYield`" etc.). Host-analysis LSP noise (`Arduino.h not found`, unknown `int16_t`) is expected and irrelevant — only the cross-compile matters.

- [ ] **Step 7: Commit (cores / teensy-cores repo)**

```bash
cd ~/Development/rt1170/evkb/cores
git add imxrt1176/EventResponder.h imxrt1176/EventResponder.cpp imxrt1176/yield.cpp
git commit -m "EventResponder: restore yield-deferred + immediate paths (minimal)

Implement the Phase-0-gutted EventResponder.cpp (yield list only) and trim
the kept-verbatim header to the minimal surface: attachInterrupt falls back
to attachImmediate, MillisTimer/PendSV/waitForEvent dropped. Wire
EventResponder::runFromYield() into yield().

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: EventResponder QEMU gate (`evkb/eventresponder_test/`)

Pure firmware — no QEMU device-model work; runs on the existing `mimxrt1170-evk` machine.

**Files:**
- Create: `evkb/eventresponder_test/eventresponder_test.cpp`
- Create: `evkb/eventresponder_test/CMakeLists.txt` (copy/adapt from `spi_loopback_test`)
- Create: `evkb/eventresponder_test/run_qemu_er.sh` (copy/adapt from `spi_loopback_test/run_qemu_spi.sh`)
- Create: `evkb/eventresponder_test/toolchain/` (copy from `spi_loopback_test/toolchain/`)

- [ ] **Step 1: Copy the gate scaffolding**

```bash
mkdir -p ~/Development/rt1170/evkb/eventresponder_test
cp -R ~/Development/rt1170/evkb/spi_loopback_test/toolchain ~/Development/rt1170/evkb/eventresponder_test/
cp ~/Development/rt1170/evkb/spi_loopback_test/CMakeLists.txt ~/Development/rt1170/evkb/eventresponder_test/
cp ~/Development/rt1170/evkb/spi_loopback_test/run_qemu_spi.sh ~/Development/rt1170/evkb/eventresponder_test/run_qemu_er.sh
```
Then in `eventresponder_test/CMakeLists.txt` change the project/target/elf name from `spi_loopback_test` to `eventresponder_test` (edit the `project(...)`, `add_executable(...)`, and any `spi_loopback_test`-named references so the built ELF is `eventresponder_test.elf`; the file(GLOB) of the core sources stays as-is).

- [ ] **Step 2: Write the firmware test**

Create `evkb/eventresponder_test/eventresponder_test.cpp`:
```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "EventResponder.h"

static EventResponder er;
static volatile int fired = 0;
static volatile int seen_status = -1;
static volatile void *seen_data = nullptr;

static void cb(EventResponderRef e) {
    fired++;
    seen_status = e.getStatus();
    seen_data = e.getData();
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    bool ok = true;

    // STAGE_IMMEDIATE: attachImmediate → callback runs synchronously in triggerEvent()
    fired = 0;
    er.attachImmediate(cb);
    er.triggerEvent();
    bool s_imm = (fired == 1);                 // fired before any yield()
    Serial1.println(s_imm ? "STAGE_IMMEDIATE=PASS" : "STAGE_IMMEDIATE=FAIL");
    if (!s_imm) ok = false;
    er.detach();

    // STAGE_YIELD: attach → deferred; not fired until yield()
    fired = 0;
    er.attach(cb);
    er.triggerEvent();
    bool before = (fired == 0);                // not yet
    yield();
    bool after = (fired == 1);                 // now
    bool s_yield = before && after;
    Serial1.println(s_yield ? "STAGE_YIELD=PASS" : "STAGE_YIELD=FAIL");
    if (!s_yield) ok = false;
    er.detach();

    // STAGE_CLEAR: attach + trigger + clearEvent + yield → callback does NOT run
    fired = 0;
    er.attach(cb);
    er.triggerEvent();
    er.clearEvent();
    yield();
    bool s_clear = (fired == 0);
    Serial1.println(s_clear ? "STAGE_CLEAR=PASS" : "STAGE_CLEAR=FAIL");
    if (!s_clear) ok = false;
    er.detach();

    // STAGE_STATUS: triggerEvent(42, &marker) → callback sees status + data
    static int marker = 7;
    fired = 0; seen_status = -1; seen_data = nullptr;
    er.attach(cb);
    er.triggerEvent(42, &marker);
    yield();
    bool s_status = (fired == 1) && (seen_status == 42) && (seen_data == &marker);
    Serial1.println(s_status ? "STAGE_STATUS=PASS" : "STAGE_STATUS=FAIL");
    if (!s_status) ok = false;
    er.detach();

    Serial1.println(ok ? "EVENTRESPONDER_ALL=PASS" : "EVENTRESPONDER_ALL=FAIL");
}
void loop() {}
```

- [ ] **Step 3: Adapt the run script**

In `eventresponder_test/run_qemu_er.sh`, point it at `eventresponder_test.elf` (not the SPI elf), and replace the SPI-specific grep(s) with the EventResponder markers. The check block should be:
```sh
grep -q "STAGE_IMMEDIATE=PASS" "$VCOM" || { echo "FAIL: immediate"; exit 1; }
grep -q "STAGE_YIELD=PASS"     "$VCOM" || { echo "FAIL: yield";     exit 1; }
grep -q "STAGE_CLEAR=PASS"     "$VCOM" || { echo "FAIL: clear";     exit 1; }
grep -q "STAGE_STATUS=PASS"    "$VCOM" || { echo "FAIL: status";    exit 1; }
grep -q "EVENTRESPONDER_ALL=PASS" "$VCOM" || { echo "FAIL: overall"; exit 1; }
echo "PASS: EVENTRESPONDER_ALL"
```
Keep the rest of the harness (the `qrun`/gtimeout invocation, VCOM capture path, machine flags) identical to `run_qemu_spi.sh` — this gate needs no special QEMU device or flags.

- [ ] **Step 4: Build + run the gate**

```bash
cd ~/Development/rt1170/evkb/eventresponder_test && rm -rf build \
  && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . >/dev/null \
  && cmake --build build 2>&1 | tail -2 && ./run_qemu_er.sh 2>&1 | tail -10
```
Expected VCOM: `STAGE_IMMEDIATE=PASS`, `STAGE_YIELD=PASS`, `STAGE_CLEAR=PASS`, `STAGE_STATUS=PASS`, `EVENTRESPONDER_ALL=PASS`, then `PASS: EVENTRESPONDER_ALL`. If `STAGE_YIELD` fails (callback fired before `yield()` or never fired), the `runFromYield()` wiring in `yield.cpp` (Task 1 Step 5) is wrong — fix there, do not weaken the test.

- [ ] **Step 5: Commit (evkb repo)**

```bash
cd ~/Development/rt1170/evkb
git add eventresponder_test/eventresponder_test.cpp eventresponder_test/CMakeLists.txt \
        eventresponder_test/run_qemu_er.sh eventresponder_test/toolchain
git commit -m "eventresponder_test: QEMU gate (immediate/yield/clear/status) green

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: Hardware smoke test

Confirm the four stages PASS on silicon (the deferred callback really fires from the real main-loop `yield()`). The controller drives this — the board is connected; flash + VCOM are scriptable. Light: no code changes expected.

- [ ] **Step 1: Flash + capture**

```bash
LS=/Applications/LinkServer_26.6.137/LinkServer
PORT=/dev/cu.usbmodem5DQ2DDHVWO5EI3
OUT=/tmp/er_vcom.txt   # controller: use the session scratchpad, not /tmp
( gtimeout 20 python3 -c "
import serial,sys
s=serial.Serial('$PORT',115200,timeout=1)
while True:
    l=s.readline()
    if l: sys.stdout.write(l.decode('utf-8','replace')); sys.stdout.flush()
" > \"$OUT\" 2>&1 ) &
CAP=$!
sleep 2
gtimeout 90 "$LS" flash MIMXRT1176:MIMXRT1170-EVKB load ~/Development/rt1170/evkb/eventresponder_test/build/eventresponder_test.elf 2>&1 | grep -iE 'finish|loaded|error' | tail -2
wait $CAP
cat "$OUT"
```

- [ ] **Step 2: Verify on silicon**

Expected VCOM: `STAGE_IMMEDIATE=PASS`, `STAGE_YIELD=PASS`, `STAGE_CLEAR=PASS`, `STAGE_STATUS=PASS`, `EVENTRESPONDER_ALL=PASS`. `STAGE_YIELD=PASS` on hardware is the meaningful bit — it proves deferred dispatch fires from the real `yield()`. If it fails on silicon but passed in QEMU, investigate (e.g. the main loop / `yield()` path on hardware) before declaring done. No commit (verification only).

---

## Task 4: Memory note + push

**Files:**
- Create: `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170/memory/rt1176-eventresponder.md`
- Modify: memory `MEMORY.md`

- [ ] **Step 1: Write the memory note**

Capture: EventResponder was Phase-0-gutted; restored **minimal** (yield-deferred `attach` + `attachImmediate`; `attachInterrupt`→`attachImmediate`; `triggerEvent`/`clearEvent`/`runFromYield`); MillisTimer/PendSV/waitForEvent **dropped** (MillisTimer needs a reliable 1 ms tick the RT1176 SysTick can't give — `millis()` is DWT-based; see `delay.c`). Wired `runFromYield()` into `yield.cpp` (after `serialEvent1`); dispatches **one event per `yield()` call`; guarded by `runningFromYield` + `ipsr`. Header is now "restored, adapted" (no longer byte-verbatim Teensy). Gate `evkb/eventresponder_test/` = pure firmware (no QEMU model). Built as sub-project A for **SPI full-duplex DMA async transfer** (sub-project B). Link `[[rt1176-lpspi-spi]]`, `[[rt1176-edma-dmachannel]]`, `[[rt1176-intervaltimer-pit]]`. Add a one-line `MEMORY.md` pointer.

- [ ] **Step 2: Push (only when the user asks)**

```bash
cd ~/Development/rt1170/evkb/cores && git push origin master 2>&1 | tail -2   # teensy-cores
# evkb is local-only — nothing to push
```

---

## Self-Review

**Spec coverage:** §1 files/changes → Task 1 (.h/.cpp/yield) + Task 2 (gate). §2 API surface → Task 1 header adapt. §3 data flow → Task 1 .cpp + yield wiring. §4 QEMU gate (4 stages) → Task 2. §5 HW verification → Task 3. Deferred items (MillisTimer/PendSV/waitForEvent) → explicitly removed in Task 1 Steps 2-3. Memory/push → Task 4. All covered.

**Placeholder scan:** No TBD/TODO; every code step shows complete code; the gate scaffolding steps (CMake/run script/toolchain) are copy-from-`spi_loopback_test` with the exact edits named. The `/tmp` in Task 3 is annotated to use the scratchpad.

**Type consistency:** `EventResponderRef` / `EventResponderFunction` / `attach` / `attachImmediate` / `attachInterrupt` / `triggerEvent(status,data)` / `clearEvent` / `getStatus` / `getData` / `runFromYield` / `firstYield` / `lastYield` / `runningFromYield` / `triggerEventNotImmediate` / `detachNoInterrupts` used consistently across the header, the .cpp, the yield wiring, and the gate. The gate uses only the kept public methods.
