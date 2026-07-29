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
ELF="$DIR/build/guard_sweep_test.elf"; OUT="$DIR/guard_sweep.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/guard_sweep.dbg" &
P=$!; gate_pid $P; sleep 10; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
for t in DELAY KARPLUS DRUM WAVETABLE RECQ PLAYQ; do
    grep -q "STAGE_$t=PASS" "$OUT" || { echo "FAIL: $t"; exit 1; }
done
grep -q "GUARD_SWEEP_ALL=PASS" "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: GUARD_SWEEP_ALL"
