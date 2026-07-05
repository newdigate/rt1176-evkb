#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/tone_test.elf"; OUT="$DIR/tone.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/tone.dbg" &
P=$!; sleep 20; kill $P 2>/dev/null; wait $P 2>/dev/null || true   # -icount: deterministic delay()/PIT coupling
echo "==== captured ===="; cat "$OUT"
grep -q "TONE=PASS" "$OUT" || { echo "FAIL: tone"; exit 1; }
echo "PASS: tone verified"
