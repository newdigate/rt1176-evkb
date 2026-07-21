#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/guard_sweep_test.elf"; OUT="$DIR/guard_sweep.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/guard_sweep.dbg" &
P=$!; gate_pid $P; sleep 10; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
for t in DELAY KARPLUS DRUM WAVETABLE RECQ PLAYQ; do
    grep -q "STAGE_$t=PASS" "$OUT" || { echo "FAIL: $t"; exit 1; }
done
grep -q "GUARD_SWEEP_ALL=PASS" "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: GUARD_SWEEP_ALL"
