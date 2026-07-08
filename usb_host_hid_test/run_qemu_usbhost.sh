#!/bin/sh
# RT1176 USB Host enumeration gate (USB_OTG2 / usbhost.0).
#
# The firmware brings up the ChipIdea EHCI in host mode and enumerates a HID
# device, printing markers over Serial1 (LPUART1 = serial_hd(0) = first -serial):
#   USB_HOST_BEGIN                 after myusb.begin()
#   KBD_CONNECT=<vid>:<pid>        when a keyboard enumerates
#   MOUSE_CONNECT=<vid>:<pid>      when a mouse enumerates
#
# OTG2's EHCI root hub is single-port (portnr=1), so kbd + mouse can't attach at
# once -- we do separate runs. Each device is pinned to the single root port
# with port=1: without it QEMU's convenience auto-hub (nfree==1) inserts a
# full-speed usb-hub ahead of the device, which is not how the real single
# physical root port behaves. The companion-less i.MX EHCI drives HS/FS/LS on
# the root port directly, so we also cover a FULL-speed device (run C,
# usb_version=1) -- the path a real 12 Mbit/s keyboard/mouse takes. -icount
# shift=auto keeps the EHCI frame timer (QEMU_CLOCK_VIRTUAL) deterministic
# against the firmware's polling.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/usb_host_hid_test.elf"
OUT_A="$DIR/usb_kbd.uart"; OUT_B="$DIR/usb_mouse.uart"; OUT_C="$DIR/usb_kbd_fs.uart"
rm -f "$OUT_A" "$OUT_B" "$OUT_C"

# Run A: high-speed keyboard on the OTG2 root port.
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto -display none \
    -serial file:"$OUT_A" -d guest_errors -D "$DIR/usb_kbd.dbg" \
    -device usb-kbd,bus=usbhost.0,port=1 &
P=$!; gate_pid $P; sleep 18; kill $P 2>/dev/null; wait $P 2>/dev/null || true

# Run B: high-speed mouse on the OTG2 root port.
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto -display none \
    -serial file:"$OUT_B" -d guest_errors -D "$DIR/usb_mouse.dbg" \
    -device usb-mouse,bus=usbhost.0,port=1 &
P=$!; gate_pid $P; sleep 18; kill $P 2>/dev/null; wait $P 2>/dev/null || true

# Run C: full-speed keyboard (usb_version=1) -- exercises the companion-less
# FS/LS root-port path a real keyboard/mouse uses.
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto -display none \
    -serial file:"$OUT_C" -d guest_errors -D "$DIR/usb_kbd_fs.dbg" \
    -device usb-kbd,usb_version=1,bus=usbhost.0,port=1 &
P=$!; gate_pid $P; sleep 18; kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== run A (usb-kbd, high speed) ===="; cat "$OUT_A"
echo "==== run B (usb-mouse, high speed) ===="; cat "$OUT_B"
echo "==== run C (usb-kbd, full speed) ===="; cat "$OUT_C"
python3 "$DIR/check_markers.py" "$OUT_A" "$OUT_B" "$OUT_C"
