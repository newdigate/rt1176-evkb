# RT1176 USB device HID — Scope B: add Mouse to the composite

**Date:** 2026-07-14
**Status:** design approved, ready for `writing-plans`
**Builds on:** `rt1176-usb-device-hid-keyboard` (Scope A, HW-verified). Appends a Mouse
interface to the existing CDC + Keyboard composite.

---

## 1. Goal & success criteria

Add a HID **Mouse** interface to the composite (now CDC + Keyboard + **Mouse**),
driven by the Teensy-style `Mouse.move()/click()/scroll()` Arduino global, with CDC
Serial and the Keyboard still working.

**Success:**
- **QEMU gate** (`evkb/usb_mouse_test/`): a firmware `Mouse.move(10, 5)` is captured on
  the mouse interrupt-IN (EP6) and the runner asserts the exact report bytes
  **`01 00 0A 05 00 00`** (report-ID 1, buttons 0, dx 10, dy 5, wheel 0, horiz 0).
  Fail-first, then pass.
- **Regressions:** the Keyboard gate (`usb_keyboard_test`) and the CDC gate
  (`usb_data_test`) both still pass against the 4-interface composite.
- **Hardware:** `system_profiler` / `hidutil list` show a HID **Mouse** (UsagePage 1 /
  Usage 2) **and** Keyboard **and** CDC at VID 0x1209 / **PID 0x0003**; a firmware
  `Mouse.move` visibly moves the Mac cursor; Keyboard + CDC still work.

Scope B is **Mouse only**. Joystick (C) is a later cycle.

---

## 2. What exists today (post-Keyboard composite)

- `usb_desc.h`: `PRODUCT_ID 0x0002`, `NUM_ENDPOINTS 5`, `NUM_INTERFACE 3`,
  `CONFIG_DESC_SIZE 100`, device class 0xEF/0x02/0x01 (IAD). CDC iface 0/1 (EP2/3/4),
  Keyboard iface 2 (EP5), `KEYBOARD_HID_DESC_OFFSET 84`.
- `usb.c`: EP0 engine; `SET_CONFIGURATION` arms EP2/3/4/5 + `usb_serial_configure()` +
  `usb_keyboard_configure()`; HID `SET_REPORT` (keyboard LEDs); `GET_DESCRIPTOR` served
  by the list-walk.
- `usb_keyboard.c` present (verbatim + 3 deltas). `usb_mouse.h` already vendored;
  `usb_mouse.c` **not** present.
- QEMU: chipidea_cdc.c composite host parses one HID interrupt-IN into `int_in_ep`
  (first HID int-IN, gated on `bInterfaceClass 0x03`), arms it when `hid_host`, forwards
  reports to `usbhid-tap` and re-arms. `ci_complete_endpoint` already dispatches to
  `ci_cdc_on_complete` for `cdc_host || hid_host`.

---

## 3. Locked decisions

1. **Verbatim `usb_mouse.c` port** + the same 3 precedented deltas as the keyboard
   (`DMAMEM tx_transfer`, drop `printf`, drop `debug/printf.h`). Gives the full `Mouse`
   API (move/moveTo/click/scroll/press/release).
2. **Bump `PRODUCT_ID` 0x0002 → 0x0003.** The composite shape changes (new interface +
   endpoint); a fresh PID forces a clean macOS descriptor re-read.
3. **QEMU: reactively tap ALL HID interrupt-INs** (supersede the single `int_in_ep`).
   Two HID interrupt-INs now exist (keyboard EP5, mouse EP6); the parser records a
   bitmask `hid_in_mask`, and whenever the guest primes any HID interrupt-IN while
   `hid_host` is set, the host services + forwards it. Each gate's sketch sends only its
   own class, so only that report appears on the tap. Extends to Joystick EP7 for free.

**Licensing:** unchanged — `usb_mouse.{c,h}` are PJRC modified-MIT (same family as the
shipped core; not copyleft). QEMU stays GPL test-harness.

---

## 4. Design

### 4.1 Descriptor surgery — `usb_desc.h` / `usb_desc.c`

**`usb_desc.h`:**
- `PRODUCT_ID 0x0002 → 0x0003`; `NUM_ENDPOINTS 5 → 6`; `NUM_INTERFACE 3 → 4`;
  `CONFIG_DESC_SIZE 100 → 125` (+25 = 9 iface + 9 HID + 7 endpoint).
- New defines: `MOUSE_INTERFACE 3`, `MOUSE_ENDPOINT 6`, `MOUSE_SIZE 8`,
  `MOUSE_INTERVAL 2`, `MOUSE_HID_DESC_OFFSET 109` (= 100 end-of-keyboard-block + 9).
- `ENDPOINT6_CONFIG = (ENDPOINT_RECEIVE_UNUSED | ENDPOINT_TRANSMIT_INTERRUPT)`.

