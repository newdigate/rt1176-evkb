# RT1176 USB device HID — Add Mouse to the composite — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a HID **Mouse** interface to the existing CDC + Keyboard composite (`Mouse.move()` etc.), keeping Keyboard and CDC working.

**Architecture:** Append a 4th interface (Mouse, iface 3, interrupt-IN EP6) to the composite descriptor; port teensy4 `usb_mouse.c` verbatim (+ the RT1176 `DMAMEM` dTD fix). In QEMU, generalize the single-HID-endpoint tap to **reactively tap any HID interrupt-IN** (a `hid_in_mask`), so keyboard EP5 and mouse EP6 are both captured. Gate-first: core lands (Keyboard + CDC gates stay green), a mouse gate goes RED against the single-endpoint QEMU, then the reactive tap turns it GREEN, then hardware.

**Tech Stack:** C (Arduino/Teensyduino core, ARM GCC 10.2.1), C (QEMU device model), CMake + Unix Makefiles, Python 3 socket driver, LinkServer.

**Spec:** `docs/superpowers/specs/2026-07-14-rt1176-usb-device-hid-mouse-design.md`

**Commits:** `git commit` runs **only on explicit request** — each "Commit" step is a checkpoint that stages the listed files and pauses. Repos: `git -C ~/Development/rt1170/evkb/cores` (→ github teensy-cores), `git -C ~/Development/qemu2` (→ gitlab qemu-rt1170), `git -C ~/Development/rt1170/evkb` (local). Stage only this task's files; never the pre-existing `evkb` WIP.

---

## Task 1: Core — add the Mouse interface

The `MOUSE_INTERFACE` guard activates all mouse code at once. Verified by: compiles, **and** the Keyboard gate + CDC gate still pass against the 4-interface composite (the current single-endpoint QEMU still taps the first HID int-IN = keyboard EP5, so both regressions hold).

**Files:** Modify `usb_desc.h`, `usb_desc.c`, `usb.c`, `usb_inst.cpp`, `Arduino.h`; Create `usb_mouse.c` (all under `cores/imxrt1176/`).

- [ ] **Step 1.1: `usb_desc.h` — defines**

Change: `PRODUCT_ID 0x0002 → 0x0003`, `NUM_ENDPOINTS 5 → 6`, `NUM_INTERFACE 3 → 4`, `CONFIG_DESC_SIZE 100 → 125`.
Add after the keyboard defines:
```c
/* Mouse HID (relative report-ID 1 + absolute report-ID 2; NON-boot) */
#define MOUSE_INTERFACE       3
#define MOUSE_ENDPOINT        6        /* interrupt IN (0x86) */
#define MOUSE_SIZE            8
#define MOUSE_INTERVAL        2
#define MOUSE_HID_DESC_OFFSET 109      /* 100 (end of keyboard block) + 9 (mouse iface desc) */
```
Add after `ENDPOINT5_CONFIG`:
```c
#define ENDPOINT6_CONFIG  (ENDPOINT_RECEIVE_UNUSED | ENDPOINT_TRANSMIT_INTERRUPT) /* 0x00CC0002 */
```

- [ ] **Step 1.2: `usb_desc.c` — mouse report descriptor, config append, list entries**

(a) Add `mouse_report_desc[]` **above** `config_descriptor` (verbatim teensy4 `usb_desc.c:248`, 84 bytes), guarded:
```c
#if defined(MOUSE_INTERFACE)
static uint8_t mouse_report_desc[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x85,0x01, 0x05,0x09,
    0x19,0x01, 0x29,0x08, 0x15,0x00, 0x25,0x01, 0x95,0x08,
    0x75,0x01, 0x81,0x02, 0x05,0x01, 0x09,0x30, 0x09,0x31,
    0x09,0x38, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x03,
    0x81,0x06, 0x05,0x0C, 0x0A,0x38,0x02, 0x15,0x81, 0x25,0x7F,
    0x75,0x08, 0x95,0x01, 0x81,0x06, 0xC0, 0x05,0x01, 0x09,0x02,
    0xA1,0x01, 0x85,0x02, 0x05,0x01, 0x09,0x30, 0x09,0x31,
    0x15,0x00, 0x26,0xFF,0x7F, 0x75,0x10, 0x95,0x02, 0x81,0x02, 0xC0
};
#endif
```
(At execution, prefer extracting it verbatim from teensy4 to avoid transcription error, then confirm 84 bytes.)

