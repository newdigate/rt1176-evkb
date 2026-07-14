# RT1176 USB device HID — Add Joystick to the composite — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a HID **Joystick** interface to the CDC + Keyboard + Mouse composite (`Joystick.button()/X()/…`), keeping the other classes working.

**Architecture:** Append a 5th interface (Joystick, iface 4, interrupt-IN EP7); port teensy4 `usb_joystick.c` verbatim (+ the RT1176 `DMAMEM` dTD fix). **No QEMU changes** — the Scope-B reactive `hid_in_mask` tap already captures any HID interrupt-IN, so EP7 is tapped for free. Core lands (Mouse/Keyboard/CDC gates stay green), then the joystick gate goes green immediately (validating the core + confirming the reactive-tap payoff), then hardware.

**Tech Stack:** C (Arduino/Teensyduino core, ARM GCC 10.2.1), CMake + Unix Makefiles, Python 3 socket driver, LinkServer. (No QEMU edits this cycle.)

**Spec:** `docs/superpowers/specs/2026-07-14-rt1176-usb-device-hid-joystick-design.md`

**Commits:** `git commit` runs **only on explicit request** — each "Commit" step stages the listed files and pauses. Repos: `git -C ~/Development/rt1170/evkb/cores` (→ github teensy-cores), `git -C ~/Development/rt1170/evkb` (local). No qemu2 changes this cycle. Stage only this task's files.

---

## Task 1: Core — add the Joystick interface

Verified by: compiles, **and** the Mouse + Keyboard + CDC gates still pass against the 5-interface composite.

**Files:** Modify `usb_desc.h`, `usb_desc.c`, `usb.c`, `usb_inst.cpp`, `Arduino.h`; Create `usb_joystick.c` (all under `cores/imxrt1176/`).

- [ ] **Step 1.1: `usb_desc.h` — defines**

Change: `PRODUCT_ID 0x0003 → 0x0004`, `NUM_ENDPOINTS 6 → 7`, `NUM_INTERFACE 4 → 5`, `CONFIG_DESC_SIZE 125 → 150`.
Add after the mouse defines:
```c
/* Joystick HID (12-byte report: 32 buttons + hat + 6 axes; no REPORT_ID) */
#define JOYSTICK_INTERFACE       4
#define JOYSTICK_ENDPOINT        7        /* interrupt IN (0x87) */
#define JOYSTICK_SIZE            12
#define JOYSTICK_INTERVAL        1
#define JOYSTICK_HID_DESC_OFFSET 134      /* 125 (end of mouse block) + 9 (joystick iface desc) */
```
Add after `ENDPOINT6_CONFIG`:
```c
#define ENDPOINT7_CONFIG  (ENDPOINT_RECEIVE_UNUSED | ENDPOINT_TRANSMIT_INTERRUPT) /* 0x00CC0002 */
```

- [ ] **Step 1.2: `usb_desc.c` — report descriptor, config append, list entries**

(a) Add `joystick_report_desc[]` **above** `config_descriptor` (verbatim teensy4 `usb_desc.c:296-339`, the `JOYSTICK_SIZE==12` variant, 85 bytes), guarded:
```c
#if defined(JOYSTICK_INTERFACE)
static uint8_t joystick_report_desc[] = {
    0x05,0x01, 0x09,0x04, 0xA1,0x01, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x20,
    0x05,0x09, 0x19,0x01, 0x29,0x20, 0x81,0x02, 0x15,0x00, 0x25,0x07, 0x35,0x00,
    0x46,0x3B,0x01, 0x75,0x04, 0x95,0x01, 0x65,0x14, 0x05,0x01, 0x09,0x39, 0x81,0x42,
    0x05,0x01, 0x09,0x01, 0xA1,0x00, 0x15,0x00, 0x26,0xFF,0x03, 0x75,0x0A, 0x95,0x04,
    0x09,0x30, 0x09,0x31, 0x09,0x32, 0x09,0x35, 0x81,0x02, 0xC0,
    0x15,0x00, 0x26,0xFF,0x03, 0x75,0x0A, 0x95,0x02, 0x09,0x36, 0x09,0x36, 0x81,0x02, 0xC0
};
#endif
```
(At execution, prefer extracting verbatim from teensy4 `usb_desc.c:296-339`; confirm 85 bytes.)

(b) Config header: `bNumInterfaces 4 → 5`.

