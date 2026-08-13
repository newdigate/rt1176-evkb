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
# Wait on the run's TWO liveness signals instead of a fixed sleep. The fixed
# `sleep 5` flaked twice on 2026-08-13 under load (~5.7 and ~7-8.6): the tap
# was healthy but the console token had not landed yet, so the gate reaped a
# run that was merely slow (docs/KNOWN-BROKEN-GATES.md, 2026-08-13 entry).
#
# BOTH conditions matter, in this order (the spec's 4.3 names the trap):
#   1. the VCOM contains a STAGE_SYNTH= verdict (PASS or FAIL -- this is
#      liveness, not the assertion; a FAIL must stop the wait and fail below,
#      not spin to the cap), AND
#   2. the tap has accumulated TAP_MIN_BYTES. The token arrives BEFORE the tap
#      finishes accumulating, so polling the token alone would reap early and
#      starve check_tap.py. 128 KiB is ~1 s of tap at the observed QEMU drain
#      (~130 KB/s); the old sleep 5 collected ~650 KB, far more than the
#      amplitude-only peak check needs.
# The cap is a diagnosis aid, not an assertion: on expiry the wait notes it
# and falls through, and the named assertions below say what was missing. A
# QEMU that died early ends the wait immediately rather than eating the cap.
TAP_MIN_BYTES=131072
WAIT_CAP=60
waited=0
while :; do
    if grep -q "STAGE_SYNTH=" "$VCOM" 2>/dev/null; then
        sz=$(wc -c < "$TAP" 2>/dev/null | tr -d ' ')
        if [ "${sz:-0}" -ge "$TAP_MIN_BYTES" ]; then break; fi
    fi
    if ! kill -0 "$P" 2>/dev/null; then break; fi
    if [ "$waited" -ge "$WAIT_CAP" ]; then
        echo "note: liveness wait capped at ${WAIT_CAP}s (token or tap never arrived)"
        break
    fi
    sleep 1; waited=$((waited + 1))
done
gate_reap $P
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
