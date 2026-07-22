#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/pxp_blit_test.elf"; OUT="$DIR/pxp.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/pxp.dbg" &
P=$!; gate_pid $P; sleep 5; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "PXP_BEGIN=PASS" "$OUT" || { echo "FAIL: begin"; exit 1; }
grep -q "PXP_BLIT=PASS"    "$OUT" || { echo "FAIL: blit"; exit 1; }
grep -q "PXP_SUBRECT=PASS" "$OUT" || { echo "FAIL: subrect"; exit 1; }
grep -q "PXP_ALL=PASS"   "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: PXP Phase 1 verified"