(b) Config header: `bNumInterfaces 3 → 4` (the `…, 4, 1, 0, 0xC0, 50,` byte); total-length bytes already use `LSB/MSB(CONFIG_DESC_SIZE)` so they track 125 automatically.

(c) Append after the **keyboard** endpoint line (the last bytes of `config_descriptor`, `7, 5, KEYBOARD_ENDPOINT | 0x80, 0x03, KEYBOARD_SIZE, 0, KEYBOARD_INTERVAL`) — add a comma and:
```c
    ,
    /* mouse interface (9) — NON-boot: subclass/protocol 0x00 */
    9, 4, MOUSE_INTERFACE, 0, 1, 0x03, 0x00, 0x00, 0,
    /* HID descriptor (9) */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22,
        LSB(sizeof(mouse_report_desc)), MSB(sizeof(mouse_report_desc)),
    /* mouse endpoint, EP6 IN (7) */
    7, 5, MOUSE_ENDPOINT | 0x80, 0x03, MOUSE_SIZE, 0, MOUSE_INTERVAL
```

(d) Add to `usb_descriptor_list[]` (after the keyboard entries):
```c
#if defined(MOUSE_INTERFACE)
    {0x2200, MOUSE_INTERFACE, mouse_report_desc, sizeof(mouse_report_desc)},
    {0x2100, MOUSE_INTERFACE, config_descriptor + MOUSE_HID_DESC_OFFSET, 9},
#endif
```

- [ ] **Step 1.3: Create `usb_mouse.c` — verbatim port + 3 deltas**

```bash
cp ~/Development/rt1170/evkb/cores/teensy4/usb_mouse.c \
   ~/Development/rt1170/evkb/cores/imxrt1176/usb_mouse.c
```
Then, exactly as for `usb_keyboard.c`:
- Line ~99: `static transfer_t tx_transfer[TX_NUM] …;` → prefix **`DMAMEM `**.
- Line ~36: comment out `#include "debug/printf.h"`.
- Line ~147: comment out the `printf("ERROR status = %x…", …);` statement.

Verify: `diff teensy4/usb_mouse.c imxrt1176/usb_mouse.c` shows **only** those 3 deltas.

- [ ] **Step 1.4: `usb.c` — SET_CONFIGURATION arms EP6 + configure**

After the `#if defined(ENDPOINT5_CONFIG) … #endif` block add:
```c
		#if defined(ENDPOINT6_CONFIG)
		USB1_ENDPTCTRL6 = ENDPOINT6_CONFIG;
		#endif
```
After the `#if defined(KEYBOARD_INTERFACE) usb_keyboard_configure(); #endif` block add:
```c
		#if defined(MOUSE_INTERFACE)
		usb_mouse_configure();
		#endif
```
And in the extern block near the top (next to the keyboard externs) add:
```c
#if defined(MOUSE_INTERFACE)
void usb_mouse_configure(void);
#endif
```
(No `SET_REPORT` for mouse — it has no output report.)

- [ ] **Step 1.5: `usb_inst.cpp` — the `Mouse` global**

Append:
```cpp
#if defined(MOUSE_INTERFACE)
usb_mouse_class Mouse;
#endif
```

- [ ] **Step 1.6: `Arduino.h` — surface `Mouse`**

After the `#include "usb_keyboard.h"` line add:
```c
#include "usb_mouse.h"      // Mouse, MOUSE_LEFT/... (guarded by MOUSE_INTERFACE)
```

- [ ] **Step 1.7: Build (fresh — new core .c)**

```bash
cd ~/Development/rt1170/evkb/usb_keyboard_test
rm -rf build
cmake -B build -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchain/rt1170-evkb.toolchain.cmake" .
cmake --build build 2>&1 | tail -20
```
Expected: links `usb_keyboard_test.elf` (compiles the core incl. `usb_mouse.c`) with no errors.

- [ ] **Step 1.8: Regression — Keyboard gate + CDC gate against the 4-iface composite**

