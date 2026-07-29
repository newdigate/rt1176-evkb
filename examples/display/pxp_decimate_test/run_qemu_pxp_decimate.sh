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
ELF="$DIR/build/pxp_decimate_test.elf"; OUT="$DIR/pxp_decimate.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/pxp_decimate.dbg" &
P=$!; gate_pid $P; sleep 5; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "PXP_BEGIN=PASS"     "$OUT" || { echo "FAIL: begin"; exit 1; }
grep -q "DEC_2=PASS"         "$OUT" || { echo "FAIL: decimate /2"; exit 1; }
grep -q "DEC_4=PASS"         "$OUT" || { echo "FAIL: decimate /4"; exit 1; }
grep -q "DEC_ROT_GUARD=PASS" "$OUT" || { echo "FAIL: rotate+decimate guard"; exit 1; }
grep -q "PXP_DECIMATE=PASS"  "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: PXP decimation verified against software oracle"
