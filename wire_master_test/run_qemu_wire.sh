#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/wire_master_test.elf"; OUT="$DIR/wire.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/wire.dbg" &
P=$!; gate_pid $P; sleep 3; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "scan_found=0x50"          "$OUT" || { echo "FAIL: scan"; exit 1; }
grep -q "scan_count=1"             "$OUT" || { echo "FAIL: phantom devices on scan (NACK detection broken)"; exit 1; }
grep -q "wr_status=0"              "$OUT" || { echo "FAIL: write status"; exit 1; }
grep -q "readback=DE AD BE EF "    "$OUT" || { echo "FAIL: readback"; exit 1; }
grep -q "absent_status=2"          "$OUT" || { echo "FAIL: absent NACK"; exit 1; }
echo "PASS: Wire I2C master verified (scan, write, readback, NACK)"
