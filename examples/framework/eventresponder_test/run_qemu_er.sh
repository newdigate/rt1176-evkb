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
ELF="$DIR/build/eventresponder_test.elf"; OUT="$DIR/er.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/er.dbg" &
P=$!; gate_pid $P; sleep 5; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "STAGE_IMMEDIATE=PASS" "$OUT" || { echo "FAIL: immediate"; exit 1; }
grep -q "STAGE_YIELD=PASS"     "$OUT" || { echo "FAIL: yield";     exit 1; }
grep -q "STAGE_CLEAR=PASS"     "$OUT" || { echo "FAIL: clear";     exit 1; }
grep -q "STAGE_STATUS=PASS"    "$OUT" || { echo "FAIL: status";    exit 1; }
grep -q "EVENTRESPONDER_ALL=PASS" "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: EVENTRESPONDER_ALL"
