# RT1176 USB **device** HID — Scope A: composite CDC + Keyboard

**Date:** 2026-07-14
**Status:** design approved, ready for `writing-plans`
**Builds on:** `rt1176-serialusb` (USB device CDC Serial, Phases 1+2, HW-verified). Inverse of `rt1176-usb-host-hid` (there we were the host; here we are the keyboard).

---

## 1. Goal & success criteria

Make the EVKB present to a host PC as a **composite CDC + HID Keyboard**, driven by the Teensy-style `Keyboard.press()/release()/print()` Arduino globals, **with CDC `Serial` still alive alongside**.

**Success:**
- **QEMU gate** (`evkb/usb_keyboard_test/`): the emulated host polls the keyboard's interrupt-IN endpoint; a firmware-sent `KEY_A` press+release is captured and the runner asserts the exact report bytes — press `00 00 04 00 00 00 00 00`, then release `00 00 00 00 00 00 00 00`. Fail-first, then pass.
- **Regression:** the existing `evkb/usb_data_test` CDC-echo gate still passes against the now-composite descriptor (proves CDC still enumerates + bulk-drains).
- **Hardware (the real arbiter — the Mac is the host):** `system_profiler SPUSBDataType` shows a HID Keyboard **and** the CDC serial node under VID `0x1209` / PID `0x0002`; a firmware keystroke lands in a focused text field; `Serial` echoes over CDC concurrently.

This cycle is **Keyboard only**. Mouse (B) and Joystick (C) are separate later brainstorm→spec→plan→implement cycles that *append* to this same composite.

---

## 2. What exists today (the CDC-only baseline this extends)

- `cores/imxrt1176/usb.c` — EP0 engine + dQH/dTD scheduler, near-verbatim teensy4 port, CDC-trimmed. **Already** handles `GET_DESCRIPTOR` cases `0x0680`/`0x0681` via a `usb_descriptor_list[]` walk, and `SET_CONFIGURATION` writes `ENDPOINT2/3/4_CONFIG` + calls `usb_serial_configure()`. Scheduler primitives (`usb_transmit`, `usb_config_tx`, `usb_prepare_transfer`, `schedule_transfer`, `run_callbacks`) present and DMAMEM-correct.
- `cores/imxrt1176/usb_desc.{h,c}` — single hardcoded CDC-ACM config: `NUM_ENDPOINTS 4`, `NUM_INTERFACE 2`, EP2 ACM / EP3 bulk-OUT / EP4 bulk-IN, `CONFIG_DESC_SIZE 75`, device class `0/0/0`, VID `0x1209` / PID `0x0001`.
- `cores/imxrt1176/usb_serial.c` — CDC `Serial` rings; **HW-verified**. Its `tx_transfer[]`/`rx_transfer[]` dTDs are `DMAMEM` (usb_serial.c:61,68) — the proven RT1176 DMA-reachability pattern. All debug `printf` are commented out (no `debug/printf.h` wired).
- `cores/imxrt1176/usb_inst.cpp` — instantiates `Serial` / `SerialUSB`.
- **Already vendored + committed, license-clean (PJRC MIT):** `usb_keyboard.h` (byte-identical to teensy4), `keylayouts.h`, `keylayouts.c`, plus the other HID headers (`usb_mouse.h`, `usb_joystick.h`, …) — all inert behind their `*_INTERFACE` guards. `usb_keyboard.c` is **not** present.
- QEMU: `qemu2/hw/usb/chipidea_cdc.c` (in-QEMU CDC host that enumerates the gadget + bridges bulk to the `usbcdc` chardev), `chipidea_udc.c` (dQH/dTD device engine, **transfer-type-agnostic**), `chipidea.{c,h}`, `hw/arm/fsl-imxrt1170.c` (binds the `usbcdc` chardev to USB_OTG1).

---

## 3. Locked decisions

1. **Hardcode one growing composite** (CDC + Keyboard), **not** teensy4's `#if defined(USB_*)` "USB Type" build-variant system. A/B/C extend one composite; the variant switch is YAGNI. Mirrors how CDC-only was built.
2. **Bump `PRODUCT_ID` `0x0001` → `0x0002`.** macOS caches USB descriptors by VID:PID; a fresh PID forces a clean re-read of the new composite descriptor (avoids a stale-cache HW-debug trap).
3. **Port `SET_REPORT` (0x0921)** + its completion branch (keyboard-LED output report → `keyboard_leds`). Faithful to teensy4; not strictly required (STALL is tolerated) but well-behaved. `SET_IDLE`/`SET_PROTOCOL`/`GET_REPORT` stay unimplemented (STALL-tolerated, exactly as teensy4).
4. **Keyboard backend = verbatim `usb_keyboard.c` port** (keylayouts already vendored) → full `Keyboard.print("hi")` + raw `Keyboard.press(KEY_A)`. Define `LAYOUT_US_ENGLISH`.

