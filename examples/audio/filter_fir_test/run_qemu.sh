#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/filter_fir_test.elf"; OUT="$DIR/filter_fir.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/filter_fir.dbg" &
P=$!; gate_pid $P; sleep 10; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "STAGE_PB=PASS" "$OUT" || { echo "FAIL: passband"; exit 1; }
grep -q "STAGE_SB=PASS" "$OUT" || { echo "FAIL: stopband"; exit 1; }
grep -q "FIR_ALL=PASS"  "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: FIR_ALL"
