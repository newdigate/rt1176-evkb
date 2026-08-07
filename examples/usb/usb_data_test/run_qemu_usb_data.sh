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
ELF="$DIR/$(gate_build_dir)/usb_data_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/usb.dbg"; RES="$DIR/echo.result"
gate_tmp "$RES"
PORT=14555
rm -f "$VCOM" "$DBG" "$RES"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none \
    -serial file:"$VCOM" \
    -chardev socket,id=usbcdc,host=127.0.0.1,port=$PORT,server=on,wait=off \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
python3 "$DIR/usb_echo_driver.py" 127.0.0.1 $PORT "PHASE2-ECHO" > "$RES" 2>&1
RC=$?
sleep 1; gate_reap $P
gate_require_capture "$VCOM"
echo "==== VCOM ===="; cat "$VCOM"
echo "==== CI-CDC ===="; grep "CI-CDC" "$DBG" 2>/dev/null | head
echo "==== echo driver ===="; cat "$RES"
[ $RC -eq 0 ] || { echo "FAIL: USB CDC echo"; exit 1; }
echo "PASS: USB CDC bulk data echo verified"
