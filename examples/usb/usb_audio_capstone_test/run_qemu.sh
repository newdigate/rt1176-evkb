#!/bin/sh
# QEMU gate for usb_audio_capstone_test -- the capstone graph, rt1062 only.
#
# WHAT THIS PROVES: that the whole graph is built, clocked and RUNNING --
# AudioMemory, the WM8960 control path, both USB audio nodes, the peak
# analyser and AudioOutputI2S's SAI1 TX DMA -- and that the USB host stack
# declines a device it cannot play to instead of hanging on it.
#
# ★ WHAT IT CANNOT PROVE: anything about USB audio transport. The emulated
# device is NEVER CLAIMED here, and that is the CORRECT negotiation outcome,
# not a failure. QEMU's usb-audio model advertises exactly one sample rate --
# 48000 (hw/usb/dev-audio.c, USBAUDIO_SAMPLE_RATE) -- while this graph runs at
# 44100, and AudioOutputUSBHost's constructor pins the OUT side to the graph
# rate by construction (Audio/output_usbhost.cpp), deliberately, because a rate
# mismatch there is a permanent 8.8% pitch error rather than a caveat. So
# uac1_find_alt() matches no alternate setting, claim() returns false at the
# OUT alt search, and the console reads "out=none in=none" forever with no
# "+ Audio" line. Measured, twice, independently.
#
# Do NOT "fix" that by moving a rate. The graph rate is a compile-time
# constant, so a 48000 USB rate would ship a deliberate 8.8% pitch error into
# the silicon path; usb/usb_audio_uac1_test's gate asserts rates=48000 from
# QEMU's descriptor as an external oracle, so moving the model would break it;
# and the USB claim path is ALREADY gated on rt1062 by that example, so
# re-proving it here would add no coverage.
#
# ★ THE UNCLAIMED DEVICE HANDS US THE CLOCK-OWNERSHIP PROOF FOR FREE, and it
# is stronger than any symbol inspection. Because nothing is ever claimed,
# AudioOutputUSBHost::frame_consumed() never fires -- yet the graph still
# updated ~430 times per guest second for the whole run. Only the SAI TX DMA
# can be pacing it. That is the sketch's declaration-order clocking design
# (AudioOutputI2S declared first wins update_setup()) verified by a run rather
# than by reading the constructor order.
#
# ★ out=none IS ASSERTED AS A TRIPWIRE, NOT AS A LIMITATION. If QEMU's model
# ever gains 44100, this gate goes RED -- deliberately -- and whoever sees that
# red must re-read what the gate can prove and widen it, because assertions
# about a never-claimed device would then be describing a different world.
# A red here is "the world changed", not "the firmware regressed".
#
# ★ COUNTER RATES ARE NOT ASSERTED, ONLY DIRECTION. in_under and out_drop climb
# at ~430/s against guest millis(), not the ~344.5/s (44100/128) that
# arithmetic predicts: QEMU's SAI-paced graph runs fast against guest time
# (which itself runs at roughly half wall-clock here). Assert that a counter
# CLIMBS; never assert how fast.
#
# ★ out_drop CLIMBING AT THE FULL GRAPH RATE IS CORRECT HERE and flatly
# contradicts the sketch's "one drop every seven minutes" comment. That comment
# is a SILICON prediction, for a claimed device whose USB frames drain the
# FIFO. In QEMU nothing drains it, so every block is dropped. The gate must
# never assert a small out_drop.
#
# Silicon is the sole proof audio actually moves -- see transcript_hw_evkb.txt.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/usb_audio_capstone_test.elf"
OUT=$(gate_capture_path "$DIR" capstone.uart)
DBG=$(gate_capture_path "$DIR" capstone.dbg)
TAP=$(gate_capture_path "$DIR" tap.raw)
rm -f "$OUT" "$DBG" "$TAP"
# The usb-audio device stays ATTACHED even though it is never claimed. That is
# the point: an unclaimable device must be declined cleanly and the graph must
# keep running past it. Detach it and the gate stops proving that.
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -chardev file,id=sai1-tap,path="$TAP" \
    -audiodev none,id=snd0 \
    -device usb-audio,bus=usbhost.0,port=1,audiodev=snd0 \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# Poll the run's TWO liveness signals rather than sleeping a guessed duration.
