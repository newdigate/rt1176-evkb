#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/analog_test.elf"; OUT="$DIR/adc.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/adc.dbg" &
P=$!; gate_pid $P; sleep 3; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "adc1_ch5=341"       "$OUT" || { echo "FAIL: adc1_ch5"; exit 1; }
grep -q "adc2_ch3=204"       "$OUT" || { echo "FAIL: adc2_ch3"; exit 1; }
grep -q "adc1_ch15_12b=4095" "$OUT" || { echo "FAIL: 12-bit"; exit 1; }
grep -q "A0=0"               "$OUT" || { echo "FAIL: A0 pin path"; exit 1; }
grep -q "async_fired=1" "$OUT" || { echo "FAIL: async ISR did not fire"; exit 1; }
grep -q "async_val=341"  "$OUT" || { echo "FAIL: async value"; exit 1; }
echo "PASS: LPADC blocking reads verified (both instances, resolution, pin path)"
