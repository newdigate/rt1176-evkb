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
ELF="$DIR/build/audiooutput_i2s_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/audiooutput.dbg"; TAP="$DIR/tap.raw"
rm -f "$VCOM" "$DBG" "$TAP"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$VCOM" \
    -chardev file,id=sai1-tap,path="$TAP" \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
sleep 5; gate_reap $P
gate_require_capture "$VCOM"
echo "==== VCOM ===="; cat "$VCOM"
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
