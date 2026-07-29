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
ELF="$DIR/build/filter_fir_test.elf"; OUT="$DIR/filter_fir.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/filter_fir.dbg" &
P=$!; gate_pid $P; sleep 10; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "STAGE_PB=PASS" "$OUT" || { echo "FAIL: passband"; exit 1; }
grep -q "STAGE_SB=PASS" "$OUT" || { echo "FAIL: stopband"; exit 1; }
grep -q "FIR_ALL=PASS"  "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: FIR_ALL"