**Licensing:** all-clear on copyleft (audited). The added core files are PJRC modified-MIT (same family as the shipped `usb_serial.c`; `license-audit.sh` only rejects GPL/LGPL). `keylayouts.h` is solely PJRC-copyright, no embedded third-party code. QEMU is GPLv2 but it's the emulator/test-harness — a separate binary that *runs* the firmware, never linked into it (the audit's depfile walk proves nothing copyleft feeds a firmware object). New gate lives in `evkb/` (local, un-distributed).

---

## 4. Design

### 4.1 Descriptor surgery — `usb_desc.h` / `usb_desc.c`

**`usb_desc.h`:**
- `PRODUCT_ID 0x0001 → 0x0002`; `NUM_ENDPOINTS 4 → 5`; `NUM_INTERFACE 2 → 3`; `CONFIG_DESC_SIZE 75 → 100`.
- New defines: `KEYBOARD_INTERFACE 2`, `KEYBOARD_ENDPOINT 5`, `KEYBOARD_SIZE 8`, `KEYBOARD_INTERVAL 1`, `KEYBOARD_HID_DESC_OFFSET 84` (= 75 end-of-CDC + 9 keyboard-interface-desc).
- `ENDPOINT5_CONFIG = (ENDPOINT_RECEIVE_UNUSED | ENDPOINT_TRANSMIT_INTERRUPT)` (0x00CC0002) — the `ENDPOINT_*` macros already exist.
- Add `#define LAYOUT_US_ENGLISH` near the top (before any keylayouts.h inclusion). Include-order is safe: `usb_keyboard.c` pulls `usb_keyboard.h` → `usb_desc.h` **before** `keylayouts.h`. (Build-flag `-DLAYOUT_US_ENGLISH` is the more-robust alternative but needs a CMake edit; the header `#define` avoids the `file(GLOB)` reconfigure churn.)

**`usb_desc.c`:**
- Device descriptor: class/sub/proto bytes `0,0,0 → 0xEF,0x02,0x01` (Misc/IAD composite). PID follows `PRODUCT_ID`.
- `config_descriptor[]`: total-length bytes → `LSB/MSB(100)`; `bNumInterfaces 2 → 3`; **append** after the EP4-IN endpoint the 25-byte keyboard block, verbatim from teensy4 `usb_desc.c:1152-1180` (interface 9 + HID 9 + endpoint 7), substituting `KEYBOARD_INTERFACE=2`, `KEYBOARD_ENDPOINT|0x80 = 0x85`, `KEYBOARD_SIZE=8`, `KEYBOARD_INTERVAL=1`:
  ```c
  /* keyboard interface (9) */
  9, 4, KEYBOARD_INTERFACE, 0, 1, 0x03, 0x01, 0x01, 0,
  /* HID descriptor (9) */
  9, 0x21, 0x11, 0x01, 0, 1, 0x22,
      LSB(sizeof(keyboard_report_desc)), MSB(sizeof(keyboard_report_desc)),
  /* keyboard endpoint, EP5 IN (7) */
  7, 5, KEYBOARD_ENDPOINT | 0x80, 0x03, KEYBOARD_SIZE, 0, KEYBOARD_INTERVAL
  ```
- Add `keyboard_report_desc[]` = **63 bytes**, the `KEYBOARD_SIZE==8` boot-protocol branch, verbatim from teensy4 `usb_desc.c:173-219` (guarded `#if defined(KEYBOARD_INTERFACE)`).
- Two new `usb_descriptor_list[]` entries (order irrelevant — the walk scans to first match):
  ```c
  {0x2200, KEYBOARD_INTERFACE, keyboard_report_desc, sizeof(keyboard_report_desc)},
  {0x2100, KEYBOARD_INTERFACE, config_descriptor + KEYBOARD_HID_DESC_OFFSET, 9},
  ```
  `0x2200` (report descriptor) is what the host GET_DESCRIPTOR(report)=`0x0681`/wValue `0x2200` fetches — served by the **existing** EP0 walk, **no new EP0 code**. `0x2100` (HID descriptor standalone) is optional-but-faithful.

