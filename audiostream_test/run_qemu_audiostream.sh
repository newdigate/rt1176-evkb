#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/audiostream_test.elf"; OUT="$DIR/audiostream.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/audiostream.dbg" &
P=$!; sleep 5; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "STAGE_FLOW=PASS"    "$OUT" || { echo "FAIL: flow";   exit 1; }
grep -q "STAGE_NOLEAK=PASS"  "$OUT" || { echo "FAIL: noleak"; exit 1; }
grep -q "AUDIOSTREAM_ALL=PASS" "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: AUDIOSTREAM_ALL"
