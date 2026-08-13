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
ELF="$DIR/$(gate_build_dir)/audiooutput_i2s_test.elf"
VCOM=$(gate_capture_path "$DIR" vcom.uart)
DBG=$(gate_capture_path "$DIR" audiooutput.dbg)
TAP=$(gate_capture_path "$DIR" tap.raw)
rm -f "$VCOM" "$DBG" "$TAP"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$VCOM") \
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
# ★ STAGE_TONE is AMPLITUDE-ONLY. check_tap.py computes peak > 4000 over the raw
# int16 tap; there is no frequency analysis. So it proves non-silent samples
# reached SAI1 TDR -- NOT that they form a 1 kHz tone, and NOT the sample rate.
# Note also that mimxrt1060-evk leaves the SAI at its 48 kHz default while the
# Audio library is 44100; that does not affect this assertion, because the tap
# mirrors raw TDR writes independently of the audio backend. Silicon is what
# proves audibility -- see transcript_hw_evkb.txt.
python3 "$DIR/check_tap.py" "$TAP" && TONE_OK=1 || echo "FAIL: STAGE_TONE"

if [ "$SYNTH_OK" = "1" ] && [ "$TONE_OK" = "1" ]; then
    echo "AUDIOOUTPUT_ALL=PASS"
else
    echo "AUDIOOUTPUT_ALL=FAIL"
    exit 1
fi
