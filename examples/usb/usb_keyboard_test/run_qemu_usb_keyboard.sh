#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location. The old
# hardcoded ~/Development/rt1170/evkb/tools/... meant a worktree or a clone at
# any other path silently loaded a DIFFERENT tree's gate-lib.sh -- which surfaces
# as "gate_reap: command not found", or worse, as a gate quietly running against
# the wrong library.
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/usb_keyboard_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/usb.dbg"; RES="$DIR/kbd.result"
gate_tmp "$RES"
PORT=14556
rm -f "$VCOM" "$DBG" "$RES"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none \
    -serial file:"$VCOM" \
    -chardev socket,id=usbhid-tap,host=127.0.0.1,port=$PORT,server=on,wait=off \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# Capture the driver's exit code without letting `set -e` abort before the
# diagnostics below (the driver exits non-zero on a RED gate, by design).
set +e
python3 "$DIR/usb_kbd_driver.py" 127.0.0.1 $PORT > "$RES" 2>&1
RC=$?
set -e
sleep 1; gate_reap $P
gate_require_capture "$VCOM"
echo "==== VCOM ===="; cat "$VCOM"
echo "==== CI-HID ===="; grep -E "CI-CDC|CI-HID" "$DBG" 2>/dev/null | head
echo "==== kbd driver ===="; cat "$RES"
[ $RC -eq 0 ] || { echo "FAIL: USB keyboard report"; exit 1; }
echo "PASS: USB keyboard HID report verified"
