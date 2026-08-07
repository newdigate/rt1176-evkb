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
ELF="$DIR/$(gate_build_dir)/arm_math_test.elf"; OUT="$DIR/arm_math.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/arm_math.dbg" &
P=$!; gate_pid $P; sleep 5; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "STAGE_FFT=PASS"    "$OUT" || { echo "FAIL: fft"; exit 1; }
grep -q "STAGE_FIR=PASS"    "$OUT" || { echo "FAIL: fir"; exit 1; }
grep -q "STAGE_SIN=PASS"    "$OUT" || { echo "FAIL: sin"; exit 1; }
grep -q "ARM_MATH_ALL=PASS" "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: ARM_MATH_ALL"
