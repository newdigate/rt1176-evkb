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
ELF="$DIR/build/camera_preview_synth.elf"; OUT="$DIR/camera_synth.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/camera_synth.dbg" &
P=$!; gate_pid $P; sleep 12; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "PXP_BEGIN=PASS"  "$OUT" || { echo "FAIL: pxp begin"; exit 1; }
grep -q "PIPE_RUN=PASS"   "$OUT" || { echo "FAIL: pipeline run"; exit 1; }
grep -q "PIPE_SUM=.* PASS" "$OUT" || { echo "FAIL: pipeline output vs oracle"; exit 1; }
grep -q "CAM_SYNTH=PASS"  "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: synthetic UYVY -> CSC -> decimate pipeline verified vs oracle"
