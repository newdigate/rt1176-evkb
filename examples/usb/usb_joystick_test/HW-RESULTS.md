# USB device HID Joystick (composite CDC + Keyboard + Mouse + Joystick) — Hardware Results

**Date:** 2026-07-14
**Board:** MIMXRT1176-EVKB — device on **USB_OTG1**, host = Mac (Darwin 22.6.0)
**Firmware:** `usb_joystick_test` (one-shot gate) + `usb_joystick_hw` (periodic
button/axis stepping + CDC echo, for HW observation).

## Results — ALL PASS ✅

### 1. Composite enumeration
`system_profiler SPUSBDataType`: **"USB Serial"**, **VID 0x1209 / PID 0x0004**.

`hidutil list` (three HID interfaces under one device):
- `0x1209 0x4 … UsagePage 1 Usage 4` = **Joystick**
- `0x1209 0x4 … UsagePage 1 Usage 2` = **Mouse**
- `0x1209 0x4 … UsagePage 1 Usage 6` = **Keyboard**

CDC node: **`/dev/cu.usbmodem145301`**.

→ Joystick **and** Mouse **and** Keyboard **and** CDC, all under one composite at 1209:0004.

### 2. Joystick report observed
`usb_joystick_hw` steps button 1 + the X axis every ~0.5 s; confirmed live in
**gamepad-tester.com** (button 1 blinking + X axis stepping, user-confirmed). A firmware
joystick report reaches and is interpreted by the host.

### 3. Concurrent CDC
Over `/dev/cu.usbmodem145301`, wrote `CDC-alongside-joystick-mouse-keyboard` and received
it back **byte-exact**, while joystick + mouse + keyboard interfaces were active.

## Notes
- **PID bump 0x0003 → 0x0004** for the new composite shape.
- Joystick report = 12-byte `usb_joystick_data` on **interrupt-IN EP7**, non-boot
  interface, no REPORT_ID.
- The in-app browser (`preview_start`) timed out; the live report was confirmed in the
  user's own browser instead.

## QEMU gates (for reference)
- `usb_joystick_test`: **PASS** — `button(1)+X(512)` = `01 00 00 00 00 20 00 00 00 00 00 00`
  off EP7 (reactive tap, `hid_in_mask=0xe0` = EP5|EP6|EP7).
- `usb_mouse_test` + `usb_keyboard_test` + `usb_data_test` regressions: **PASS**.
- **QEMU unchanged this cycle** — the Scope-B reactive `hid_in_mask` tap already handled EP7.
