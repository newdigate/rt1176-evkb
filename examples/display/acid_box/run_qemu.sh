#!/bin/sh
# acid_box — the audio+display capstone gate: touch -> pattern -> SOUND.
#
# WHAT THIS GATE IS FOR.  Every other display gate stops at the framebuffer and
# every other audio gate stops at the samples.  This one closes the loop: a
# scripted finger presses ▶ and a step cell, and the assertion is that the
# per-step RMS table produced by the AUDIO graph changes at exactly the step the
# finger touched.  A dead touch path, a UI that repaints but writes no engine
# state, or a sequencer that ignores the write all land here as a red gate.
#
# ★ LOCAL-ONLY QEMU DEPENDENCY.  The injected gestures come from qemu2's
# `imxrt.gt911` `touch-script` property, which is not upstream.  A fresh clone
# sees this gate red for that reason — the same class as `sai1-rxinject` and the
# rt1062 half of `usb_descriptor_survey`.  That is the GPL firewall working, not
# a regression; see docs/KNOWN-BROKEN-GATES.md.
#
# ★ THE -global LONG FORM IS MANDATORY.  The device type is `imxrt.gt911`, with
# a DOT, and qemu_global_option() splits the driver name at the FIRST dot, so
#     -global imxrt.gt911.touch-script=FILE
# parses as driver "imxrt", property "gt911.touch-script", matches nothing and
# prints NOTHING — a nonexistent path passed that way boots happily and the run
# silently asserts against the model's BUILT-IN script instead of ours.  Measured
# in Task 6 of the capstone plan.  Never shorten the line below.
#
# REGENERATING touch_script.txt.  It is generated, never hand-counted.  The
# generator lives in transcript_qemu.txt beside the run it produced; the pads are
# sized from two measured constants — the model publishes an instant every
# IMXRT_GT911_STEP_MS = 20 ms (include/hw/i2c/imxrt_gt911.h:195; the capstone
# plan's "40 ms" is wrong), and a 128 BPM bar of sixteenths is 1.875 s of audio
# time, which this tree's QEMU audio clock delivers at ~0.81x wall ≈ 2.31 s
# ≈ 116 instants.  Both pads are 300 instants ≈ 2.6 bars.
#
# GEOMETRY.  The three tap points are percentages of the 720x1280 panel and are
# derived from acid_box.cpp's absolute placement — move a widget and the tap
# moves with it:
#   ▶            lv_obj_set_pos(play,540,16) + set_size(70,48) -> 540..610 x 16..64
#                (80%, 3%)  = pixel (576, 38)                          ✔ inside
#   step cell 2  pos(8 + 2*88, 640) + size(82,82)              -> 184..266 x 640..722
#                (31%, 53%) = pixel (223, 678)                         ✔ inside
#   CUTOFF knob  pos(15, 90) + size(150,150)                   -> 15..165 x 90..240
#                (12%, 13%) = pixel (86, 166)                          ✔ inside
# The knob drag stops at 18% = y 230, still inside 90..240 on purpose: the widget
# does not set LV_OBJ_FLAG_PRESS_LOCK, so a sample past its edge would hand LVGL
# a different object and end the drag as PRESS_LOST with no further CUTOFF lines.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location (a
# hardcoded path silently loads a different tree's gate-lib.sh from a worktree).
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/acid_box.elf"
OUT=$(gate_capture_path "$DIR" acid_box.uart)
DBG=$(gate_capture_path "$DIR" acid_box.dbg)
SCRIPT="$DIR/touch_script.txt"

# PRE-FLIGHT, and it is not redundant belt-and-braces.  QEMU itself refuses a
# missing script (imxrt_gt911_load_script -> error_report + exit(1)), but that
# message goes to QEMU's STDERR, which qrun redirects into the -D log — so the
# operator's first symptom would be "no UART capture", which is also what a
# firmware that never booted looks like.  Naming the file here makes the two
# distinguishable at a glance.  The QEMU-side refusal is still what guarantees
# this gate cannot pass vacuously if this check is ever removed; both were
# measured (see transcript_qemu.txt, vacuity proof (c)).
[ -f "$SCRIPT" ] || {
    echo "FAIL: touch script missing at $SCRIPT -- every injected gesture is gone"
    exit 1
}

rm -f "$OUT" "$DBG"
# Room for boot + ~8 bars at 128 BPM + the 636-instant script; a healthy run is
# reaped by the poll below at ~20 s, so this only bounds a hang.
QRUN_TIMEOUT=${QRUN_TIMEOUT:-150}
export QRUN_TIMEOUT
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -global driver=imxrt.gt911,property=touch-script,value="$SCRIPT" \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P

# Poll for the LAST scripted gesture (the knob drag) rather than sleeping a fixed
# window: the run is ~20 s idle and several times that under a loaded sweep.
# Ceiling 120 s (240 x 0.5).  `kill -0` breaks out early when QEMU has died, so a
# fatal model error costs 0.5 s instead of the full ceiling.
i=0
while [ $i -lt 240 ]; do
    kill -0 $P 2>/dev/null || break
    n=$(grep -c 'CUTOFF=' "$OUT" 2>/dev/null || true)
    if [ "${n:-0}" -ge 3 ]; then
        sleep 2          # let the tail of the drag land before reaping
        break
    fi
    sleep 0.5
    i=$((i+1))
