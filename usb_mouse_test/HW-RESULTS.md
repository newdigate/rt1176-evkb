# USB device HID Mouse (composite CDC + Keyboard + Mouse) — Hardware Results

**Date:** 2026-07-14
**Board:** MIMXRT1176-EVKB — device on **USB_OTG1**, host = Mac (Darwin 22.6.0)
**Firmware:** `usb_mouse_test` (one-shot `Mouse.move` gate) + `usb_mouse_hw` (periodic
cursor jiggle + CDC echo, for HW observation).

## Results — ALL PASS ✅

### 1. Composite enumeration
`system_profiler SPUSBDataType`: **"USB Serial"**, **VID 0x1209 / PID 0x0003**, 480 Mb/s.

`hidutil list` (both HID interfaces present under one device):
- `0x1209 0x3 … UsagePage 1 Usage 2` = **Mouse**
- `0x1209 0x3 … UsagePage 1 Usage 6` = **Keyboard**

CDC node: **`/dev/cu.usbmodem145301`**.

→ Mouse **and** Keyboard **and** CDC, all under one composite at 1209:0003.

### 2. Cursor moves
`usb_mouse_hw` jiggles the cursor ~30 px right↔left every ~1 s; the cursor visibly
twitched on the Mac (user-confirmed). A firmware `Mouse.move` moves the host cursor.

### 3. Concurrent CDC
Over `/dev/cu.usbmodem145301`, wrote `CDC-alongside-mouse-and-keyboard` and received it
back **byte-exact**, while the mouse + keyboard interfaces were active.

## Notes
- **PID bump 0x0002 → 0x0003** for the new composite shape (forces a macOS descriptor re-read).
- Mouse report on **interrupt-IN EP6**, report-ID 1 relative: `01 <buttons> <dx> <dy> <wheel> <horiz>`.
- Mouse interface is **non-boot** (subclass/protocol 0x00), unlike the keyboard's 0x01/0x01.

## QEMU gates (for reference)
- `usb_mouse_test`: **PASS** — `Mouse.move(10,5)` = `01 00 0A 05 00 00` captured off EP6
  (reactive tap, `hid_in_mask=0x60` = EP5|EP6).
- `usb_keyboard_test` + `usb_data_test` regressions: **PASS**.
