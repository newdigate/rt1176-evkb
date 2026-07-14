# RT1176 USB device HID — Composite CDC + Keyboard — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the MIMXRT1176-EVKB present to a host as a composite **CDC serial + HID keyboard**, driven by Teensy-style `Keyboard.press()/release()/print()`, with `Serial` still alive alongside.

**Architecture:** Extend the existing CDC-only USB device stack into one hardcoded composite descriptor (CDC on EP2/3/4 + Keyboard interrupt-IN on EP5). Port teensy4's `usb_keyboard.c` verbatim (keylayouts already vendored) with the RT1176 `DMAMEM` dTD fix. Extend the in-QEMU ChipIdea CDC host to also arm + tap the interrupt-IN endpoint, using **one tap per gate** to stay within QEMU's single virtual-host-IN slot. Gate-first: core lands (regression-checked against the CDC gate), then a keyboard gate goes RED against unmodified QEMU, then the QEMU interrupt-IN capture turns it GREEN, then hardware.

**Tech Stack:** C (Arduino/Teensyduino core, ARM GCC 10.2.1), C (QEMU device model, `qemu-system-arm`), CMake + Unix Makefiles cross-build, Python 3 socket assert driver, LinkServer flash.

**Spec:** `docs/superpowers/specs/2026-07-14-rt1176-usb-device-hid-keyboard-design.md`

**Commits:** Per the user's workflow, `git commit` runs **only on explicit request**. Treat every "Commit" step as a checkpoint that names the repo + exact files to stage and then **pauses for the go-ahead** — do not commit unprompted. Repos: core = `git -C ~/Development/rt1170/evkb/cores` (nested repo → github `teensy-cores`); qemu = `git -C ~/Development/qemu2`; gates = `git -C ~/Development/rt1170/evkb` (local only). Never sweep the pre-existing `evkb` WIP into a commit — stage only files this plan touches.

---

## Task 1: Core — composite CDC + Keyboard support