`usb.c`'s `usb_descriptor_buffer[CONFIG_DESC_SIZE]` staging buffer auto-grows to 100; the 63-byte report descriptor fits.

### 4.2 EP0 engine — `usb.c`

1. **`SET_CONFIGURATION` (case 0x0900):** after the `ENDPOINT4_CONFIG` block add
   ```c
   #if defined(ENDPOINT5_CONFIG)
   USB1_ENDPTCTRL5 = ENDPOINT5_CONFIG;
   #endif
   ```
   and after `usb_serial_configure();` add a guarded `usb_keyboard_configure();`.
2. **`SET_REPORT` (new case 0x0921),** guarded `#if defined(KEYBOARD_INTERFACE)`, verbatim from teensy4 `usb.c:637-645`: stage the output report into `endpoint0_buffer` (fits — `endpoint0_buffer[8]`, wLength=1), `endpoint0_receive(endpoint0_buffer, wLength, 1)`.
3. **`endpoint0_complete()`:** add the guarded branch from teensy4 `usb.c:867-871` — on `setup.word1==0x02000921 && setup.word2==((1<<16)|KEYBOARD_INTERFACE)`, `keyboard_leds = endpoint0_buffer[0]; endpoint0_transmit(NULL,0,0);`.
4. **Extern visibility:** `usb.c` currently has no `keyboard_leds`. Add a guarded `extern volatile uint8_t keyboard_leds;` (or `#include "usb_keyboard.h"` under the guard). `keyboard_leds` is defined in `usb_keyboard.c`.
5. `GET_DESCRIPTOR(report)` — **unchanged**, served by the existing `0x0680/0x0681` walk.

### 4.3 Keyboard backend — new `usb_keyboard.c`

Port teensy4 `usb_keyboard.c` **verbatim except** for this small set of deltas — all precedented by the `usb_serial.c` port:
- **(a) DMA reachability:** `static transfer_t tx_transfer[TX_NUM]` (teensy4 line 96) → **`DMAMEM static transfer_t tx_transfer[TX_NUM]`** (RT1176 DTCM is DMA-unreachable; matches usb_serial.c:61). `txbuffer` is already `DMAMEM`.
- **(b) Debug:** comment out the single `printf("ERROR status …")` (teensy4 line 572) and drop `#include "debug/printf.h"` — matching usb_serial.c (no `debug/printf.h` in our core).
- **(c) Includes:** align the DMAMEM/PROGMEM include source with usb_serial.c (verify `avr/pgmspace.h` vs whatever usb_serial.c uses).
- Everything else (the UTF-8→Unicode→keycode pipeline, `usb_keyboard_press/release_*`, `usb_keyboard_send`, `usb_keyboard_transmit` ring, globals `keyboard_modifier_keys`/`keyboard_keys[6]`/`keyboard_leds`/…) is byte-for-byte.

### 4.4 Arduino globals & wiring

- **`usb_inst.cpp`:** add `#if defined(KEYBOARD_INTERFACE) usb_keyboard_class Keyboard; #endif` (mirrors the `Serial` instantiation).
- **`Arduino.h`:** currently includes `usb_serial.h` but **not** `usb_keyboard.h` → sketches can't see `Keyboard`/`KEY_*`. Add a guarded `#include "usb_keyboard.h"` (mirror the usb_serial.h inclusion). This is required for the gate sketch to compile.
- **`file(GLOB)` trap:** the new `usb_keyboard.c` compilation unit means each consuming gate dir needs a from-scratch `rm -rf build && cmake -B build …`, not just a rebuild.

### 4.5 QEMU model — interrupt-IN, one-tap-per-gate

Extend the existing ChipIdea CDC host into a **CDC+HID composite host** (purely additive; the dQH/dTD engine is transfer-type-agnostic, so no engine primitive is added — `ci_udc_arm_bulk_in(ci, ep)` polls an interrupt-IN endpoint identically to bulk-IN).

