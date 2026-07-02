#!/bin/sh
set -e
QEMU=~/Development/qemu2/build/qemu-system-arm
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/wire_master_test.elf"; OUT="$DIR/wire.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/wire.dbg" &
P=$!; sleep 3; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "scan_found=0x50"          "$OUT" || { echo "FAIL: scan"; exit 1; }
grep -q "wr_status=0"              "$OUT" || { echo "FAIL: write status"; exit 1; }
grep -q "readback=DE AD BE EF "    "$OUT" || { echo "FAIL: readback"; exit 1; }
grep -q "absent_status=2"          "$OUT" || { echo "FAIL: absent NACK"; exit 1; }
echo "PASS: Wire I2C master verified (scan, write, readback, NACK)"
