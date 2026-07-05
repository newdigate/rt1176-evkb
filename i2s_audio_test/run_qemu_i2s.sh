#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/i2s_audio_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/i2s.dbg"; TAP="$DIR/tap.raw"
rm -f "$VCOM" "$DBG" "$TAP"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$VCOM" \
    -chardev file,id=sai1-tap,path="$TAP" \
    -d guest_errors -D "$DBG" &
P=$!
sleep 4; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== VCOM ===="; cat "$VCOM" 2>/dev/null || true
grep -q "STAGE_A_PASS" "$VCOM" || { echo "FAIL: stage A"; exit 1; }
echo "PASS: stage A (SAI1 config)"