done
gate_reap $P

# QEMU's stderr lands in the -D log, so a fatal model error (bad touch script,
# unparsable instant) is otherwise invisible behind "no UART capture".
if [ ! -s "$OUT" ] && [ -s "$DBG" ]; then
    echo "==== QEMU stderr (-D log) ===="; cat "$DBG"
fi
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

# --- boot: the three subsystems, in the order the firmware brings them up ----
grep -q "CODEC_OK" "$OUT" || { echo "FAIL: WM8962 codec"; exit 1; }
grep -q "PANEL_OK" "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "I2C_OK"   "$OUT" || { echo "FAIL: touch bring-up"; exit 1; }
grep -q "ACIDBOX_DONE" "$OUT" || { echo "FAIL: setup() never completed"; exit 1; }

# --- the boot frame ----------------------------------------------------------
# GOLDEN — FNV-1a over the whole 720x1280 XRGB8888 framebuffer, taken before the
# indev exists, so it is a statement about the SCENE and nothing about touch.
#
# ★ THE FRAME BEHIND THIS GOLDEN WAS LOOKED AT, not merely reproduced: dumped
# out of QEMU's monitor with pmemsave and eyeballed (capstone Task 5, and
# re-confirmed here — see transcript_qemu.txt for the exact recipe and what was
# checked: layout, all 8 knob boot angles, the lane matching the preset
# cell-for-cell).  This tree does not record a golden for a frame nobody has
# seen; the knob pilot's clamped arc and the VGLite GPU frame were both perfectly
# reproducible AND visibly wrong.  Silicon confirmation is owed (plan Task 8).
#
# ★ ANCHORED WITH \r?$, AND THE ANCHOR IS LOAD-BEARING.  Measured in Task 4:
# FNV-1a converges in its low bits over a repeating 4-byte pattern, so the blank
# frame, an X=0 frame and an X=0xFF frame ALL end in 9DC5.  Only the top half
# discriminates, and an unanchored grep would accept the blank screen this
# assertion exists to reject.
grep -qE "ACIDBOX_UI_SUM=0xD3BC88D7\r?$" "$OUT" || { echo "FAIL: UI golden"; exit 1; }
# The all-zero framebuffer, rejected BY NAME: 0x9BC99DC5 is the FNV of 3686400
# zero bytes.  A blank frame is a real failure mode in this tree
# (vglite_lvgl_test) and is otherwise indistinguishable from any other mismatch.
grep -q "ACIDBOX_UI_SUM=0x9BC99DC5" "$OUT" \
    && { echo "FAIL: framebuffer is all zeros (the anti-golden, by name)"; exit 1; }

# --- the injected gestures ---------------------------------------------------
grep -qE "^PLAYING=1$" "$OUT" || { echo "FAIL: PLAY tap never landed"; exit 1; }
# The preset has step 2 as a REST (note 0, gate 0), and cbStepTap parks a rest on
# A1 when it turns it on, so this exact string is the tap's signature: any other
# note or gate value means the finger hit a different cell.
grep -qE "^STEP\[2\]=note33 gate1" "$OUT" \
    || { echo "FAIL: step-2 tap never wrote the pattern"; exit 1; }

PLAY_LN=$(grep -n "^PLAYING=1$"          "$OUT" | head -1 | cut -d: -f1)
STEP_LN=$(grep -n "^STEP\[2\]=note33 gate1" "$OUT" | head -1 | cut -d: -f1)
CUT_LN=$(grep -n  "CUTOFF="              "$OUT" | head -1 | cut -d: -f1)
[ -n "$CUT_LN" ] || { echo "FAIL: the cutoff drag produced no CUTOFF line"; exit 1; }
[ "$STEP_LN" -gt "$PLAY_LN" ] || { echo "FAIL: gesture order — edit before play"; exit 1; }
[ "$CUT_LN"  -gt "$STEP_LN" ] || { echo "FAIL: gesture order — drag before edit"; exit 1; }

# BOOT IS SILENT, and the firmware header calls that a contract: while the
# transport is stopped, currentStep() sits at -1 and audio_probe_poll() returns
# before it can print.  A box that hums on power-up shows up as a bar line ahead
# of the play tap.
awk -v p="$PLAY_LN" 'NR<p && /^ACIDBOX_BAR=/ { print "FAIL: audio before ▶ -- boot was not silent"; exit 1 }' \
    "$OUT" || exit 1

