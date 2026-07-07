#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/audiooutput_i2s_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/audiooutput.dbg"; TAP="$DIR/tap.raw"
rm -f "$VCOM" "$DBG" "$TAP"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$VCOM" \
    -chardev file,id=sai1-tap,path="$TAP" \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
sleep 5; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== VCOM ===="; cat "$VCOM" 2>/dev/null || true
grep -q "^info synth_peak=" "$VCOM" || { echo "FAIL: no info synth_peak= line"; exit 1; }
grep "^info synth_peak=" "$VCOM"
SYNTH_OK=0
grep -q "STAGE_SYNTH=PASS" "$VCOM" && SYNTH_OK=1 || echo "FAIL: STAGE_SYNTH"

echo "==== TAP ===="
TONE_OK=0
python3 "$DIR/check_tap.py" "$TAP" && TONE_OK=1 || echo "FAIL: STAGE_TONE"

if [ "$SYNTH_OK" = "1" ] && [ "$TONE_OK" = "1" ]; then
    echo "AUDIOOUTPUT_ALL=PASS"
else
    echo "AUDIOOUTPUT_ALL=FAIL"
    exit 1
fi