```bash
cd ~/Development/rt1170/evkb/usb_keyboard_test && ./run_qemu_usb_keyboard.sh 2>&1 | tail -3
cd ~/Development/rt1170/evkb/usb_data_test && ./run_qemu_usb_data.sh 2>&1 | tail -2
```
Expected: both **PASS**. (Current QEMU taps the first HID int-IN = keyboard EP5; the mouse EP6 is inert to it, so keyboard + CDC are unaffected.)

- [ ] **Step 1.9: Commit (checkpoint — on request)**

```bash
git -C ~/Development/rt1170/evkb/cores add \
  imxrt1176/usb_desc.h imxrt1176/usb_desc.c imxrt1176/usb_mouse.c \
  imxrt1176/usb.c imxrt1176/usb_inst.cpp imxrt1176/Arduino.h
git -C ~/Development/rt1170/evkb/cores commit -m "feat(usb): add HID mouse interface to the composite"
```

---

## Task 2: Mouse QEMU gate — RED

Run against the **current** (single-`int_in_ep`) QEMU: it taps the first HID int-IN (keyboard EP5), so the mouse report on EP6 is never captured → RED. Task 3 makes it GREEN.

**Files:** Create `usb_mouse_test/{CMakeLists.txt, usb_mouse_test.cpp, usb_mouse_driver.py, run_qemu_usb_mouse.sh, toolchain/rt1170-evkb.toolchain.cmake}`.

- [ ] **Step 2.1: Scaffold**

```bash
mkdir -p ~/Development/rt1170/evkb/usb_mouse_test/toolchain
cp ~/Development/rt1170/evkb/usb_keyboard_test/toolchain/rt1170-evkb.toolchain.cmake \
   ~/Development/rt1170/evkb/usb_mouse_test/toolchain/
```

- [ ] **Step 2.2: `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(usb_mouse_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)
import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)
teensy_add_executable(usb_mouse_test usb_mouse_test.cpp)
teensy_target_link_libraries(usb_mouse_test cores)
target_link_libraries(usb_mouse_test.elf stdc++)
```

- [ ] **Step 2.3: `usb_mouse_test.cpp`**

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "usb_mouse.h"      // Mouse
#include "usb_dev.h"        // usb_configuration

extern volatile uint8_t usb_configuration;

void setup() {
    Serial1.begin(115200);
    uint32_t t0 = millis();
    while (usb_configuration == 0 && (millis() - t0) < 3000) { /* wait for host */ }
    Serial1.println(usb_configuration ? "USB=CONFIGURED" : "USB=TIMEOUT");
    delay(100);                 // let the host be ready
    Mouse.move(10, 5);          // report: 01 00 0A 05 00 00
    Serial1.println("MOUSE=SENT");
}