# --- the windows the RMS assertions run in ----------------------------------
# PRE  = the last bar line printed BEFORE the edit.  A bar line is emitted at the
#        15->0 seam, after its whole window has been filled, so a bar printed
#        before the STEP token is entirely pre-edit.  Rigorous by construction —
#        no timing assumption.
# POST = the last bar line between the edit and the drag.  Requiring >= 2 of them
#        is what makes the chosen one a window that OPENED after the tap rather
#        than the bar the tap landed inside (whose step 2 may already have played).
#        Bounding it before the first CUTOFF keeps this assertion about the TAP:
#        the drag takes the filter down to ~100 Hz, and a post-edit window that
#        straddled it would be measuring two edits at once.
PRE=$( awk -v s="$STEP_LN"               'NR<s && /^ACIDBOX_BAR=/ { l=$0 } END { print l }' "$OUT")
POST=$(awk -v s="$STEP_LN" -v c="$CUT_LN" 'NR>s && NR<c && /^ACIDBOX_BAR=/ { l=$0 } END { print l }' "$OUT")
NPOST=$(awk -v s="$STEP_LN" -v c="$CUT_LN" 'NR>s && NR<c && /^ACIDBOX_BAR=/ { n++ } END { print n+0 }' "$OUT")
[ -n "$PRE"  ] || { echo "FAIL: no complete bar before the edit -- pad 2 too short"; exit 1; }
[ -n "$POST" ] || { echo "FAIL: no complete bar between the edit and the drag -- pad 3 too short"; exit 1; }
[ "$NPOST" -ge 2 ] || { echo "FAIL: only $NPOST bar(s) between edit and drag -- the post-edit window is not provably post-edit"; exit 1; }

# ★ BAR 1 IS NOT A VALID WINDOW.  The transport records boundaries strictly
# inside (from, to], so it never emits tick 0 at phase 0: step 0 first fires at
# the loop seam and reads ~0.18 in bar 1 against 0.42+ in every bar after it.
# Asserting bar 1 would either fail honestly or invite someone to lower the
# margin until it passed, which is how a real threshold gets destroyed.
PRE_N=$(printf '%s\n' "$PRE" | sed 's/^ACIDBOX_BAR=//; s/ .*//')
[ "$PRE_N" -ge 2 ] || { echo "FAIL: pre-edit window is bar $PRE_N -- bar 1 is the transport's tick-0 outlier, pad 2 too short"; exit 1; }

PRE_RMS=$( printf '%s\n' "$PRE"  | sed 's/.*RMS=\[//; s/\].*//')
POST_RMS=$(printf '%s\n' "$POST" | sed 's/.*RMS=\[//; s/\].*//')
echo "pre-edit  window: $PRE"
echo "post-edit window: $POST"

# MARGINS: sounding > 0.02, rest < 0.005 -- a 4x separation between the two
# thresholds, the acid_bass_test convention for float DSP (windows with margin,
# never bit-goldens).  Measured room either side is far larger: gated steps read
# 0.36..0.44 (18x the sounding floor) and rests read 0.0001..0.0006 (8x under the
# rest ceiling).  Neither number may be moved to make a run pass.

# The untouched preset, in the pre-edit window: every gated step sounds, every
# rest is silent.  Twelve gated indices and four rests -- a stuck voice, a
# sequencer ignoring gates, or a pattern that never loaded all fail here.
echo "$PRE_RMS" | awk -F, '{
  split("0 1 3 4 6 7 8 10 11 12 13 15", g, " ");
  split("2 5 9 14", r, " ");
  for (i in g) if ($(g[i]+1) + 0 <= 0.02)  { printf "FAIL: preset step %s silent (rms %s)\n",   g[i], $(g[i]+1); exit 1 }
  for (i in r) if ($(r[i]+1) + 0 >= 0.005) { printf "FAIL: preset rest %s sounding (rms %s)\n", r[i], $(r[i]+1); exit 1 }
}' || exit 1

# ★ THE INTEGRATION ASSERTION -- the reason the capstone exists.  The SAME step
# index, in a window before the finger touched it and a window after: silent,
# then sounding.  Nothing short of touch -> LVGL hit test -> cbStepTap ->
# seq.step() -> the note pump -> the voice -> the analyzer can move it.
echo "$PRE_RMS"  | awk -F, '$3 + 0 >= 0.005 { printf "FAIL: pre-edit step 2 not silent (rms %s)\n",    $3; exit 1 }' || exit 1
echo "$POST_RMS" | awk -F, '$3 + 0 <= 0.02  { printf "FAIL: post-edit step 2 not sounding (rms %s)\n", $3; exit 1 }' || exit 1

# The knob drag: >= 3 samples, strictly decreasing.  Strictly, not merely
# non-increasing -- a knob that latched on its first PRESSING and then repeated
# the same value would satisfy "not increasing" while proving nothing.
grep 'CUTOFF=' "$OUT" | sed 's/.*CUTOFF=//' | awk '
  { v[n++] = $1 + 0 }
  END {
    if (n < 3) { printf "FAIL: %d CUTOFF sample(s) from the drag, need >= 3\n", n; exit 1 }
    for (i = 1; i < n; i++)
      if (v[i] >= v[i-1]) { printf "FAIL: cutoff not strictly decreasing at sample %d (%.1f >= %.1f)\n", i, v[i], v[i-1]; exit 1 }
  }' || exit 1

echo "PASS: acid box -- boot golden, injected play/edit/drag, step 2 silent before the tap and sounding after"