(c) Append after the **mouse** endpoint line (`7, 5, MOUSE_ENDPOINT | 0x80, 0x03, MOUSE_SIZE, 0, MOUSE_INTERVAL`) — add a comma and:
```c
    ,
    /* joystick interface (9) -- NON-boot: subclass/protocol 0x00 */
    9, 4, JOYSTICK_INTERFACE, 0, 1, 0x03, 0x00, 0x00, 0,
    /* HID descriptor (9) */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22,
        LSB(sizeof(joystick_report_desc)), MSB(sizeof(joystick_report_desc)),
    /* joystick endpoint, EP7 IN (7) */
    7, 5, JOYSTICK_ENDPOINT | 0x80, 0x03, JOYSTICK_SIZE, 0, JOYSTICK_INTERVAL
```

(d) Add to `usb_descriptor_list[]` (after the mouse entries):
```c
#if defined(JOYSTICK_INTERFACE)
    {0x2200, JOYSTICK_INTERFACE, joystick_report_desc, sizeof(joystick_report_desc)},
    {0x2100, JOYSTICK_INTERFACE, config_descriptor + JOYSTICK_HID_DESC_OFFSET, 9},
#endif
```

- [ ] **Step 1.3: Create `usb_joystick.c` — verbatim port + 3 deltas**

```bash
cp ~/Development/rt1170/evkb/cores/teensy4/usb_joystick.c \
   ~/Development/rt1170/evkb/cores/imxrt1176/usb_joystick.c
```
Then:
- Line ~56: `static transfer_t tx_transfer[TX_NUM] …;` → prefix **`DMAMEM `**.
- Line ~36: comment out `#include "debug/printf.h"`.
- Line ~83: comment out the `printf("ERROR status …", …);` statement (it spans 2 lines; `usb_joystick.c` uses **space** indentation like `usb_mouse.c`).

Verify: `diff teensy4/usb_joystick.c imxrt1176/usb_joystick.c` shows **only** those 3 deltas.

- [ ] **Step 1.4: `usb.c` — SET_CONFIGURATION arms EP7 + configure**

After the `#if defined(ENDPOINT6_CONFIG) … #endif` block add:
```c
		#if defined(ENDPOINT7_CONFIG)
		USB1_ENDPTCTRL7 = ENDPOINT7_CONFIG;
		#endif
```
After the `#if defined(MOUSE_INTERFACE) usb_mouse_configure(); #endif` block add:
```c
		#if defined(JOYSTICK_INTERFACE)
		usb_joystick_configure();
		#endif
```
And in the extern block (next to the mouse extern) add:
```c
#if defined(JOYSTICK_INTERFACE)
void usb_joystick_configure(void);
#endif
```

- [ ] **Step 1.5: `usb_inst.cpp` — the `Joystick` global + static member**

Append (both lines — the class has a static `manual_mode` needing a definition, else a linker error):
```cpp
#if defined(JOYSTICK_INTERFACE)
usb_joystick_class Joystick;
uint8_t usb_joystick_class::manual_mode = 0;
#endif
```

- [ ] **Step 1.6: `Arduino.h` — surface `Joystick`**

After the `#include "usb_mouse.h"` line add:
```c
#include "usb_joystick.h"   // Joystick (guarded by JOYSTICK_INTERFACE)
```

- [ ] **Step 1.7: Build (fresh — new core .c)**

```bash
cd ~/Development/rt1170/evkb/usb_mouse_test
rm -rf build
cmake -B build -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchain/rt1170-evkb.toolchain.cmake" .
cmake --build build 2>&1 | tail -20
```
Expected: links with no errors. A linker `undefined reference to usb_joystick_class::manual_mode` means Step 1.5's static-member definition is missing.

- [ ] **Step 1.8: Regression — Mouse + Keyboard + CDC gates against the 5-iface composite**

```bash
cd ~/Development/rt1170/evkb/usb_mouse_test && ./run_qemu_usb_mouse.sh 2>&1 | tail -2
cd ~/Development/rt1170/evkb/usb_keyboard_test && ./run_qemu_usb_keyboard.sh 2>&1 | tail -1
cd ~/Development/rt1170/evkb/usb_data_test && ./run_qemu_usb_data.sh 2>&1 | tail -1
```
Expected: all three **PASS**. (The composite grew by one interface; the reactive tap records EP7 in the mask but the gates send only their own class.)

