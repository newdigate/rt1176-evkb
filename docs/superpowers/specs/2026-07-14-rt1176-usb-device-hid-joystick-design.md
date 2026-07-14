# RT1176 USB device HID — Scope C: add Joystick to the composite

**Date:** 2026-07-14
**Status:** design approved, ready for `writing-plans`
**Builds on:** `rt1176-usb-device-hid-mouse` (Scope B, HW-verified). Appends a Joystick
interface to the CDC + Keyboard + Mouse composite. **The QEMU reactive `hid_in_mask`
tap already handles the joystick's interrupt-IN — no QEMU changes.**

---

## 1. Goal & success criteria

Add a HID **Joystick** interface to the composite (now CDC + Keyboard + Mouse +
**Joystick**), driven by the Teensy `Joystick.button()/X()/…` global, with all existing
classes still working.

**Success:**
- **QEMU gate** (`evkb/usb_joystick_test/`): a firmware
  `Joystick.useManualSend(true); Joystick.button(1,1); Joystick.X(512); Joystick.send_now();`
  is captured on the joystick interrupt-IN (EP7); the runner asserts the 12-byte report
  **`01 00 00 00 00 20 00 00 00 00 00 00`** (button 1 → `data[0]=0x1`; X=512 → `data[1]=512<<4=0x2000`).
  Fail-first, then pass.
- **Regressions:** the Mouse, Keyboard, and CDC gates all still pass against the
  5-interface composite.
- **Hardware:** `hidutil list` / `system_profiler` show a HID **Joystick** (UsagePage 1 /
  Usage 4) alongside Mouse (Usage 2) + Keyboard (Usage 6) + CDC, at VID 0x1209 / **PID
  0x0004**; a firmware joystick report is observed by the host (e.g. macOS Game
  Controller / a HID monitor); the other classes still work.

Scope C is the last of the three HID classes.

---

## 2. What exists today (post-Mouse composite)

- `usb_desc.h`: `PRODUCT_ID 0x0003`, `NUM_ENDPOINTS 6`, `NUM_INTERFACE 4`,
  `CONFIG_DESC_SIZE 125`. CDC iface 0/1 (EP2/3/4), Keyboard iface 2 (EP5), Mouse iface 3
  (EP6), `MOUSE_HID_DESC_OFFSET 109`.
- `usb.c`: `SET_CONFIGURATION` arms EP2–6 + `usb_serial/keyboard/mouse_configure()`;
  `usb_descriptor_buffer[CONFIG_DESC_SIZE]` staging buffer.
- `usb_keyboard.c`, `usb_mouse.c` present; `usb_joystick.h` vendored; `usb_joystick.c`
  **not** present.
- QEMU: chipidea_cdc.c parses **all** HID interrupt-INs into `hid_in_mask` and
  `ci_udc_service_prime` taps any of them reactively (Scope B). **This already covers
  EP7** — mask becomes `0xE0` (EP5|EP6|EP7).

---

## 3. Locked decisions

1. **Verbatim `usb_joystick.c` port** + the same 3 deltas as keyboard/mouse
   (`DMAMEM tx_transfer`, drop `printf`, drop `debug/printf.h`).
2. **`JOYSTICK_SIZE 12`** — normal joystick (32 buttons + 6 10-bit axes + hat), not the
   64-byte "extreme" (128-button) variant. Matches teensy4 `USB_SERIAL_HID`.
3. **Bump `PRODUCT_ID` 0x0003 → 0x0004** (new composite shape → macOS descriptor re-read).
4. **No QEMU changes** — the reactive `hid_in_mask` tap already captures EP7.

**Licensing:** `usb_joystick.{c,h}` are PJRC modified-MIT (same family; not copyleft).

---

## 4. Design

### 4.1 Descriptor surgery — `usb_desc.h` / `usb_desc.c`

**`usb_desc.h`:**
- `PRODUCT_ID 0x0003 → 0x0004`; `NUM_ENDPOINTS 6 → 7`; `NUM_INTERFACE 4 → 5`;
  `CONFIG_DESC_SIZE 125 → 150` (+25).
