# USB device HID Keyboard (composite CDC + Keyboard) — Hardware Results

**Date:** 2026-07-14
**Board:** MIMXRT1176-EVKB — device on **USB_OTG1** (native-USB), host = Mac (Darwin 22.6.0)
**Firmware:** `usb_keyboard_test` (one-shot, the QEMU gate sketch) and `usb_keyboard_hw`
(periodic-type every 2 s + CDC echo, for repeatable HW observation).

## Results — ALL PASS ✅

### 1. Composite enumeration
`system_profiler SPUSBDataType`:
- Device **"USB Serial"**, **Vendor ID 0x1209 / Product ID 0x0002**, Speed up to 480 Mb/s, Manufacturer **RT1176**.

`hidutil list`:
- HID device at **VID 0x1209**, **UsagePage 1 / Usage 6** = Generic Desktop / **Keyboard**.

CDC node: **`/dev/cu.usbmodem145301`** (distinct from the MCU-Link VCOM `usbmodem5DQ2DDHVWO5EI3`).

→ The composite presents a **HID Keyboard AND a CDC serial** interface at once, under one device.

### 2. Keystroke lands on the host
`usb_keyboard_hw` types `a` every 2 s. With a text field focused on the Mac, `a`
appeared repeatedly (user-confirmed). A firmware-emitted HID report is acted on by
the host — the success criterion.

### 3. Concurrent CDC data
Over `/dev/cu.usbmodem145301`, wrote `HELLO-CDC-alongside-keyboard` and received it
back **byte-exact**, while the keyboard interface was active and typing. `Serial`
is alive alongside HID.

## Notes / traps
- **PID bump 0x0001 → 0x0002** forced a clean macOS descriptor re-read for the new
  composite; the device duly enumerated at 0x0002 (no stale CDC-only cache).
- **Flash:** a recurring CMSIS-DAP `Unable to retrieve DAPInfo: Hardware interface
  transfer error` (probe fault, not code) required an **MCU-Link USB (J11) replug**;
  `LinkServer run` succeeded afterward.
- The iProduct string is still "USB Serial" (cosmetic; both interfaces enumerate
  regardless) — optional future polish.

## QEMU gates (for reference)
- `usb_keyboard_test`: **PASS** — interrupt-IN report `00 00 04 00 00 00 00 00`
  (press) then `00 00 00 00 00 00 00 00` (release) captured byte-exact off EP5.
- `usb_data_test` (CDC regression): **PASS** — composite descriptor still enumerates
  CDC + bulk echo.
