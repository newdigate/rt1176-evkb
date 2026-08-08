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
ELF="$DIR/$(gate_build_dir)/pxp_yuv_test.elf"; OUT="$DIR/pxp_yuv.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/pxp_yuv.dbg" &
P=$!; gate_pid $P; sleep 5; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "PXP_BEGIN=PASS"   "$OUT" || { echo "FAIL: begin"; exit 1; }
grep -q "PXP_CSC_RUN=PASS" "$OUT" || { echo "FAIL: csc run"; exit 1; }
grep -q "CSC_SUM=.* PASS"  "$OUT" || { echo "FAIL: csc checksum vs oracle"; exit 1; }
grep -q "PXP_YUV=PASS"     "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: PXP UYVY->RGB565 CSC verified against software oracle"