- New defines: `JOYSTICK_INTERFACE 4`, `JOYSTICK_ENDPOINT 7`, `JOYSTICK_SIZE 12`,
  `JOYSTICK_INTERVAL 1`, `JOYSTICK_HID_DESC_OFFSET 134` (= 125 end-of-mouse-block + 9).
- `ENDPOINT7_CONFIG = (ENDPOINT_RECEIVE_UNUSED | ENDPOINT_TRANSMIT_INTERRUPT)`.

**`usb_desc.c`:**
- `config_descriptor[]`: total-length → `LSB/MSB(150)`; `bNumInterfaces 4 → 5`; **append**
  after the mouse endpoint the 25-byte joystick block, verbatim from teensy4
  `usb_desc.c` (interface is **non-boot**, subclass/protocol `0x00`):
  ```c
  /* joystick interface (9) */
  9, 4, JOYSTICK_INTERFACE, 0, 1, 0x03, 0x00, 0x00, 0,
  /* HID descriptor (9) */
  9, 0x21, 0x11, 0x01, 0, 1, 0x22,
      LSB(sizeof(joystick_report_desc)), MSB(sizeof(joystick_report_desc)),
  /* joystick endpoint, EP7 IN (7) */
  7, 5, JOYSTICK_ENDPOINT | 0x80, 0x03, JOYSTICK_SIZE, 0, JOYSTICK_INTERVAL
  ```
- Add `joystick_report_desc[]` = the **`JOYSTICK_SIZE==12`** variant (~85 bytes, teensy4
  `usb_desc.c:296-339`; no REPORT_ID), guarded by `#if defined(JOYSTICK_INTERFACE)`,
  above `config_descriptor`. **Note:** teensy4 has two `#if JOYSTICK_SIZE`-guarded
  definitions — copy the **12** one only.
- Two new `usb_descriptor_list[]` entries: `{0x2200, JOYSTICK_INTERFACE, joystick_report_desc, sizeof(...)}`
  and `{0x2100, JOYSTICK_INTERFACE, config_descriptor + JOYSTICK_HID_DESC_OFFSET, 9}`.

**Buffer check (done):** `sizeof(joystick_report_desc)` ≈ 85 < `CONFIG_DESC_SIZE` 150, so
`usb_descriptor_buffer[CONFIG_DESC_SIZE]` in `usb.c` still holds every served descriptor —
**no buffer resize needed**.

### 4.2 EP0 engine — `usb.c`

- `SET_CONFIGURATION`: add `#if defined(ENDPOINT7_CONFIG) USB1_ENDPTCTRL7 = ENDPOINT7_CONFIG; #endif`
  and a guarded `usb_joystick_configure();` (mirroring the EP6/mouse block). Add
  `#if defined(JOYSTICK_INTERFACE) void usb_joystick_configure(void); #endif` to the extern block.
- No `SET_REPORT`. `GET_DESCRIPTOR(report)` served by the list-walk (wIndex = JOYSTICK_INTERFACE).

### 4.3 Backend — new `usb_joystick.c` + globals

Port teensy4 `usb_joystick.c` **verbatim except** the 3 deltas:
- `static transfer_t tx_transfer[TX_NUM]` (teensy4:56) → **`DMAMEM static transfer_t …`**.
- Comment out `printf("ERROR status …")` (teensy4:83); drop `#include "debug/printf.h"`.

`usb_inst.cpp`: add **both** (the class has a static member needing a definition):
```cpp
#if defined(JOYSTICK_INTERFACE)
usb_joystick_class Joystick;
uint8_t usb_joystick_class::manual_mode = 0;
#endif
```
`Arduino.h`: add `#include "usb_joystick.h"` (surfaces `Joystick`).
**file(GLOB):** new `usb_joystick.c` → fresh `rm -rf build && cmake -B build`.

### 4.4 QEMU — unchanged

