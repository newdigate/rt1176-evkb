#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/camera_preview_synth.elf"; OUT="$DIR/camera_synth.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/camera_synth.dbg" &
P=$!; gate_pid $P; sleep 12; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "PXP_BEGIN=PASS"  "$OUT" || { echo "FAIL: pxp begin"; exit 1; }
grep -q "PIPE_RUN=PASS"   "$OUT" || { echo "FAIL: pipeline run"; exit 1; }
grep -q "PIPE_SUM=.* PASS" "$OUT" || { echo "FAIL: pipeline output vs oracle"; exit 1; }
grep -q "CAM_SYNTH=PASS"  "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: synthetic UYVY -> CSC -> decimate pipeline verified vs oracle"