- **`chipidea.h`** (`include/hw/usb/chipidea.h`): add `CharBackend hid_be; bool hid_host;` to `ChipideaState`; add `int int_in_ep;` to `CIUDCState` alongside `bulk_in_ep`/`bulk_out_ep` (chipidea.h:51).
- **`chipidea.c`:** `DEFINE_PROP_CHR("hid-chardev", ChipideaState, hid_be)` in `chipidea_props`; in `chipidea_realize`, `if (qemu_chr_fe_backend_connected(&ci->hid_be)) { ci->hid_host = true; ci_hid_setup(ci); }` (HID is device→host only — `ci_hid_setup` needs no `can_read`/`read` OUT handlers; it just marks the backend live).
- **`chipidea_udc.c`:** the three CDC-host dispatch sites now fire on `cdc_host || hid_host`:
  - `ci_cdc_start` dispatch @ udc:371 (R_USBCMD RS path),
  - `ci_cdc_on_reset_acked` @ udc:436-444 (setup timer),
  - `ci_cdc_on_complete` @ udc:250-254 (completion).
  `ci_cdc_on_complete` routes by `ep`: `ep==bulk_in_ep` → `cdc_on_bulk_in` (→ `cdc_be`); `ep==int_in_ep` → new `hid_on_int_in` (→ `hid_be`).
- **`chipidea_cdc.c`:**
  - `cdc_parse_endpoints` (chipidea_cdc.c:40-67): in addition to the bulk match, capture the interrupt-IN — `if ((attr&0x3)==0x3 && (addr&0x80) && !int_in_ep) int_in_ep = addr & 0x0f;`.
  - `cdc_enter_run` (chipidea_cdc.c:91-102): `if (cdc_host && bulk_in_ep) ci_udc_arm_bulk_in(ci, bulk_in_ep);` **and** `if (hid_host && int_in_ep) ci_udc_arm_bulk_in(ci, int_in_ep);`. The enumeration script (incl. `SET_CONTROL_LINE_STATE` to CDC iface 0) is **unchanged** — valid for the composite even when only `hid_host` is set.
  - new `hid_on_int_in(ci, len)`: `if (len) qemu_chr_fe_write_all(&ci->hid_be, ci->udc.vh_in, len); ci_udc_arm_bulk_in(ci, ci->udc.int_in_ep);` (forward report + re-arm the standing interrupt-IN).
- **`hw/arm/fsl-imxrt1170.c`:** after the `usbcdc` block (fsl-imxrt1170.c:973-978), mirror the `sai1-tap` precedent (fsl-imxrt1170.c:839):
  ```c
  Chardev *hid = qemu_chr_find("usbhid-tap");
  if (hid) { qdev_prop_set_chr(DEVICE(&s->usb[i]), "hid-chardev", hid); }
  ```

**One-tap-per-gate rule** — sidesteps the UDC's single virtual-host-IN slot (`vh_in_pending`/`vh_in_ep` are scalar), so two standing INs can't be armed at once:
- **Keyboard gate** wires `usbhid-tap` **only** (no `usbcdc`) → `hid_host` true, `cdc_host` false → arms interrupt-IN only. Enumeration still runs (dispatch on `cdc_host||hid_host`); the composite still fully enumerates both interfaces.
- **CDC gate** (`usb_data_test`) is **untouched** → `usbcdc` only → bulk-IN only (regression check that the composite still does CDC).
- Simultaneous CDC+HID data is proven on **hardware**, not in one QEMU gate.

### 4.6 QEMU gate — new `evkb/usb_keyboard_test/` (copy of `usb_data_test/`)

- **Sketch `usb_keyboard_test.cpp`:** `Serial1.begin(115200)`; wait ≤3 s for `usb_configuration`; print `USB=CONFIGURED`/`USB=TIMEOUT`; `delay(100); Keyboard.press(KEY_A); delay(10); Keyboard.release(KEY_A);` → two interrupt-IN report frames. (`delay(10)` lets the press frame drain before the release frame.)
- **Runner `run_qemu_usb_keyboard.sh`:** copy the `usb_data_test` runner (qrun + gate-lib), swap the chardev to `-chardev socket,id=usbhid-tap,host=127.0.0.1,port=$PORT,server=on,wait=off`; verdict = python driver exit code; grep `usb.dbg` for the `CI-CDC`/HID markers as diagnostics.
- **Driver `usb_kbd_driver.py`** (read-only, modeled on `usb_echo_driver.py`): connect to the `usbhid-tap` socket (retry ≤10 s), read two 8-byte frames, assert frame1 `== 00 00 04 00 00 00 00 00` and frame2 `== 00 00 00 00 00 00 00 00`; exit 0/1/2 (match/mismatch/connect-fail).
- **Fail-first:** confirm the driver fails before the core/QEMU changes land, then passes after.

### 4.7 Hardware verification

