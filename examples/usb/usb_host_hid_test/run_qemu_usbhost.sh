#!/bin/sh
# RT1176 USB Host gate (USB_OTG2 / usbhost.0): enumeration + HID input.
#
# The firmware brings up the ChipIdea EHCI in host mode and enumerates a HID
# device, printing markers over Serial1 (LPUART1 = serial_hd(0) = first -serial):
#   USB_HOST_BEGIN                 after myusb.begin()
#   KBD_CONNECT=<vid>:<pid>        when a keyboard enumerates
#   MOUSE_CONNECT=<vid>:<pid>      when a mouse enumerates
#   KEY=<n>                        when an injected keypress is delivered
#   MOUSE=<dx>,<dy>,<btn>          when an injected mouse report is delivered
#
# Enumeration (the *_CONNECT lines) only uses control transfers on the async
# schedule. To also prove the *periodic* schedule / interrupt-IN path, each run
# opens a QMP socket and, once the device has enumerated, inject_qmp.py feeds a
# synthetic keypress / mouse move via input-send-event; usb-kbd/usb-mouse turn
# it into a HID report the firmware collects on its next interrupt poll and
# prints as KEY=/MOUSE=. See inject_qmp.py for why the event routes to the
# console-less USB HID handler with -display none and no `device` targeting.
#
# OTG2's EHCI root hub is single-port (portnr=1), so kbd + mouse can't attach at
# once -- we do separate runs. Each device is pinned to the single root port
# with port=1: without it QEMU's convenience auto-hub (nfree==1) inserts a
# full-speed usb-hub ahead of the device, which is not how the real single
# physical root port behaves. The companion-less i.MX EHCI drives HS/FS/LS on
# the root port directly, so we also cover a FULL-speed device (run C,
# usb_version=1) -- the path a real 12 Mbit/s keyboard/mouse takes, including
# its interrupt-IN. -icount shift=auto keeps the EHCI frame timer
# (QEMU_CLOCK_VIRTUAL) deterministic against the firmware's polling.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/usb_host_hid_test.elf"
OUT_A="$DIR/usb_kbd.uart"; OUT_B="$DIR/usb_mouse.uart"; OUT_C="$DIR/usb_kbd_fs.uart"
rm -f "$OUT_A" "$OUT_B" "$OUT_C"

# QMP sockets live in /tmp (the AF_UNIX sun_path limit is 104 bytes, which $DIR
# can blow) and carry the runner PID so concurrent gate runs on the shared tree
# don't collide. gate_tmp reaps them on teardown; QEMU won't unlink them itself.
SOCK_A="/tmp/usbhost_qmp_kbd_$$.sock"
SOCK_B="/tmp/usbhost_qmp_mouse_$$.sock"
SOCK_C="/tmp/usbhost_qmp_kbdfs_$$.sock"
gate_tmp "$SOCK_A" "$SOCK_B" "$SOCK_C"
rm -f "$SOCK_A" "$SOCK_B" "$SOCK_C"

# Each run: launch QEMU headless with a QMP socket, let inject_qmp.py wait for
# enumeration then inject + confirm KEY=/MOUSE= (bounded internally), then kill.
# The injector's failure is non-fatal here -- check_markers.py is the authority.

# Run A: high-speed keyboard on the OTG2 root port.
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto -display none \
    -serial file:"$OUT_A" -d guest_errors -D "$DIR/usb_kbd.dbg" \
    -qmp unix:"$SOCK_A",server=on,wait=off \
    -device usb-kbd,bus=usbhost.0,port=1 &
P=$!; gate_pid $P
python3 "$DIR/inject_qmp.py" --sock "$SOCK_A" --marker "$OUT_A" \
    --kind kbd --connect KBD_CONNECT --result "KEY=" || echo "WARN: kbd/HS injector rc=$?"
sleep 1; kill $P 2>/dev/null; wait $P 2>/dev/null || true

# Run B: high-speed mouse on the OTG2 root port.
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto -display none \
    -serial file:"$OUT_B" -d guest_errors -D "$DIR/usb_mouse.dbg" \
    -qmp unix:"$SOCK_B",server=on,wait=off \
    -device usb-mouse,bus=usbhost.0,port=1 &
P=$!; gate_pid $P
python3 "$DIR/inject_qmp.py" --sock "$SOCK_B" --marker "$OUT_B" \
    --kind mouse --connect MOUSE_CONNECT --result "MOUSE=" || echo "WARN: mouse/HS injector rc=$?"
sleep 1; kill $P 2>/dev/null; wait $P 2>/dev/null || true

# Run C: full-speed keyboard (usb_version=1) -- exercises the companion-less
# FS/LS root-port path (enumeration + interrupt-IN) a real keyboard/mouse uses.
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto -display none \
    -serial file:"$OUT_C" -d guest_errors -D "$DIR/usb_kbd_fs.dbg" \
    -qmp unix:"$SOCK_C",server=on,wait=off \
    -device usb-kbd,usb_version=1,bus=usbhost.0,port=1 &
P=$!; gate_pid $P
python3 "$DIR/inject_qmp.py" --sock "$SOCK_C" --marker "$OUT_C" \
    --kind kbd --connect KBD_CONNECT --result "KEY=" || echo "WARN: kbd/FS injector rc=$?"
sleep 1; kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== run A (usb-kbd, high speed) ===="; cat "$OUT_A"
echo "==== run B (usb-mouse, high speed) ===="; cat "$OUT_B"
echo "==== run C (usb-kbd, full speed) ===="; cat "$OUT_C"
python3 "$DIR/check_markers.py" "$OUT_A" "$OUT_B" "$OUT_C"
