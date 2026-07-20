#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/usb_mouse_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/usb.dbg"; RES="$DIR/mouse.result"
gate_tmp "$RES"
PORT=14557
rm -f "$VCOM" "$DBG" "$RES"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none \
    -serial file:"$VCOM" \
    -chardev socket,id=usbhid-tap,host=127.0.0.1,port=$PORT,server=on,wait=off \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# Capture the driver's exit without letting `set -e` abort before the diagnostics.
set +e
python3 "$DIR/usb_mouse_driver.py" 127.0.0.1 $PORT > "$RES" 2>&1
RC=$?
set -e
sleep 1; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== VCOM ===="; cat "$VCOM" 2>/dev/null || true
echo "==== CI-HID ===="; grep -E "CI-CDC|CI-HID" "$DBG" 2>/dev/null | head
echo "==== mouse driver ===="; cat "$RES"
[ $RC -eq 0 ] || { echo "FAIL: USB mouse report"; exit 1; }
echo "PASS: USB mouse HID report verified"
