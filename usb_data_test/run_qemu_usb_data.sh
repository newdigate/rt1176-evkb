#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/usb_data_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/usb.dbg"; RES="$DIR/echo.result"
PORT=14555
rm -f "$VCOM" "$DBG" "$RES"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none \
    -serial file:"$VCOM" \
    -chardev socket,id=usbcdc,host=127.0.0.1,port=$PORT,server=on,wait=off \
    -d guest_errors -D "$DBG" &
P=$!
python3 "$DIR/usb_echo_driver.py" 127.0.0.1 $PORT "PHASE2-ECHO" > "$RES" 2>&1
RC=$?
sleep 1; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== VCOM ===="; cat "$VCOM" 2>/dev/null || true
echo "==== CI-CDC ===="; grep "CI-CDC" "$DBG" 2>/dev/null | head
echo "==== echo driver ===="; cat "$RES"
[ $RC -eq 0 ] || { echo "FAIL: USB CDC echo"; exit 1; }
echo "PASS: USB CDC bulk data echo verified"