Flash via LinkServer (`LinkServer run MIMXRT1176:MIMXRT1170-EVKB <elf>`; `pkill -9 -f LinkServer; pkill -9 -f redlinkserv` first). Plug **native-USB** (VID 0x1209) into the Mac.
1. `system_profiler SPUSBDataType` shows a **HID Keyboard** and the **CDC serial** node (PID 0x0002).
2. Focus a text field; a firmware `Keyboard.press/release(KEY_A)` types `a` (or capture via `hidutil`).
3. Concurrently echo bytes over the CDC node — `Serial` still works next to the keyboard.

Keyboard tx runs in **thread context** (`usb_keyboard_transmit` calls `yield()`), not the ISR — but honor the **"never `Serial1.print` from the USB ISR"** deadlock trap in any debug/gate code.

---

## 5. File manifest

| Repo | Files |
|---|---|
| **core** (`evkb/cores/imxrt1176`, git → github `teensy-cores`; commit from inside `cores/`) | `usb_desc.h`, `usb_desc.c`, `usb.c`, **new** `usb_keyboard.c`, `usb_inst.cpp`, `Arduino.h` |
| **qemu2** (`~/Development/qemu2`) | `include/hw/usb/chipidea.h`, `hw/usb/chipidea.c`, `hw/usb/chipidea_udc.c`, `hw/usb/chipidea_cdc.c`, `hw/arm/fsl-imxrt1170.c` |
| **evkb** (gates, local) | **new** `usb_keyboard_test/` (`usb_keyboard_test.cpp`, `run_qemu_usb_keyboard.sh`, `usb_kbd_driver.py`, `CMakeLists.txt`, `toolchain/`) |

---

## 6. Risks & mitigations

1. **DMA-unreachable dTDs** (the classic RT1176 trap) → `DMAMEM tx_transfer[]` (§4.3a); verify placement with a review pass, as on prior peripherals.
2. **macOS descriptor cache** serving the stale CDC-only descriptor → PID bump to 0x0002 (§3.2).
3. **QEMU single vh_in slot** collision → one-tap-per-gate (§4.5).
4. **CDC regression** from the descriptor becoming composite → re-run `usb_data_test` as an explicit gate; it must stay green.
5. **`printf`/`debug/printf.h`** in the verbatim backend won't compile in our core → comment out / drop, per usb_serial.c precedent (§4.3b).
6. **`Arduino.h` / `LAYOUT_US_ENGLISH` / `keyboard_leds` extern** wiring gaps → §4.2.4, §4.4, §4.1.
7. **`file(GLOB)` non-reconfigure** hides the new `usb_keyboard.c` → `rm -rf build && cmake -B build` each gate dir (§4.4).
8. **QEMU false-pass** (a gate built to match my own assumption) → HW is the final arbiter; model what the silicon does, then let gate **and** Mac judge.
9. **Subagent trust** → independently `diff` each ported file against its teensy4 source and re-run the gate myself.

---

## 7. Deferred (YAGNI — noted, not silent)

NKRO 16-byte report; `KEYMEDIA`/consumer keys; RawHID/SEREMU; MTP/MIDI-device USB-Type variants; the build-selectable "USB Type" system; simultaneous CDC+HID *in a single QEMU gate*. Mouse (B) and Joystick (C) are their own later cycles that append to this composite.

---

## 8. Reference line-number index (ground truth = teensy4 + our core)

- teensy4 `usb_desc.h:280-325` — `USB_SERIAL_HID` composite (device class 0xEF/0x02/0x01; endpoint-config-word pattern).
- teensy4 `usb_desc.c:173-219` — `keyboard_report_desc[]` (63 B, KEYBOARD_SIZE==8).
- teensy4 `usb_desc.c:1152-1180` — keyboard interface + HID + endpoint descriptor bytes.
- teensy4 `usb_desc.c:2805-2807` — the `0x2200`/`0x2100` descriptor-list entries.
- teensy4 `usb_keyboard.c` — backend to port (deltas §4.3); `:96` tx_transfer, `:572` printf.
- teensy4 `usb.c:637-645` + `:867-871` — SET_REPORT case + completion branch.
- our `usb_serial.c:61,68` — the DMAMEM dTD pattern; commented-out printf precedent.
- QEMU `chipidea_cdc.c:40-67` (parse), `:91-102` (enter_run), `:150-156` (standing-IN re-arm); `chipidea_udc.c:306-313` (`ci_udc_arm_bulk_in`), `:250-254`/`:371`/`:436-444` (dispatch sites); `fsl-imxrt1170.c:973-978` (usbcdc), `:839` (sai1-tap precedent).