The `KEYBOARD_INTERFACE` guard activates all keyboard code at once, so the descriptor, backend, EP0 wiring and globals land together and are verified by: (a) the core compiles, (b) the existing CDC gate still passes (proves the composite descriptor didn't break CDC).

**Files:**
- Modify: `cores/imxrt1176/usb_desc.h`
- Modify: `cores/imxrt1176/usb_desc.c`
- Create: `cores/imxrt1176/usb_keyboard.c`
- Modify: `cores/imxrt1176/usb.c`
- Modify: `cores/imxrt1176/usb_inst.cpp`
- Modify: `cores/imxrt1176/Arduino.h`

- [ ] **Step 1.1: Edit `usb_desc.h` — defines**

In `cores/imxrt1176/usb_desc.h`: change `PRODUCT_ID`, `NUM_ENDPOINTS`, `NUM_INTERFACE`, `CONFIG_DESC_SIZE`, and add the keyboard defines + `LAYOUT_US_ENGLISH`.

Change these existing lines:
```c
#define PRODUCT_ID           0x0002   /* was 0x0001 — bump forces macOS descriptor re-read */
#define NUM_ENDPOINTS        5        /* was 4 */
#define NUM_INTERFACE        3        /* was 2 */
```
Change:
```c
#define CONFIG_DESC_SIZE     100      /* was 75: +25 = 9 iface + 9 HID + 7 endpoint */
```
Add, immediately after the CDC endpoint defines (before `ENDPOINT2_CONFIG`):
```c
/* Keyboard HID (boot protocol, 8-byte report, no REPORT_ID) */
#define KEYBOARD_INTERFACE       2
#define KEYBOARD_ENDPOINT        5        /* interrupt IN (0x85) */
#define KEYBOARD_SIZE            8
#define KEYBOARD_INTERVAL        1
#define KEYBOARD_HID_DESC_OFFSET 84       /* 75 (end of CDC block) + 9 (kbd iface desc) */
#define LAYOUT_US_ENGLISH                 /* selects keylayouts.h US table; set before keylayouts.h is included */
```
Add, after the existing `ENDPOINT4_CONFIG` define:
```c
#define ENDPOINT5_CONFIG  (ENDPOINT_RECEIVE_UNUSED | ENDPOINT_TRANSMIT_INTERRUPT) /* 0x00CC0002 */
```

- [ ] **Step 1.2: Edit `usb_desc.c` — device class, config bytes, report descriptor, list entries**

(a) Device descriptor — change the class/subclass/protocol triple from `0, 0, 0` to the IAD composite:
```c
    0xEF, 0x02, 0x01,                       /* Misc / IAD / composite (was 0,0,0) */
```

(b) Config descriptor header — change `bNumInterfaces` from `2` to `3` (the 5th byte of the config descriptor):
```c
    /* configuration (9) */
    9, 2, LSB(CONFIG_DESC_SIZE), MSB(CONFIG_DESC_SIZE), 3, 1, 0, 0xC0, 50,
```

(c) Append the keyboard block to `config_descriptor[]` — after the final `/* bulk IN endpoint, EP4 IN (7) */` line, add a comma and:
```c
    ,
    /* keyboard interface (9) */
    9, 4, KEYBOARD_INTERFACE, 0, 1, 0x03, 0x01, 0x01, 0,
    /* HID descriptor (9) */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22,
        LSB(sizeof(keyboard_report_desc)), MSB(sizeof(keyboard_report_desc)),
    /* keyboard endpoint, EP5 IN (7) */
    7, 5, KEYBOARD_ENDPOINT | 0x80, 0x03, KEYBOARD_SIZE, 0, KEYBOARD_INTERVAL
```

(d) Add the report descriptor — place this **above** `config_descriptor` (so `sizeof(keyboard_report_desc)` is in scope), guarded by the interface define. Bytes verbatim from teensy4 `usb_desc.c:173-219` (KEYBOARD_SIZE==8 branch, 63 bytes):
```c
#if defined(KEYBOARD_INTERFACE)
/* Keyboard Protocol 1, HID 1.11 spec, Appendix B, boot layout (63 bytes) */
static uint8_t keyboard_report_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x75, 0x01, 0x95, 0x08,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x03, 0x95, 0x05,
    0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x03, 0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0x7F, 0x05, 0x07, 0x19, 0x00, 0x29, 0x7F,
    0x81, 0x00, 0xC0
};
#endif
```

(e) Add two entries to `usb_descriptor_list[]` — insert before the string0 entry:
```c
#if defined(KEYBOARD_INTERFACE)
    {0x2200, KEYBOARD_INTERFACE, keyboard_report_desc, sizeof(keyboard_report_desc)},
    {0x2100, KEYBOARD_INTERFACE, config_descriptor + KEYBOARD_HID_DESC_OFFSET, 9},
#endif
```

- [ ] **Step 1.3: Create `usb_keyboard.c` — verbatim port + 3 precedented deltas**

Copy the teensy4 backend verbatim:
```bash
cp ~/Development/rt1170/evkb/cores/teensy4/usb_keyboard.c \
   ~/Development/rt1170/evkb/cores/imxrt1176/usb_keyboard.c
```
Then apply exactly these three edits (all precedented by the `usb_serial.c` port):

Delta (a) — DMA-reachable dTDs. Change line ~96:
```c
/* was: static transfer_t tx_transfer[TX_NUM] __attribute__ ((used, aligned(32))); */
DMAMEM static transfer_t tx_transfer[TX_NUM] __attribute__ ((used, aligned(32)));
```

Delta (b) — drop the debug printf (our core has no `debug/printf.h`; `usb_serial.c` comments its printf out). Remove the include line `#include "debug/printf.h"` and comment out the single `printf("ERROR status = %x…")` call (~line 572):
```c
			if (status & 0x68) {
				// printf("ERROR status = %x, i=%d, ms=%u\n",
				//	status, tx_head, systick_millis_count);
			}
```

Delta (c) — include alignment. Confirm `DMAMEM`/`PROGMEM` resolve (they come via `avr/pgmspace.h`, already used by `usb_serial.c`). Keep the `#include "avr/pgmspace.h"` line; drop the duplicate `#include "core_pins.h"` only if it causes a warning (leave otherwise). No other changes — the rest of the file is byte-for-byte teensy4.

- [ ] **Step 1.4: Edit `usb.c` — SET_CONFIGURATION, SET_REPORT, completion, extern**

(a) In `endpoint0_setup()` `case 0x0900` (SET_CONFIGURATION), after the `#if defined(ENDPOINT4_CONFIG) … #endif` block add:
```c
		#if defined(ENDPOINT5_CONFIG)
		USB1_ENDPTCTRL5 = ENDPOINT5_CONFIG;
		#endif
```
and after the `usb_serial_configure();` guarded block add:
```c
		#if defined(KEYBOARD_INTERFACE)
		usb_keyboard_configure();
		#endif
```

(b) Add the SET_REPORT case — place it after the CDC `case 0x2021` block, before the final closing `}` / stall (verbatim from teensy4 `usb.c:637-645`):
```c
#if defined(KEYBOARD_INTERFACE)
	  case 0x0921: // HID SET_REPORT (e.g. keyboard LEDs)
		if (setup.wLength <= sizeof(endpoint0_buffer)) {
			endpoint0_setupdata.bothwords = setup.bothwords;
			endpoint0_buffer[0] = 0xE9;
			endpoint0_receive(endpoint0_buffer, setup.wLength, 1);
			return;
		}
		break;
#endif
```

(c) In `endpoint0_complete()`, after the CDC `#ifdef CDC_STATUS_INTERFACE … #endif` block add (verbatim from teensy4 `usb.c:867-871`):
```c
#ifdef KEYBOARD_INTERFACE
	if (setup.word1 == 0x02000921 && setup.word2 == ((1 << 16) | KEYBOARD_INTERFACE)) {
		keyboard_leds = endpoint0_buffer[0];
		endpoint0_transmit(NULL, 0, 0);
	}
#endif
```

(d) Give `usb.c` visibility of `keyboard_leds` — add near the top of `usb.c` (after the includes):
```c
#if defined(KEYBOARD_INTERFACE)
extern volatile uint8_t keyboard_leds;
void usb_keyboard_configure(void);
#endif
```

- [ ] **Step 1.5: Edit `usb_inst.cpp` — the `Keyboard` global**

Append to `cores/imxrt1176/usb_inst.cpp`:
```cpp
#include "usb_keyboard.h"

#if defined(KEYBOARD_INTERFACE)
usb_keyboard_class Keyboard;
#endif
```

- [ ] **Step 1.6: Edit `Arduino.h` — surface `Keyboard` / `KEY_*` to sketches**

In `cores/imxrt1176/Arduino.h`, after the existing `#include "usb_serial.h"` line, add:
```c
#include "usb_keyboard.h"   // Keyboard, KEY_* (guarded by KEYBOARD_INTERFACE)
```

- [ ] **Step 1.7: Build — fresh reconfigure of the CDC gate (new core .c → file(GLOB))**

Adding `usb_keyboard.c` requires a from-scratch configure of any dir that compiles the core.

Run:
```bash
cd ~/Development/rt1170/evkb/usb_data_test
rm -rf build
cmake -B build -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchain/rt1170-evkb.toolchain.cmake" .
cmake --build build 2>&1 | tail -20
```
Expected: links `build/usb_data_test.elf` with **no errors** (compiles `usb_keyboard.c` + the composite descriptor). If `keycodes_ascii`/`KEY_*` are undefined → `LAYOUT_US_ENGLISH` isn't reaching `keylayouts.h`; verify Step 1.1 placed the `#define` before any keylayouts include in the chain.

- [ ] **Step 1.8: Regression — CDC gate must still pass against the composite descriptor**

Run:
```bash
cd ~/Development/rt1170/evkb/usb_data_test
./run_qemu_usb_data.sh
```
Expected: `PASS: USB CDC bulk data echo verified`, and VCOM shows `USB=CONFIGURED`. This proves the now-composite descriptor still enumerates CDC and the core compiles cleanly. If it fails, fix the descriptor bytes (Step 1.2) before proceeding — a malformed config descriptor breaks enumeration for both classes.

- [ ] **Step 1.9: Commit (checkpoint — on request)**

```bash
git -C ~/Development/rt1170/evkb/cores add \
  imxrt1176/usb_desc.h imxrt1176/usb_desc.c imxrt1176/usb_keyboard.c \
  imxrt1176/usb.c imxrt1176/usb_inst.cpp imxrt1176/Arduino.h
git -C ~/Development/rt1170/evkb/cores commit -m "feat(usb): composite CDC + HID keyboard device (descriptor + backend + EP0)"
```

---

## Task 2: Keyboard QEMU gate — RED

Write the gate. Run it against **unmodified** QEMU: with only `usbhid-tap` connected, the unmodified CDC host has `cdc_host=false` and no HID concept, so it never enumerates the gadget and never taps the interrupt-IN → the driver captures nothing → RED. Task 3 makes it GREEN.

**Files:**
- Create: `usb_keyboard_test/CMakeLists.txt`
- Create: `usb_keyboard_test/usb_keyboard_test.cpp`
- Create: `usb_keyboard_test/usb_kbd_driver.py`
- Create: `usb_keyboard_test/run_qemu_usb_keyboard.sh`
- Create: `usb_keyboard_test/toolchain/rt1170-evkb.toolchain.cmake` (copy)

- [ ] **Step 2.1: Scaffold the gate dir + toolchain**

```bash
mkdir -p ~/Development/rt1170/evkb/usb_keyboard_test/toolchain
cp ~/Development/rt1170/evkb/usb_data_test/toolchain/rt1170-evkb.toolchain.cmake \
   ~/Development/rt1170/evkb/usb_keyboard_test/toolchain/
```

- [ ] **Step 2.2: Write `CMakeLists.txt`**

Create `usb_keyboard_test/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.24)
project(usb_keyboard_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)
import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)
teensy_add_executable(usb_keyboard_test usb_keyboard_test.cpp)
teensy_target_link_libraries(usb_keyboard_test cores)
target_link_libraries(usb_keyboard_test.elf stdc++)
```

- [ ] **Step 2.3: Write the sketch `usb_keyboard_test.cpp`**

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "usb_keyboard.h"   // Keyboard, KEY_A
#include "usb_dev.h"        // usb_configuration

extern volatile uint8_t usb_configuration;

void setup() {
    Serial1.begin(115200);                              // debug VCOM (LPUART1)
    uint32_t t0 = millis();
    while (usb_configuration == 0 && (millis() - t0) < 3000) { /* wait for host */ }
    Serial1.println(usb_configuration ? "USB=CONFIGURED" : "USB=TIMEOUT");
    delay(100);                                         // let the host arm the interrupt-IN
    Keyboard.press(KEY_A);                              // report: 00 00 04 00 00 00 00 00
    delay(10);                                          // separate the two reports
    Keyboard.release(KEY_A);                            // report: 00 00 00 00 00 00 00 00
    Serial1.println("KBD=SENT");
}

void loop() { }
```

- [ ] **Step 2.4: Write the driver `usb_kbd_driver.py`**

```python
#!/usr/bin/env python3
"""Read the QEMU usbhid-tap socket: capture two 8-byte HID keyboard reports.
Assert press {00 00 04 00 00 00 00 00} then release {00 00 00 00 00 00 00 00}.
Exit 0 on match, 1 on mismatch/short, 2 on connection failure."""
import socket, sys, time

host, port = sys.argv[1], int(sys.argv[2])

sock = None
deadline = time.time() + 10
while time.time() < deadline:
    try:
        sock = socket.create_connection((host, port), timeout=1)
        break
    except OSError:
        time.sleep(0.2)
if sock is None:
    print("ERROR: could not connect to usbhid-tap socket")
    sys.exit(2)

sock.settimeout(8)          # guest waits for enum (~<3s) then sends after 100ms
got = b""
try:
    while len(got) < 16:
        chunk = sock.recv(64)
        if not chunk:
            break
        got += chunk
except socket.timeout:
    pass

print("got=%r (%d bytes)" % (got, len(got)))
press   = bytes([0, 0, 4, 0, 0, 0, 0, 0])
release = bytes([0, 0, 0, 0, 0, 0, 0, 0])
ok = len(got) >= 16 and got[0:8] == press and got[8:16] == release
sys.exit(0 if ok else 1)
```

- [ ] **Step 2.5: Write the runner `run_qemu_usb_keyboard.sh`**

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/usb_keyboard_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/usb.dbg"; RES="$DIR/kbd.result"
gate_tmp "$RES"
PORT=14556
rm -f "$VCOM" "$DBG" "$RES"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none \
    -serial file:"$VCOM" \
    -chardev socket,id=usbhid-tap,host=127.0.0.1,port=$PORT,server=on,wait=off \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
python3 "$DIR/usb_kbd_driver.py" 127.0.0.1 $PORT > "$RES" 2>&1
RC=$?
sleep 1; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== VCOM ===="; cat "$VCOM" 2>/dev/null || true
echo "==== CI-HID ===="; grep -E "CI-CDC|CI-HID" "$DBG" 2>/dev/null | head
echo "==== kbd driver ===="; cat "$RES"
[ $RC -eq 0 ] || { echo "FAIL: USB keyboard report"; exit 1; }
echo "PASS: USB keyboard HID report verified"
```
```bash
chmod +x ~/Development/rt1170/evkb/usb_keyboard_test/run_qemu_usb_keyboard.sh
```

- [ ] **Step 2.6: Build the gate**

```bash
cd ~/Development/rt1170/evkb/usb_keyboard_test
rm -rf build
cmake -B build -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchain/rt1170-evkb.toolchain.cmake" .
cmake --build build 2>&1 | tail -20
```
Expected: `build/usb_keyboard_test.elf` links with no errors (compiles because Task 1 made `Keyboard`/`KEY_A` available).

- [ ] **Step 2.7: Run — expect RED**

```bash
cd ~/Development/rt1170/evkb/usb_keyboard_test
./run_qemu_usb_keyboard.sh; echo "exit=$?"
```
Expected: **`FAIL: USB keyboard report`** (`exit=1`). VCOM shows `USB=TIMEOUT` (unmodified QEMU never enumerates a HID-only-tap gadget) and the driver prints `got=b'' (0 bytes)`. This is the intended RED — the QEMU host can't yet drive/tap the interrupt-IN.

- [ ] **Step 2.8: Commit (checkpoint — on request)**

```bash
git -C ~/Development/rt1170/evkb add usb_keyboard_test/CMakeLists.txt \
  usb_keyboard_test/usb_keyboard_test.cpp usb_keyboard_test/usb_kbd_driver.py \
  usb_keyboard_test/run_qemu_usb_keyboard.sh usb_keyboard_test/toolchain
git -C ~/Development/rt1170/evkb commit -m "test(usb): keyboard HID gate (RED — awaits QEMU interrupt-IN tap)"
```

---

## Task 3: QEMU interrupt-IN capture — GREEN

Extend the ChipIdea CDC host into a CDC+HID composite host: parse the interrupt-IN endpoint, arm it when a `usbhid-tap` chardev is bound, forward each report to that tap, and drive enumeration whenever **either** `cdc_host` or `hid_host` is set. Additive — the dQH/dTD engine is transfer-type-agnostic, so `ci_udc_arm_bulk_in` polls interrupt-IN identically.

**Files:**
- Modify: `~/Development/qemu2/include/hw/usb/chipidea.h`
- Modify: `~/Development/qemu2/hw/usb/chipidea.c`
- Modify: `~/Development/qemu2/hw/usb/chipidea_cdc.c`
- Modify: `~/Development/qemu2/hw/usb/chipidea_udc.c`
- Modify: `~/Development/qemu2/hw/arm/fsl-imxrt1170.c`

- [ ] **Step 3.1: `chipidea.h` — new state fields**

In `struct ChipideaState`, next to the existing `cdc_be`/`cdc_host`, add:
```c
    CharBackend hid_be;
    bool hid_host;
```
In `CIUDCState` (next to `int bulk_in_ep, bulk_out_ep;`, ~line 51), add:
```c
    int int_in_ep;
```

- [ ] **Step 3.2: `chipidea.c` — chardev property + realize flag**

In `chipidea_props[]`, after `DEFINE_PROP_CHR("chardev", ChipideaState, cdc_be),` add:
```c
    DEFINE_PROP_CHR("hid-chardev", ChipideaState, hid_be),
```
In `chipidea_realize()`, after the existing `if (qemu_chr_fe_backend_connected(&ci->cdc_be)) { ci->cdc_host = true; ci_cdc_setup(ci); }` block, add:
```c
    if (qemu_chr_fe_backend_connected(&ci->hid_be)) {
        ci->hid_host = true;   /* device->host only: no read handlers needed */
    }
```

- [ ] **Step 3.3: `chipidea_cdc.c` — parse interrupt-IN, arm it, forward + re-arm, route**

(a) In `cdc_parse_endpoints`, inside the `if (type == 0x05 && …)` endpoint-descriptor block, after the existing bulk `if`/`else if`, add:
```c
        if ((attr & 0x3) == 0x3 && (addr & 0x80) && !ci->udc.int_in_ep) {
            ci->udc.int_in_ep = addr & 0x0f;      /* interrupt IN (HID report) */
        }
```

(b) Add `hid_on_int_in` — place it **above** `ci_cdc_on_complete` (so it needs no forward decl), mirroring `cdc_on_bulk_in`:
```c
static void hid_on_int_in(ChipideaState *ci, uint32_t len)
{
    if (len) {
        qemu_chr_fe_write_all(&ci->hid_be, ci->udc.vh_in, len);
    }
    ci_udc_arm_bulk_in(ci, ci->udc.int_in_ep);    /* re-arm the standing interrupt-IN */
}
```

(c) Replace the body of `cdc_enter_run` (currently early-returns if no bulk, then arms bulk-IN only) with host-gated arming of both pipes:
```c
static void cdc_enter_run(ChipideaState *ci)
{
    ci->udc.cdc_step = CDC_RUN;
    if (ci->cdc_host && ci->udc.bulk_in_ep) {
        ci_udc_arm_bulk_in(ci, ci->udc.bulk_in_ep);   /* standing bulk-IN (CDC) */
        qemu_chr_fe_accept_input(&ci->cdc_be);
    }
    if (ci->hid_host && ci->udc.int_in_ep) {
        ci_udc_arm_bulk_in(ci, ci->udc.int_in_ep);    /* standing interrupt-IN (HID) */
    }
}
```
(One-tap-per-gate: the keyboard gate binds only `usbhid-tap`, so only the interrupt-IN is armed — no collision on the single `vh_in` slot. Keep the function name `cdc_enter_run`.)

(d) In `ci_cdc_on_complete`, alongside the existing `dir == 1 && ep == ci->udc.bulk_in_ep → cdc_on_bulk_in(ci, len)` branch, add:
```c
        } else if (dir == 1 && ep == ci->udc.int_in_ep) {
            hid_on_int_in(ci, len);
```

- [ ] **Step 3.4: `chipidea_udc.c` — drive enumeration on cdc_host OR hid_host**

Three dispatch sites currently gate on `ci->cdc_host`; change each to `ci->cdc_host || ci->hid_host`:
1. In the `R_USBCMD` RS path (~line 371) that calls `ci_cdc_start(ci)`.
2. In `ci_udc_setup_timer_cb` (~line 439-440) that calls `ci_cdc_on_reset_acked(ci)`.
3. In `ci_complete_endpoint` (~line 253) the `else if (ci->cdc_host)` that calls `ci_cdc_on_complete(...)`.

Example (site 3):
```c
    } else if (ci->cdc_host || ci->hid_host) {
        ci_cdc_on_complete(ci, ep, dir, n);
    }
```
Apply the same `|| ci->hid_host` to sites 1 and 2. (`ci_cdc_on_complete` already handles `ep==0` control for enumeration, so a HID-only run still enumerates.)

- [ ] **Step 3.5: `fsl-imxrt1170.c` — bind the `usbhid-tap` chardev**

Inside the `if (i == 0) { … }` USB block, right after the `usbcdc` `qemu_chr_find`/`qdev_prop_set_chr` lines (~973-978), add:
```c
            Chardev *hidtap = qemu_chr_find("usbhid-tap");
            if (hidtap) {
                qdev_prop_set_chr(DEVICE(&s->usb[i]), "hid-chardev", hidtap);
            }
```

- [ ] **Step 3.6: Build QEMU**

```bash
cd ~/Development/qemu2/build && ninja qemu-system-arm 2>&1 | tail -20
```
Expected: builds `qemu-system-arm` with no errors.

- [ ] **Step 3.7: Run the keyboard gate — expect GREEN**

```bash
cd ~/Development/rt1170/evkb/usb_keyboard_test
./run_qemu_usb_keyboard.sh; echo "exit=$?"
```
Expected: **`PASS: USB keyboard HID report verified`** (`exit=0`). VCOM shows `USB=CONFIGURED` then `KBD=SENT`; the driver prints `got=b'\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' (16 bytes)`. If only 8 bytes arrive, increase the `delay(10)` in the sketch (Step 2.3) so the press report drains before the release report is queued.

- [ ] **Step 3.8: Regression — CDC gate still GREEN**

```bash
cd ~/Development/rt1170/evkb/usb_data_test && ./run_qemu_usb_data.sh
```
Expected: `PASS: USB CDC bulk data echo verified` (the composite host still drains bulk-IN when only `usbcdc` is bound).

- [ ] **Step 3.9: Commit (checkpoint — on request)**

```bash
git -C ~/Development/qemu2 add include/hw/usb/chipidea.h hw/usb/chipidea.c \
  hw/usb/chipidea_cdc.c hw/usb/chipidea_udc.c hw/arm/fsl-imxrt1170.c
git -C ~/Development/qemu2 commit -m "hw/usb: chipidea composite host taps HID interrupt-IN (usbhid-tap)"
```

---

## Task 4: Hardware verification (the real arbiter — the Mac is the host)

QEMU green proves consistency, not silicon correctness. The Mac is the judge.

**Files:**
- Create: `usb_keyboard_test/HW-RESULTS.md`

- [ ] **Step 4.1: Flash**

```bash
pkill -9 -f LinkServer; pkill -9 -f redlinkserv; sleep 1
/Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB \
   ~/Development/rt1170/evkb/usb_keyboard_test/build/usb_keyboard_test.elf
```
Expected: loads + resets + runs (no halt-on-vector-catch). If the probe is stuck (`DAPInfo … transfer error` after killing both processes), unplug/replug the MCU-Link USB (J11).

- [ ] **Step 4.2: Confirm composite enumeration**

Plug the **native-USB** port into the Mac. Run:
```bash
system_profiler SPUSBDataType | grep -iE "keyboard|serial|1209|0x1209|Product ID|Vendor" -A2
```
Expected: a **HID Keyboard** entry **and** a **CDC serial** node, under Vendor `0x1209` / Product `0x0002`. If neither appears, macOS may still be caching — the PID bump (0x0002) should have prevented this; if not, try a different USB port. Record the exact output.

- [ ] **Step 4.3: Confirm the keystroke lands**

The sketch types `a` once, ~100 ms after enumeration, on each boot. Open a focused text field (or `hidutil monitor --matching '{"ProductID":0x0002}'` in a terminal), then re-run Step 4.1 to reboot the board. Expected: an `a` character is typed into the focused field (or the HID input event shows usage `0x04`).

- [ ] **Step 4.4: Confirm CDC still works alongside**

With the board still enumerated, read the native-USB CDC node with pyserial (never `cat`) and echo a byte:
```bash
python3 - <<'PY'
import serial, glob, time
# native-USB CDC node (VID 0x1209) — NOT the MCU-Link VCOM (usbmodem5DQ2DDHVWO5EI3)
port = [p for p in glob.glob('/dev/cu.usbmodem*') if '5DQ2DDHVWO5EI3' not in p][0]
s = serial.Serial(port, 115200, timeout=2); time.sleep(0.5)
s.write(b'Z'); print('echo=%r' % s.read(1)); s.close()
PY
```
Expected: `echo=b'Z'` — `Serial` echoes concurrently with the keyboard interface present. (This must not run from the USB ISR — the CDC echo is in `loop()`; heed the "never `Serial1.print` from the USB ISR" trap in any added debug.)

- [ ] **Step 4.5: Record results + commit (checkpoint — on request)**

Write `usb_keyboard_test/HW-RESULTS.md` with the `system_profiler` output, the keystroke observation, and the CDC echo result. Then:
```bash
git -C ~/Development/rt1170/evkb add usb_keyboard_test/HW-RESULTS.md
git -C ~/Development/rt1170/evkb commit -m "test(usb): keyboard HID HW-verified (enumerate + keystroke + concurrent CDC)"
```

---

## Self-review

**Spec coverage:**
- §4.1 descriptor surgery → Task 1.1-1.2 ✓ · §4.2 EP0 (SET_CONFIG/SET_REPORT/complete/extern) → Task 1.4 ✓ · §4.3 backend verbatim + 3 deltas → Task 1.3 ✓ · §4.4 globals/Arduino.h/LAYOUT → Task 1.1/1.5/1.6 ✓ · §4.5 QEMU one-tap-per-gate → Task 3 ✓ · §4.6 gate → Task 2 ✓ · §4.7 HW → Task 4 ✓ · §1 CDC regression → Task 1.8 + 3.8 ✓ · §6 risks (DMAMEM, PID, single-slot, printf, file(GLOB)) each have a step.
- Deferred items (§7) intentionally have no task.

**Placeholder scan:** no TBD/TODO/"handle errors"/"similar to". Verbatim file bodies use `cp` + explicit edits (a precise instruction, not a placeholder); the one "confirm include resolves" (1.3c) is a named check with a concrete fallback.

**Type/name consistency:** `hid_be`/`hid_host`/`int_in_ep`, `usbhid-tap`, `hid-chardev`, `KEYBOARD_INTERFACE`/`KEYBOARD_ENDPOINT 5`/`KEYBOARD_SIZE 8`, `hid_on_int_in`, `usb_keyboard_configure`, `keyboard_leds`, PID `0x0002`, PORT `14556`, ELF `usb_keyboard_test.elf` — used identically across Tasks 1-4.
