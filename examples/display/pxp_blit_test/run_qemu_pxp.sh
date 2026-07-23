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
grep -q "PXP_FILL=PASS"    "$OUT" || { echo "FAIL: fill"; exit 1; }
grep -q "PXP_BLIT=PASS"    "$OUT" || { echo "FAIL: blit"; exit 1; }
grep -q "PXP_SUBRECT=PASS" "$OUT" || { echo "FAIL: subrect"; exit 1; }
grep -q "PXP_ROT90=PASS"  "$OUT" || { echo "FAIL: rot90"; exit 1; }
grep -q "PXP_ROT180=PASS" "$OUT" || { echo "FAIL: rot180"; exit 1; }
grep -q "PXP_ROT270=PASS" "$OUT" || { echo "FAIL: rot270"; exit 1; }
grep -q "PXP_HFLIP=PASS"  "$OUT" || { echo "FAIL: hflip"; exit 1; }
grep -q "PXP_VFLIP=PASS"  "$OUT" || { echo "FAIL: vflip"; exit 1; }
grep -q "PXP_ASYNC=PASS"  "$OUT" || { echo "FAIL: async"; exit 1; }
grep -q "PXP_SDRAM=PASS" "$OUT" || { echo "FAIL: sdram"; exit 1; }
grep -q "PXP_OCRAM=PASS" "$OUT" || { echo "FAIL: ocram"; exit 1; }
grep -q "PXP_TCM="       "$OUT" || { echo "FAIL: tcm probe did not report"; exit 1; }
grep -q "PXP_ALIGN="     "$OUT" || { echo "FAIL: align probe did not report"; exit 1; }
grep -q "PXP_ALL=PASS"   "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: PXP Phase 1 verified"