The Scope-B reactive tap (`hid_in_mask` + `ci_udc_service_prime`) already captures any HID
interrupt-IN. The parser will record EP7 into the mask automatically (`bInterfaceClass 0x03`);
the joystick gate sends only joystick reports → EP7 tapped. **No QEMU edits.** The only
verification is re-running the gates (mask becomes `0xE0`).

### 4.5 QEMU gate — new `evkb/usb_joystick_test/` (copy of `usb_mouse_test/`)

- **Sketch:** wait for `usb_configuration`, then
  `Joystick.useManualSend(true); Joystick.button(1,1); Joystick.X(512); Joystick.send_now();`.
  (Manual send → one combined report; `button(1,1)`→`data[0]=0x1`, `X(512)`→`data[1]=0x2000`.)
- **Runner** `run_qemu_usb_joystick.sh`: copy the mouse runner (keep the `set +e/-e` wrap);
  `id=usbhid-tap`, fresh PORT (e.g. 14558), ELF `usb_joystick_test.elf`.
- **Driver** `usb_joystick_driver.py`: read one **12-byte** report; assert
  `01 00 00 00 00 20 00 00 00 00 00 00`. Exit 0/1/2.
- **Regressions:** re-run `usb_mouse_test`, `usb_keyboard_test`, `usb_data_test` — all green.

### 4.6 Hardware verification

Flash; confirm `hidutil list` shows a **Joystick** (UsagePage 1 / Usage 4) alongside
Mouse/Keyboard, and `system_profiler` shows PID 0x0004 + CDC. Flash a periodic-report HW
variant (mirror `usb_mouse_hw`: toggle button 1 / sweep X every ~1 s) and observe it in
macOS (e.g. a HID monitor / Game Controller test); confirm Mouse+Keyboard+CDC still work.

---

## 5. File manifest

| Repo | Files |
|---|---|
| **core** | `usb_desc.h`, `usb_desc.c`, `usb.c`, **new** `usb_joystick.c`, `usb_inst.cpp`, `Arduino.h` |
| **qemu2** | *(none — reactive tap already handles EP7)* |
| **evkb** | **new** `usb_joystick_test/` (sketch, runner, driver, CMake, toolchain) |

---

## 6. Risks & mitigations

1. **DMA-unreachable dTDs** → `DMAMEM tx_transfer[]` (§4.3).
2. **`manual_mode` static member** undefined → add the definition in `usb_inst.cpp` (§4.3);
   a linker "undefined reference" if forgotten.
3. **Two `joystick_report_desc` variants in teensy4** → copy the `JOYSTICK_SIZE==12` one.
4. **macOS descriptor cache** → PID 0x0004 (§3.3).
5. **Regressions** (Mouse/Keyboard/CDC) from the 5-iface descriptor → explicit re-run (§4.5).
6. **`file(GLOB)`** for new `usb_joystick.c` → fresh reconfigure.
7. **QEMU false-pass** → HW arbiter (host sees the joystick report).

---

## 7. Deferred (YAGNI)

64-byte "extreme" joystick (128 buttons / 16-bit axes); the absolute/analog axis richness
beyond a single button+X in the gate. This completes the Keyboard/Mouse/Joystick trio; any
further USB-device polish (RawHID, SEREMU, MIDI-device, the USB-Type build system) stays
deferred per the project scope.

---

## 8. Reference line-number index

- teensy4 `usb_desc.c:296-339` — `joystick_report_desc[]` (JOYSTICK_SIZE==12, ~85 B, no REPORT_ID).
- teensy4 `usb_desc.c` (joystick block in the config builder) — interface (non-boot 0x00/0x00) + HID + EP7.
- teensy4 `usb_joystick.c` — backend; `:56` tx_transfer, `:83` printf, `:72` usb_joystick_send (sends 12-byte `usb_joystick_data`).
- `usb_joystick.h:59-123` — `usb_joystick_class` (button/X/Y/Z/hat/…, `manual_mode`, `send_now`).
- teensy4 `usb_inst.cpp:79-80` — `Joystick` + `usb_joystick_class::manual_mode = 0`.
- our `usb_desc.h` — current post-mouse defines (PID 0x0003, EP6/iface3, OFFSET 109).
