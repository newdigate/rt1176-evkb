#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/arm_math_test.elf"; OUT="$DIR/arm_math.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/arm_math.dbg" &
P=$!; gate_pid $P; sleep 5; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "STAGE_FFT=PASS"    "$OUT" || { echo "FAIL: fft"; exit 1; }
grep -q "STAGE_FIR=PASS"    "$OUT" || { echo "FAIL: fir"; exit 1; }
grep -q "STAGE_SIN=PASS"    "$OUT" || { echo "FAIL: sin"; exit 1; }
grep -q "ARM_MATH_ALL=PASS" "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: ARM_MATH_ALL"
