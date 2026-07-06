#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/audiooutput_i2s_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/audiooutput.dbg"
rm -f "$VCOM" "$DBG"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$VCOM" \
    -d guest_errors -D "$DBG" &
P=$!
sleep 5; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== VCOM ===="; cat "$VCOM" 2>/dev/null || true
grep -q "^info synth_peak=" "$VCOM" || { echo "FAIL: no info synth_peak= line"; exit 1; }
grep "^info synth_peak=" "$VCOM"
grep -q "STAGE_SYNTH=PASS" "$VCOM" || { echo "FAIL: STAGE_SYNTH"; exit 1; }
grep -q "AUDIOOUTPUT_ALL=PASS" "$VCOM" || { echo "FAIL: AUDIOOUTPUT_ALL"; exit 1; }
echo "PASS: AUDIOOUTPUT_ALL"