void loop() { }
```

- [ ] **Step 2.4: `usb_mouse_driver.py`**

```python
#!/usr/bin/env python3
"""Read the QEMU usbhid-tap socket: capture one 6-byte HID mouse report.
Assert Mouse.move(10,5) => 01 00 0A 05 00 00 (report-ID 1, buttons 0, dx 10, dy 5).
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
    while len(got) < 6:
        chunk = sock.recv(64)
        if not chunk: break
        got += chunk
except socket.timeout:
    pass

print("got=%r (%d bytes)" % (got, len(got)))
want = bytes([0x01, 0x00, 0x0A, 0x05, 0x00, 0x00])
sys.exit(0 if got[:6] == want else 1)
```

- [ ] **Step 2.5: `run_qemu_usb_mouse.sh`** (copy the keyboard runner; keep the `set +e/-e` wrap; PORT 14557, `id=usbhid-tap`, ELF `usb_mouse_test.elf`, driver `usb_mouse_driver.py`; PASS line "PASS: USB mouse HID report verified"). Then `chmod +x`.

- [ ] **Step 2.6: Build the gate**

```bash
cd ~/Development/rt1170/evkb/usb_mouse_test
rm -rf build
cmake -B build -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchain/rt1170-evkb.toolchain.cmake" .
cmake --build build 2>&1 | tail -12
```
Expected: `usb_mouse_test.elf` links.

- [ ] **Step 2.7: Run — expect RED**

```bash
cd ~/Development/rt1170/evkb/usb_mouse_test && ./run_qemu_usb_mouse.sh; echo "exit=$?"
```
Expected: **FAIL** (`exit=1`). VCOM shows `USB=CONFIGURED` + `MOUSE=SENT`; the CI-HID log shows `int IN=ep5` / armed on ep5 (the keyboard endpoint); driver `got=b'' (0 bytes)` — the host tapped the wrong HID endpoint.

- [ ] **Step 2.8: Commit (checkpoint — on request)**

```bash
git -C ~/Development/rt1170/evkb add usb_mouse_test/CMakeLists.txt \
  usb_mouse_test/usb_mouse_test.cpp usb_mouse_test/usb_mouse_driver.py \
  usb_mouse_test/run_qemu_usb_mouse.sh usb_mouse_test/toolchain
git -C ~/Development/rt1170/evkb commit -m "test(usb): mouse HID gate (RED — awaits QEMU reactive multi-HID tap)"
```

---

## Task 3: QEMU reactive multi-HID tap — GREEN

Replace the single `int_in_ep` with a `hid_in_mask` and tap any HID interrupt-IN reactively on prime.

**Files:** Modify `~/Development/qemu2/include/hw/usb/chipidea.h`, `hw/usb/chipidea_cdc.c`, `hw/usb/chipidea_udc.c`.

- [ ] **Step 3.1: `chipidea.h` — mask replaces the scalar**

In `CIUDCState`, replace:
```c
    int      int_in_ep;          /* HID interrupt-IN endpoint (report source) */
```
with:
```c
    uint32_t hid_in_mask;        /* bit N set = endpoint N is a HID interrupt-IN */
```

- [ ] **Step 3.2: `chipidea_cdc.c` — build the mask, drop pre-arm/re-arm, route by mask**

(a) `cdc_parse_endpoints` reset — replace `… = ci->udc.int_in_ep = 0;` with:
```c
    ci->udc.bulk_in_ep = ci->udc.bulk_out_ep = 0;
    ci->udc.hid_in_mask = 0;
```
(b) The interrupt-IN match — replace:
```c
            } else if ((attr & 0x3) == 0x3 && (addr & 0x80) &&
                       cur_iface_class == 0x03 &&      /* HID interface only... */
                       !ci->udc.int_in_ep) {           /* ...not the CDC ACM notify EP */
                ci->udc.int_in_ep = addr & 0x0f;
            }
```
with (record ALL HID interrupt-INs):
```c
            } else if ((attr & 0x3) == 0x3 && (addr & 0x80) &&
                       cur_iface_class == 0x03) {      /* HID interrupt-IN (not CDC ACM) */
                ci->udc.hid_in_mask |= (1u << (addr & 0x0f));
            }
```
(c) `CLOG` — change `int IN=ep%d … int_in_ep` to `hid int-IN mask=0x%02x … ci->udc.hid_in_mask`.
(d) `cdc_enter_run` — **delete** the HID pre-arm block:
```c
    if (ci->hid_host && ci->udc.int_in_ep) {
        CLOG("HID interrupt-IN armed on ep%d\n", ci->udc.int_in_ep);
        ci_udc_arm_bulk_in(ci, ci->udc.int_in_ep);
    }
```
(keep the `if (ci->cdc_host && ci->udc.bulk_in_ep) { … }` block). HID is reactive now.
(e) `hid_on_int_in` — tap only, drop the re-arm:
```c
static void hid_on_int_in(ChipideaState *ci, uint32_t len)
{
    if (len) {
        qemu_chr_fe_write_all(&ci->hid_be, ci->udc.vh_in, len);
    }
    /* reactive: the next guest prime services the next report (no re-arm) */
}
```
(f) `ci_cdc_on_complete` route — replace `else if (dir == 1 && ci->udc.int_in_ep && ep == ci->udc.int_in_ep)` with:
```c
    } else if (dir == 1 && (ci->udc.hid_in_mask & (1u << ep))) {
        hid_on_int_in(ci, len);
```

- [ ] **Step 3.3: `chipidea_udc.c` — reactive service on prime**

In `ci_udc_service_prime`, extend the `ready` test (currently at ~line 282):
```c
            bool ready = (dir && ci->udc.vh_in_pending  && ci->udc.vh_in_ep  == ep) ||
                         (!dir && ci->udc.vh_out_pending && ci->udc.vh_out_ep == ep) ||
                         (dir && ci->hid_host && (ci->udc.hid_in_mask & (1u << ep)));
```

- [ ] **Step 3.4: Build QEMU**

```bash
cd ~/Development/qemu2/build && ninja qemu-system-arm 2>&1 | tail -6
```
Expected: builds with no errors.

- [ ] **Step 3.5: Run the mouse gate — expect GREEN**

```bash
cd ~/Development/rt1170/evkb/usb_mouse_test && ./run_qemu_usb_mouse.sh; echo "exit=$?"
```
Expected: **`PASS: USB mouse HID report verified`** (`exit=0`); driver `got=b'\x01\x00\x0a\x05\x00\x00' (6 bytes)`; CI-HID shows `hid int-IN mask=0x60` (bits 5 and 6).

- [ ] **Step 3.6: Regressions — Keyboard gate + CDC gate**

```bash
cd ~/Development/rt1170/evkb/usb_keyboard_test && ./run_qemu_usb_keyboard.sh 2>&1 | tail -3
cd ~/Development/rt1170/evkb/usb_data_test && ./run_qemu_usb_data.sh 2>&1 | tail -2
```
Expected: both **PASS** (keyboard EP5 now serviced reactively; CDC unaffected since `hid_host=false`).

- [ ] **Step 3.7: Commit (checkpoint — on request)**

```bash
git -C ~/Development/qemu2 add include/hw/usb/chipidea.h hw/usb/chipidea_cdc.c hw/usb/chipidea_udc.c
git -C ~/Development/qemu2 commit -m "hw/usb: chipidea reactively taps all HID interrupt-INs (hid_in_mask)"
```

---

## Task 4: Hardware verification

- [ ] **Step 4.1: Flash** — `pkill -9 -f LinkServer; pkill -9 -f redlinkserv; sleep 1`, then `LinkServer run MIMXRT1176:MIMXRT1170-EVKB ~/Development/rt1170/evkb/usb_mouse_test/build/usb_mouse_test.elf`. (If a recurring `DAPInfo … transfer error`, ask the user to replug J11.)

- [ ] **Step 4.2: Confirm enumeration** — `hidutil list | grep -iE "4617|VendorID"` shows a **Mouse** (UsagePage 1 / **Usage 2**) alongside the Keyboard (Usage 6); `system_profiler SPUSBDataType | grep -A6 "USB Serial"` shows PID **0x0003**; CDC node present.

- [ ] **Step 4.3: Confirm the cursor moves** — build+flash a periodic-`Mouse.move` HW variant (`usb_mouse_hw.cpp`, mirror `usb_keyboard_hw`: `Mouse.move(8,0); delay(300); Mouse.move(-8,0);` every ~1 s, + CDC echo). Watch the Mac cursor drift left/right. (Reflash the quiet one-shot after.)

- [ ] **Step 4.4: Confirm Keyboard + CDC still work** — with the composite enumerated, `hidutil list` still shows the keyboard; pyserial echo on the native-USB CDC node still round-trips.

- [ ] **Step 4.5: Record + commit** — write `usb_mouse_test/HW-RESULTS.md`; `git -C ~/Development/rt1170/evkb add usb_mouse_test/HW-RESULTS.md && … commit`.

---

## Self-review

**Spec coverage:** §4.1 descriptor → Task 1.1-1.2 ✓ · §4.2 EP0 → 1.4 ✓ · §4.3 backend+globals → 1.3/1.5/1.6 ✓ · §4.4 QEMU reactive tap → Task 3 ✓ · §4.5 gate + regressions → Task 2 + 3.6 ✓ · §4.6 HW → Task 4 ✓ · Keyboard/CDC regressions → 1.8 + 3.6 ✓.

**Placeholder scan:** none; verbatim bodies use `cp`/verbatim-copy with explicit deltas; the report-desc bytes are shown and cross-checked to 84 bytes.

**Type/name consistency:** `hid_in_mask` (replaces `int_in_ep`) used identically in chipidea.h/cdc.c/udc.c; `MOUSE_INTERFACE 3`/`MOUSE_ENDPOINT 6`/`MOUSE_SIZE 8`, `usb_mouse_configure`, PID `0x0003`, PORT `14557`, report `01 00 0A 05 00 00`, ELF `usb_mouse_test.elf` — consistent across tasks.