**`usb_desc.c`:**
- `config_descriptor[]`: total-length bytes → `LSB/MSB(125)`; `bNumInterfaces 3 → 4`;
  **append** after the keyboard endpoint the 25-byte mouse block, verbatim from teensy4
  `usb_desc.c:1182-1210`, substituting `MOUSE_INTERFACE=3`, `MOUSE_ENDPOINT|0x80=0x86`,
  `MOUSE_SIZE=8`, `MOUSE_INTERVAL=2`. **Note: mouse is NON-boot** —
  `bInterfaceSubClass=0x00`, `bInterfaceProtocol=0x00` (keyboard used 0x01/0x01):
  ```c
  /* mouse interface (9) */
  9, 4, MOUSE_INTERFACE, 0, 1, 0x03, 0x00, 0x00, 0,
  /* HID descriptor (9) */
  9, 0x21, 0x11, 0x01, 0, 1, 0x22,
      LSB(sizeof(mouse_report_desc)), MSB(sizeof(mouse_report_desc)),
  /* mouse endpoint, EP6 IN (7) */
  7, 5, MOUSE_ENDPOINT | 0x80, 0x03, MOUSE_SIZE, 0, MOUSE_INTERVAL
  ```
- Add `mouse_report_desc[]` = **84 bytes**, verbatim from teensy4 `usb_desc.c:248`
  (REPORT_ID 1 relative buttons+X/Y/wheel/AC-pan; REPORT_ID 2 absolute), guarded by
  `#if defined(MOUSE_INTERFACE)`, placed above `config_descriptor` (so `sizeof` is in
  scope).
- Two new `usb_descriptor_list[]` entries:
  ```c
  {0x2200, MOUSE_INTERFACE, mouse_report_desc, sizeof(mouse_report_desc)},
  {0x2100, MOUSE_INTERFACE, config_descriptor + MOUSE_HID_DESC_OFFSET, 9},
  ```
  Served by the existing EP0 0x0680/0x0681 list-walk — no new EP0 code.

### 4.2 EP0 engine — `usb.c`

- `SET_CONFIGURATION`: add `#if defined(ENDPOINT6_CONFIG) USB1_ENDPTCTRL6 = ENDPOINT6_CONFIG; #endif`
  and a guarded `usb_mouse_configure();` (mirroring the EP5/keyboard block).
- **No `SET_REPORT`** for mouse (no output/LED report). `GET_DESCRIPTOR(report)` for the
  mouse — unchanged, served by the list-walk (wIndex = MOUSE_INTERFACE).

### 4.3 Backend — new `usb_mouse.c` + globals

Port teensy4 `usb_mouse.c` **verbatim except** the 3 precedented deltas:
- `static transfer_t tx_transfer[TX_NUM]` (teensy4:99) → **`DMAMEM static transfer_t …`**.
- Comment out the `printf("ERROR status …")` (teensy4:147); drop `#include "debug/printf.h"`.
- (`txbuffer` at :100 is already `DMAMEM`.)

`usb_inst.cpp`: add `#if defined(MOUSE_INTERFACE) usb_mouse_class Mouse; #endif`.
`Arduino.h`: add `#include "usb_mouse.h"` (surfaces `Mouse` + `MOUSE_LEFT` etc.), mirror
the `usb_keyboard.h` include.
**file(GLOB):** the new `usb_mouse.c` needs a from-scratch `rm -rf build && cmake -B build`.

### 4.4 QEMU — reactive tap of all HID interrupt-INs

Generalize the single `int_in_ep` (keyboard-era) to a bitmask, and tap reactively on
prime (no pre-arm/re-arm dance):
- **`chipidea.h`** (`CIUDCState`): replace `int int_in_ep;` with `uint32_t hid_in_mask;`
  (bit N set = endpoint N is a HID interrupt-IN).
- **`chipidea_cdc.c`**:
  - `cdc_parse_endpoints`: reset `hid_in_mask = 0`; for each interrupt-IN inside a HID
    interface (`cur_iface_class == 0x03`), `hid_in_mask |= (1u << (addr & 0x0f))` (no
    longer first-wins). Update the `CLOG` to print the mask.
  - `cdc_enter_run`: **drop** the HID pre-arm (`ci_udc_arm_bulk_in(int_in_ep)`); keep the
    CDC bulk-IN pre-arm. HID is now serviced reactively on prime.
  - `hid_on_int_in`: forward `vh_in` → `hid_be`; **no re-arm** (reactive).
  - `ci_cdc_on_complete` route: `else if (dir == 1 && (ci->udc.hid_in_mask & (1u << ep)))
    → hid_on_int_in(ci, len)`.
