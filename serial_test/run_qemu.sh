#!/bin/sh
set -e
QEMU=~/Development/qemu2/build/qemu-system-arm
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/serial_test.elf"
OUT="$DIR/serial.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/serial.dbg" &
P=$!; sleep 3; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 Serial1 up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "count=3" "$OUT" || { echo "FAIL: counter missing"; exit 1; }
echo "PASS: QEMU serial output verified"