# Both matter, and the gate waits for BOTH:
#   1. the console reaching HEARTBEAT seq=3 -- the last token asserted below;
#   2. the tap reaching TAP_MIN_BYTES, which check_tap.py --expect-silence
#      independently requires (a short tap is a DEAD graph, not a quiet one).
#
# ★ THE ORDER IS THE OPPOSITE OF audio/audiooutput_i2s_test'S, so do not
# "simplify" this to whichever single condition looks sufficient. Measured
# here: the tap passes 128 KiB at ~1.5 s wall while seq=3 lands at ~6 s wall,
# because guest millis() runs at roughly half wall-clock on this model -- so
# here it is the TAP that would reap early, whereas in the audiooutput gate it
# is the token. Waiting on both is order-agnostic and survives either.
#
# The cap is a diagnosis aid, not an assertion: on expiry the wait notes it and
# falls through, and the named assertions below say what was actually missing.
# A QEMU that died early ends the wait immediately rather than eating the cap.
TAP_MIN_BYTES=131072
WAIT_CAP=60
waited=0
while :; do
    if grep -q "HEARTBEAT seq=3 " "$OUT" 2>/dev/null; then
        sz=$(wc -c < "$TAP" 2>/dev/null | tr -d ' ')
        if [ "${sz:-0}" -ge "$TAP_MIN_BYTES" ]; then break; fi
    fi
    if ! kill -0 "$P" 2>/dev/null; then break; fi
    if [ "$waited" -ge "$WAIT_CAP" ]; then
        echo "note: liveness wait capped at ${WAIT_CAP}s (seq=3 or tap never arrived)"
        break
    fi
    sleep 1; waited=$((waited + 1))
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; head -20 "$OUT"

grep -q "CAPSTONE: start" "$OUT" || { echo "FAIL: banner"; exit 1; }
# myusb.begin() returned. Worth its own assertion on this board specifically:
# the documented rt1062 host failure mode is not "USB does not enumerate" but a
# hard hang inside setup() (PLL_USB2 spun on a CCM_ANALOG SET alias), which
# presents as the banner printed and NOTHING after it.
grep -q "CAPSTONE: host started, waiting for device" "$OUT" \
    || { echo "FAIL: USBHost::begin() never returned"; exit 1; }
grep -q "CAPSTONE: HEARTBEAT seq=3 " "$OUT" \
    || { echo "FAIL: loop did not survive to a third heartbeat"; exit 1; }

# The input node is being CLOCKED and finding nothing -- the correct emulated
# state. Direction only; see the counter-rate note in the header.
in_under_at() {   # in_under_at SEQ -> that heartbeat's underrun counter
    sed -n "s/^CAPSTONE: HEARTBEAT seq=$1 .*in_under=\([0-9][0-9]*\) .*/\1/p" "$OUT" | head -1
}
U1=$(in_under_at 1); U3=$(in_under_at 3)
[ -n "$U1" ] && [ -n "$U3" ] \
    || { echo "FAIL: no in_under counter in heartbeats 1 and 3"; exit 1; }
[ "$U3" -gt "$U1" ] \
    || { echo "FAIL: in_under did not climb ($U1 -> $U3) -- input node not clocked"; exit 1; }
echo "info in_under climbed $U1 -> $U3 across heartbeats 1..3"

# ★ TRIPWIRE, not a limitation -- see the header. seq=3 specifically, so a
# device claimed LATE cannot hide behind an early out=none.
grep -q "HEARTBEAT seq=3 .*out=none " "$OUT" \
    || { echo "FAIL: tripwire -- seq=3 does not report out=none; QEMU's model may have gained 44100, in which case this gate's assumptions need re-reading (see header)"; exit 1; }
grep -q "out=ready" "$OUT" \
    && { echo "FAIL: tripwire -- a heartbeat reports out=ready; the emulated device was claimed, which this gate assumes is impossible (see header)"; exit 1; }
# Vacuity guard on the same fact from the other side: the driver-arrival line.
grep -q "CAPSTONE: + Audio" "$OUT" \
    && { echo "FAIL: tripwire -- the emulated usb-audio device was claimed; every assertion above is describing a different world (see header)"; exit 1; }

echo "==== TAP ===="
# ★ THE CORE ASSERTION: the graph RUNS. Silence AT RATE -- peak exactly 0 AND
# at least TAP_MIN_BYTES of it. Neither half is sufficient alone: an empty tap
# is also silent, and an empty tap means the graph never ran. The checker is
# audio/audiooutput_i2s_test's, borrowed deliberately -- it is the same tap,
# the same SAI, the same board, asserted in its other mode.
python3 "$EVKB/examples/audio/audiooutput_i2s_test/check_tap.py" \
        --expect-silence --min-bytes "$TAP_MIN_BYTES" "$TAP" \
    || { echo "FAIL: STAGE_SILENCE -- SAI1 TX tap is not silence-at-rate"; exit 1; }

echo "PASS: CAPSTONE_GRAPH_RUNS_SILENT"
