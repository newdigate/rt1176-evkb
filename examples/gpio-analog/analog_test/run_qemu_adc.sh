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
ELF="$DIR/build/analog_test.elf"; OUT="$DIR/adc.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/adc.dbg" &
P=$!; gate_pid $P; sleep 3; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "adc1_ch5=341"       "$OUT" || { echo "FAIL: adc1_ch5"; exit 1; }
grep -q "adc2_ch3=204"       "$OUT" || { echo "FAIL: adc2_ch3"; exit 1; }
grep -q "adc1_ch15_12b=4095" "$OUT" || { echo "FAIL: 12-bit"; exit 1; }
grep -q "A0=136"             "$OUT" || { echo "FAIL: A0 pin path (AD_10 = ADC1 CH2A)"; exit 1; }
grep -q "A4=68"              "$OUT" || { echo "FAIL: A4 pin path (AD_09 = ADC1 CH1B)"; exit 1; }
grep -q "async_fired=1" "$OUT" || { echo "FAIL: async ISR did not fire"; exit 1; }
grep -q "async_val=341"  "$OUT" || { echo "FAIL: async value"; exit 1; }
echo "PASS: LPADC blocking reads verified (both instances, resolution, pin path)"