- **`chipidea_udc.c`** `ci_udc_service_prime` — add one term to the `ready` test (line 282):
  ```c
  bool ready = (dir && ci->udc.vh_in_pending  && ci->udc.vh_in_ep  == ep) ||
               (!dir && ci->udc.vh_out_pending && ci->udc.vh_out_ep == ep) ||
               (dir && ci->hid_host && (ci->udc.hid_in_mask & (1u << ep)));
  ```
  So when the guest primes a HID interrupt-IN under `hid_host`, `ci_complete_endpoint`
  runs immediately → copies the report to `vh_in` → dispatches to `ci_cdc_on_complete`
  (already wired for `hid_host`) → `hid_on_int_in` taps it.

**Why reactive is safe:** HID reports are only primed *after* SET_CONFIG (the sketch
waits on `usb_configuration`), so `service_prime` during RUN catches them; the keyboard
press+release still works (its `delay(10)` un-primes EP5 between the two, so each
re-primes → each is serviced). `hid_host=false` (CDC gate) makes the new term false —
CDC path unchanged. The single `vh_in` slot is used transiently per report (copy → tap
synchronously) — no collision, so multiple HID endpoints coexist.
- **`fsl-imxrt1170.c`**: unchanged (the `usbhid-tap` chardev already binds).

### 4.5 QEMU gate — new `evkb/usb_mouse_test/` (copy of `usb_keyboard_test/`)

- **Sketch:** wait for `usb_configuration`, then `delay(100); Mouse.move(10, 5);`.
- **Runner** `run_qemu_usb_mouse.sh`: copy the keyboard runner (incl. the `set +e/-e`
  wrap around the python driver); `-chardev socket,id=usbhid-tap,…` on a fresh PORT
  (e.g. 14557).
- **Driver** `usb_mouse_driver.py`: read one 6-byte report off the tap; assert
  `01 00 0A 05 00 00`. Exit 0/1/2.
- **Regressions:** re-run `usb_keyboard_test` and `usb_data_test` — both must stay green
  against the 4-interface composite (keyboard reactive-tap works; CDC unaffected).

### 4.6 Hardware verification

Flash; on the Mac confirm `hidutil list` shows a **Mouse** (UsagePage 1 / Usage 2)
alongside the Keyboard, and `system_profiler` shows CDC, all at VID 0x1209 / PID 0x0003.
Flash a periodic-`Mouse.move` HW variant (mirror `usb_keyboard_hw`) and watch the cursor
drift; confirm Keyboard still types and CDC still echoes.

---

## 5. File manifest

| Repo | Files |
|---|---|
| **core** | `usb_desc.h`, `usb_desc.c`, `usb.c`, **new** `usb_mouse.c`, `usb_inst.cpp`, `Arduino.h` |
| **qemu2** | `include/hw/usb/chipidea.h`, `hw/usb/chipidea_cdc.c`, `hw/usb/chipidea_udc.c` (fsl-imxrt1170.c unchanged) |
| **evkb** | **new** `usb_mouse_test/` (sketch, runner, driver, CMake, toolchain) |

---

## 6. Risks & mitigations

1. **Two HID int-INs → wrong endpoint tapped** — the whole point of §4.4; the reactive
   `hid_in_mask` taps whichever the guest sends on. Keyboard-gate regression proves it.
2. **DMA-unreachable dTDs** → `DMAMEM tx_transfer[]` (§4.3).
3. **macOS descriptor cache** → PID 0x0003 (§3.2).
4. **Keyboard/CDC regression** from the reactive-tap refactor + 4-iface descriptor →
   explicit re-run of both gates (§4.5) must stay green.
5. **Mouse non-boot subclass/protocol** — use `0x00/0x00`, NOT keyboard's `0x01/0x01` (§4.1).
6. **`file(GLOB)`** for the new `usb_mouse.c` → fresh reconfigure.
7. **QEMU false-pass** → HW arbiter (cursor visibly moves).

---

## 7. Deferred (YAGNI)

Absolute-mode mouse (REPORT_ID 2 / `Mouse.moveTo`) is compiled in (verbatim port) but not
gate-tested; consumer/AC-pan wheel not gate-tested. Joystick (C) is its own cycle.

---

## 8. Reference line-number index

- teensy4 `usb_desc.c:248` — `mouse_report_desc[]` (84 B, REPORT_ID 1 + 2).
- teensy4 `usb_desc.c:1182-1210` — mouse interface + HID + endpoint bytes (subclass/proto 0x00).
- teensy4 `usb_mouse.c` — backend to port; `:99` tx_transfer, `:147` printf, `:175` usb_mouse_move.
- `usb_mouse.h:65-105` — `usb_mouse_class` (move/click/scroll/…) + `extern Mouse`.
- our `usb_desc.h` — current post-keyboard defines (PID 0x0002, EP5/iface2, OFFSET 84).
- QEMU `chipidea_udc.c:154-257` (`ci_complete_endpoint`, dispatches at :252), `:259-291`
  (`ci_udc_service_prime`, `ready` at :282); `chipidea_cdc.c` parse/enter_run/on_complete/hid_on_int_in.
