#!/bin/sh
set -e
QEMU=~/Development/qemu2/build/qemu-system-arm
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/usb_enum_test.elf"; OUT="$DIR/usb.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none \
    -serial file:"$OUT" \
    -chardev null,id=usbcdc \
    -d guest_errors -D "$DIR/usb.dbg" &
P=$!; sleep 6; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== VCOM ===="; cat "$OUT"
echo "==== CI-CDC (enumeration) ===="; grep "CI-CDC" "$DIR/usb.dbg" | head
grep -q "USB=CONFIGURED" "$OUT" || { echo "FAIL: USB enumeration"; exit 1; }
echo "PASS: USB CDC enumeration verified"