- [ ] **Step 1.9: Commit (checkpoint — on request)**

```bash
git -C ~/Development/rt1170/evkb/cores add \
  imxrt1176/usb_desc.h imxrt1176/usb_desc.c imxrt1176/usb_joystick.c \
  imxrt1176/usb.c imxrt1176/usb_inst.cpp imxrt1176/Arduino.h
git -C ~/Development/rt1170/evkb/cores commit -m "feat(usb): add HID joystick interface to the composite"
```

---

## Task 2: Joystick QEMU gate — straight to GREEN

**No RED phase this cycle:** the Scope-B reactive `hid_in_mask` tap already captures any HID interrupt-IN, so once Task 1 presents the joystick on EP7, the gate passes. The gate therefore validates the *core* work (descriptor + EP7 arming + report packing) and confirms the reactive-tap generalization pays off. (If it's RED, it's a Task-1 bug — debug the core, e.g. EP7 arming or the report bytes.)

**Files:** Create `usb_joystick_test/{CMakeLists.txt, usb_joystick_test.cpp, usb_joystick_driver.py, run_qemu_usb_joystick.sh, toolchain/rt1170-evkb.toolchain.cmake}`.

- [ ] **Step 2.1: Scaffold**

```bash
mkdir -p ~/Development/rt1170/evkb/usb_joystick_test/toolchain
cp ~/Development/rt1170/evkb/usb_mouse_test/toolchain/rt1170-evkb.toolchain.cmake \
   ~/Development/rt1170/evkb/usb_joystick_test/toolchain/
```

- [ ] **Step 2.2: `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(usb_joystick_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)
import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)
teensy_add_executable(usb_joystick_test usb_joystick_test.cpp)
teensy_target_link_libraries(usb_joystick_test cores)
target_link_libraries(usb_joystick_test.elf stdc++)
```

- [ ] **Step 2.3: `usb_joystick_test.cpp`**

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "usb_joystick.h"  // Joystick
#include "usb_dev.h"       // usb_configuration

extern volatile uint8_t usb_configuration;

void setup() {
    Serial1.begin(115200);
    uint32_t t0 = millis();
    while (usb_configuration == 0 && (millis() - t0) < 3000) { /* wait for host */ }
    Serial1.println(usb_configuration ? "USB=CONFIGURED" : "USB=TIMEOUT");
    delay(100);
    Joystick.useManualSend(true);   // one combined report, not one per setter
    Joystick.button(1, 1);          // data[0] = 0x00000001
    Joystick.X(512);                // data[1] = 512<<4 = 0x00002000
    Joystick.send_now();            // report: 01 00 00 00 00 20 00 00 00 00 00 00
    Serial1.println("JOY=SENT");
}

void loop() { }
```

- [ ] **Step 2.4: `usb_joystick_driver.py`**

```python
#!/usr/bin/env python3
"""Read the QEMU usbhid-tap socket: capture one 12-byte HID joystick report.
Assert button1 + X(512) => 01 00 00 00 00 20 00 00 00 00 00 00.
Exit 0 on match, 1 on mismatch/short, 2 on connection failure."""
import socket, sys, time

host, port = sys.argv[1], int(sys.argv[2])
sock = None
deadline = time.time() + 10
while time.time() < deadline:
    try:
        sock = socket.create_connection((host, port), timeout=1); break
    except OSError:
        time.sleep(0.2)
if sock is None:
    print("ERROR: could not connect to usbhid-tap socket"); sys.exit(2)

sock.settimeout(8)
got = b""
try:
    while len(got) < 12:
        chunk = sock.recv(64)
        if not chunk: break
        got += chunk
except socket.timeout:
    pass

