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
ELF="$DIR/build/edma_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/edma.dbg"; TAP="$DIR/tap.raw"
rm -f "$VCOM" "$DBG" "$TAP"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$VCOM" \
    -chardev file,id=sai1-tap,path="$TAP" \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
sleep 4; gate_reap $P
gate_require_capture "$VCOM"
echo "==== VCOM ===="; cat "$VCOM"
grep -q "STAGE_A_PASS" "$VCOM" || { echo "FAIL: stage A mem2mem"; exit 1; }
echo "PASS: stage A"
grep -q "STAGE_B_DONE" "$VCOM" || { echo "FAIL: stage B not reached"; exit 1; }
python3 "$DIR/check_tap.py" "$TAP" || { echo "FAIL: stage B tap mismatch"; exit 1; }
grep -q "STAGE_B_PASS" "$VCOM" || { echo "FAIL: stage B block IRQ never fired"; exit 1; }
echo "PASS: EDMA_ALL (A+B)"