print("got=%r (%d bytes)" % (got, len(got)))
want = bytes([0x01,0x00,0x00,0x00, 0x00,0x20,0x00,0x00, 0x00,0x00,0x00,0x00])
sys.exit(0 if got[:12] == want else 1)
```

- [ ] **Step 2.5: `run_qemu_usb_joystick.sh`** (copy the mouse runner; keep the `set +e/-e` wrap; PORT 14558, `id=usbhid-tap`, ELF `usb_joystick_test.elf`, driver `usb_joystick_driver.py`, PASS line "PASS: USB joystick HID report verified"). Then `chmod +x`.

- [ ] **Step 2.6: Build the gate**

```bash
cd ~/Development/rt1170/evkb/usb_joystick_test
rm -rf build
cmake -B build -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchain/rt1170-evkb.toolchain.cmake" .
cmake --build build 2>&1 | tail -12
```
Expected: `usb_joystick_test.elf` links.

- [ ] **Step 2.7: Run — expect GREEN**

```bash
cd ~/Development/rt1170/evkb/usb_joystick_test && ./run_qemu_usb_joystick.sh; echo "exit=$?"
```
Expected: **`PASS: USB joystick HID report verified`** (`exit=0`); driver `got=b'\x01\x00\x00\x00\x00 \x00\x00\x00\x00\x00\x00' (12 bytes)`; CI-HID shows `hid int-IN mask=0xe0` (EP5|EP6|EP7). If RED, debug Task 1 (EP7 arming, `usb_joystick_configure`, or the report packing).

- [ ] **Step 2.8: Regressions**

```bash
cd ~/Development/rt1170/evkb/usb_mouse_test && ./run_qemu_usb_mouse.sh 2>&1 | tail -1
cd ~/Development/rt1170/evkb/usb_keyboard_test && ./run_qemu_usb_keyboard.sh 2>&1 | tail -1
cd ~/Development/rt1170/evkb/usb_data_test && ./run_qemu_usb_data.sh 2>&1 | tail -1
```
Expected: all **PASS**.

- [ ] **Step 2.9: Commit (checkpoint — on request)**

```bash
git -C ~/Development/rt1170/evkb add usb_joystick_test/CMakeLists.txt \
  usb_joystick_test/usb_joystick_test.cpp usb_joystick_test/usb_joystick_driver.py \
  usb_joystick_test/run_qemu_usb_joystick.sh usb_joystick_test/toolchain
git -C ~/Development/rt1170/evkb commit -m "test(usb): joystick HID gate (GREEN via the Scope-B reactive tap)"
```

---

## Task 3: Hardware verification

- [ ] **Step 3.1: Flash** — `pkill -9 -f LinkServer; pkill -9 -f redlinkserv; sleep 1`, then `LinkServer run MIMXRT1176:MIMXRT1170-EVKB ~/Development/rt1170/evkb/usb_joystick_test/build/usb_joystick_test.elf`. (Recurring `DAPInfo … transfer error` → ask the user to replug J11.)

- [ ] **Step 3.2: Confirm enumeration** — `hidutil list | grep 0x1209` shows a **Joystick** (UsagePage 1 / **Usage 4**) alongside Mouse (Usage 2) + Keyboard (Usage 6); `system_profiler SPUSBDataType | grep -A6 "USB Serial"` shows PID **0x0004**; CDC node present.

- [ ] **Step 3.3: Confirm a report is observed** — build+flash a periodic HW variant (`usb_joystick_hw.cpp`, mirror `usb_mouse_hw`: toggle `Joystick.button(1,·)` / sweep `Joystick.X()` every ~1 s, + CDC echo). Observe in macOS (a HID monitor, or the Game Controller framework / a browser gamepad test page). (Reflash the quiet one-shot after.)

- [ ] **Step 3.4: Confirm the other classes still work** — `hidutil list` still shows Mouse + Keyboard; pyserial echo on the native-USB CDC node round-trips.

- [ ] **Step 3.5: Record + commit** — write `usb_joystick_test/HW-RESULTS.md`; `git -C ~/Development/rt1170/evkb add usb_joystick_test/HW-RESULTS.md && … commit`.

---

## Self-review

**Spec coverage:** §4.1 descriptor → Task 1.1-1.2 ✓ · §4.2 EP0 → 1.4 ✓ · §4.3 backend+globals (incl. manual_mode) → 1.3/1.5/1.6 ✓ · §4.4 QEMU unchanged → (no task, verified by regressions) ✓ · §4.5 gate + regressions → Task 2 ✓ · §4.6 HW → Task 3 ✓.

**Placeholder scan:** none; verbatim bodies use `cp`/verbatim-copy with explicit deltas; the report-desc bytes are shown (85, cross-checked).

**Type/name consistency:** `JOYSTICK_INTERFACE 4`/`JOYSTICK_ENDPOINT 7`/`JOYSTICK_SIZE 12`, `usb_joystick_configure`, `usb_joystick_class::manual_mode`, PID `0x0004`, PORT `14558`, report `01 00 00 00 00 20 00 00 00 00 00 00`, ELF `usb_joystick_test.elf` — consistent across tasks.
