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
# GEOMETRY.  The UI is a LOGICAL 1280x720 frame presented rotated 90 degrees
# CLOCKWISE on the 720x1280 panel (lvgl_mipi_panel_create_rotated): logical
# (lx,ly) sits at physical (719-ly, lx).  The GT911 model takes percentages of
# the PHYSICAL panel and computes raw = res*pct/100 in integers, so each tap
# below is a logical target from acid_box.cpp's geometry block: its centre,
# mapped to physical, divided by 7.2 (x) and 12.8 (y), rounded to a whole
# percent, then truncated by the model -- move a widget and redo ONE line:
#   ▶            PLAY_X=1040, BAR_Y=20, 100x48 -> logical 1040..1139 x 20..67
#                centre (1090,44) -> physical (675,1090) -> 93.75%,85.2%
#                -> P 94 85 -> raw (676,1088) -> logical (1088,43)    ✔ inside
#   step cell 2  LANE_X0+2*108=232, LANE_Y0=96, 100x100 -> 232..331 x 96..195
#                centre (282,146) -> physical (573,282)  -> 79.6%,22.0%
#                -> P 80 22 -> raw (576,281)  -> logical (281,143)    ✔ inside
#   CUTOFF knob  KNOB_X0=16, KNOB_Y0=520, 150x150 -> 16..165 x 520..669
#                drag logical (91, 582..647) -> physical (137..72, 91)
#                -> 19.0%..10.0%, 7.1% -> P 19 7 ... P 10 7, ten samples
#                at 1 % (7.2 px) steps -> raw (136..72, 89)
#                -> logical (89, 583..647)                            ✔ inside
# The drag is a DOWNWARD logical drag (cutoff strictly decreasing, the same
# assertion as the portrait build); rotated, it is a leftward raw drag.  It
# stops at logical y 647, 22 px inside the knob's bottom edge, on purpose: the
# widget does not set LV_OBJ_FLAG_PRESS_LOCK, so a sample past its edge would
# hand LVGL a different object and end the drag as PRESS_LOST with no further
# CUTOFF lines.
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
# Room for boot + ~8 bars at 128 BPM + the 640-instant script; a healthy run is
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

# THE CAPTURE THE ASSERTIONS PARSE.  The reap can cut the FINAL line mid-token
# -- a last "ACIDBOX_ROT_EQ pass=7 fail=0 us=" with no digits, or a vsync line
# ending "timeouts=" -- and every per-line witness check below would then report
# a named failure the firmware never had.  A final line with no terminator is
# not evidence either way, so it is dropped here (the $(...) strips a trailing
# \n, so a complete capture compares empty and is copied whole).  The existence
# check and the printed capture above used the raw capture; from here on EVERY
# assertion reads $OUT, which names the parsed copy.  The bare `echo` ends the
# printed capture's cut line, or the verdict below would be glued onto it
# (measured: "...fail=0 us=FAIL: ...", which no ^FAIL: reader can see).
OUTP=$(gate_capture_path "$DIR" acid_box.parse)
gate_tmp "$OUTP"
if [ -n "$(tail -c1 "$OUT")" ]; then echo; sed '$d' "$OUT" > "$OUTP"; else cp "$OUT" "$OUTP"; fi
OUT="$OUTP"

# --- boot: the three subsystems, in the order the firmware brings them up ----
grep -q "CODEC_OK" "$OUT" || { echo "FAIL: WM8962 codec"; exit 1; }
grep -q "PANEL_OK" "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "I2C_OK"   "$OUT" || { echo "FAIL: touch bring-up"; exit 1; }
grep -q "ACIDBOX_DONE" "$OUT" || { echo "FAIL: setup() never completed"; exit 1; }

# --- the engine, and the pipeline's fence ------------------------------------
# One ELF, both engines (synthui_knob_test's pattern): QEMU has no GC355, so the
# chip-ID probe must read 0 and every knob stays software.  The tripwires keep a
# QEMU run from ever CLAIMING the gpu -- a fake claim would turn the golden into
# a statement about hardware this machine does not model.
# RED 2026-08-28, all three via qrun's REAL_QEMU replay of a doctored passing
# capture: ENGINE line flipped to gpu -> "engine line missing or not sw";
# ACIDBOX_GPU_ERR=0 appended -> "TRIPWIRE gpu error counter in QEMU"; one
# mid-run witness set to timeouts=3 -> "vsync fence timed out during the run".
grep -qE "^ACIDBOX_ENGINE=sw\r?$" "$OUT" || { echo "FAIL: engine line missing or not sw"; exit 1; }
grep -q "ACIDBOX_ENGINE=gpu" "$OUT" && { echo "FAIL: TRIPWIRE gpu engine claimed in QEMU"; exit 1; }
grep -q "ACIDBOX_GPU_ERR="   "$OUT" && { echo "FAIL: TRIPWIRE gpu error counter in QEMU"; exit 1; }
# vsync-fence health (the rotated present, which shares the db fence): a timeout
# silently degrades presentation to unfenced flips (tearing possible, the
# scanout-flash class), so it must fail
# BY NAME -- and on ANY witness line, because the per-bar lines are what cover
# the play/tap/drag portion of the run rather than only boot.
grep -qE "^ACIDBOX_VSYNC flips=[0-9]+ isrs=[0-9]+ timeouts=0\r?$" "$OUT" \
    || { echo "FAIL: vsync fence line missing"; exit 1; }
grep -E "^ACIDBOX_VSYNC " "$OUT" | grep -vqE "timeouts=0\r?$" \
    && { echo "FAIL: vsync fence timed out during the run"; exit 1; }

# --- the rotated present -----------------------------------------------------
# ACIDBOX_ROT is printed only by the rotated pipeline: an image that fell back
# to the portrait create_db path has NO such line and must fail BY NAME.
# errors must be 0 on EVERY witness (boot + per bar).  The LAST line must show
# ops>0, full>=2 (the two forced start-up presents -- the boot line shows only
# the first; the second lands on the next refresh), and ops>full: the threshold
# is 915456 px (display/pxp_rotate_probe on silicon, spec section 4.1), so
# after start-up every present is per-rect and the damage path MUST have run.
# ops/px/us are never pinned: ops varies by a few presents between runs
# (263-265 observed).
grep -qE "^ACIDBOX_ROT ops=[0-9]+ full=[0-9]+ px=[0-9]+ us=[0-9]+ errors=0\r?$" "$OUT" \
    || { echo "FAIL: rotation present line missing or errors!=0"; exit 1; }
grep -E "^ACIDBOX_ROT " "$OUT" | grep -vqE "errors=0\r?$" \
    && { echo "FAIL: rotation errors during the run"; exit 1; }
ROT_LAST=$(grep -E "^ACIDBOX_ROT " "$OUT" | tail -1 | tr -d '\r')
ROT_OPS=$( printf '%s\n' "$ROT_LAST" | sed 's/.* ops=\([0-9]*\).*/\1/')
ROT_FULL=$(printf '%s\n' "$ROT_LAST" | sed 's/.* full=\([0-9]*\).*/\1/')
[ "$ROT_OPS" -gt 0 ] || { echo "FAIL: no PXP present ops counted"; exit 1; }
[ "$ROT_FULL" -ge 2 ] || { echo "FAIL: fewer than 2 full-frame presents (start-up forces two)"; exit 1; }
[ "$ROT_OPS" -gt "$ROT_FULL" ] || { echo "FAIL: the damage path never ran (ops == full)"; exit 1; }
# NEW-50: full-frame presents are bounded ABOVE as well.  LVGL's inv-area
# buffer holds LV_INV_BUF_SIZE=32 rects and on overflow invalidates the WHOLE
# SCREEN (lv_refr.c) -- every golden stays green and every frame becomes a
# full present.  The playhead now damages eight strips per step (two cue
# changes x four bezel-ring boxes), so that overflow is the regression this
# change could introduce, and `full` is the only witness that names it: the
# two start-up presents are the only legitimate full-frame presents in a run
# (full=2 on every fixture line and on silicon at ops=12538).
# Demonstrated RED 2026-09-16: lv_obj_invalidate(lv_screen_active()) added to
# ui_poll (intermittently, so ops>full still held and this bound -- not the
# ops==full one above -- is what fired) -> full=82 ->
# "FAIL: full-frame presents above start-up's two (full=82)". An UNCONDITIONAL
# per-tick invalidate makes every present full-frame (ops==full identically)
# and is instead caught by the ops>full check above it.
[ "$ROT_FULL" -le 2 ] || { echo "FAIL: full-frame presents above start-up's two (full=$ROT_FULL)"; exit 1; }
# The EQUALITY GUARD: the presented buffer must equal the rotated canvas on
# every sampled row -- at boot (full-frame path, one synchronous check) and once
# per bar (damage path: an INCREMENTAL check armed at step 8, 8 sampled rows per
# loop pass, passes with a flip pending skipped, its verdict printed at the
# seam).  fail=0 on every line, and the last pass count must cover boot + every
# bar that has a guard line after it -- so each bar's check must FINISH before
# its seam; one that does not is counted a fail by the next arm.  us= is the
# largest SINGLE-PASS stall of the last check (the boot check's whole cost; a
# per-bar check's costliest chunk), printed for the silicon bench and never
# gated (QEMU time is a fiction).
grep -qE "^ACIDBOX_ROT_EQ pass=[1-9][0-9]* fail=0 us=[0-9]+\r?$" "$OUT" \
    || { echo "FAIL: rotation equality guard line missing"; exit 1; }
grep -E "^ACIDBOX_ROT_EQ " "$OUT" | grep -vqE " fail=0 us=[0-9]+\r?$" \
    && { echo "FAIL: rotation equality guard failed during the run"; exit 1; }
EQ_PASS=$(grep -E "^ACIDBOX_ROT_EQ " "$OUT" | tail -1 | tr -d '\r' | sed 's/.*pass=\([0-9]*\).*/\1/')
# Bars counted only up to the LAST equality line: the reap can land between a
# bar line and the guard line that follows it, and that bar is not evidence
# either way.
NBARS=$(awk '/^ACIDBOX_BAR=/ { b++ } /^ACIDBOX_ROT_EQ / { n = b } END { print n + 0 }' "$OUT")
[ "$EQ_PASS" -ge $((NBARS + 1)) ] \
    || { echo "FAIL: equality guard ran $EQ_PASS times for $NBARS bars + boot"; exit 1; }
# ...but the reap may cut between ONE bar line and its guard line, never more.
# Without this bound the check above is satisfied by a guard that STOPS: a
# capture whose only guard line is the boot one gives NBARS=0 and 1 >= 1, and a
# guard that stops after bar 2 passes the same way (both replayed in review).
NBARS_ALL=$(grep -c "^ACIDBOX_BAR=" "$OUT" || true)
[ "$NBARS" -ge $((NBARS_ALL - 1)) ] \
    || { echo "FAIL: equality guard stopped reporting after bar $NBARS of $NBARS_ALL"; exit 1; }

# --- the boot frame ----------------------------------------------------------
# GOLDEN — FNV-1a over the whole 720x1280 XRGB8888 framebuffer, taken before the
# indev exists, so it is a statement about the SCENE and nothing about touch.
# It has read the PRESENTED buffer since the 2026-08-28 db pipeline; what the
# landscape build changed is that the presented buffer is ROTATED -- the
# logical 1280x720 scene after the CW90 PXP present, still 3686400 bytes.
#
# ★ THE FRAME BEHIND THIS GOLDEN WAS LOOKED AT, not merely reproduced: dumped
# out of QEMU's monitor with pmemsave and eyeballed (capstone Task 5, and
# re-confirmed here — see transcript_qemu.txt for the exact recipe and what was
# checked: layout, all 8 knob boot angles, the lane matching the preset
# cell-for-cell).  This tree does not record a golden for a frame nobody has
# seen; the knob pilot's clamped arc and the VGLite GPU frame were both perfectly
# reproducible AND visibly wrong.  Silicon confirmed the landscape frame on
# 2026-09-15 (transcript_hw_evkb.txt, LANDSCAPE): upright by eye, all four lane
# corners touch-correct, gpu golden 0x2231070B on three boots -- a separate
# golden set from this sw one, never reconciled.
#
# ★ ANCHORED WITH \r?$, AND THE ANCHOR IS LOAD-BEARING.  Measured in Task 4:
# FNV-1a converges in its low bits over a repeating 4-byte pattern, so the blank
# frame, an X=0 frame and an X=0xFF frame ALL end in 9DC5.  Only the top half
# discriminates, and an unanchored grep would accept the blank screen this
# assertion exists to reject.
# Re-goldened 2026-08-27 (NEW-20 Phase 2): synthui_rotary_knob replaced the
# old knob (9 bounded knobs, explicit ±140 range; pitch knob detent-by-step).
# The new frame was dumped (pmemsave recipe in transcript_qemu.txt) and LOOKED
# AT: notch rotors, bounded track arcs, boot angles verified against the
# preset (CUTOFF +21.5deg, pitch A1 at -35deg). Bit-identical across two runs.
# Re-goldened 2026-09-15 (LANDSCAPE, spec 2026-09-14): layout C presented CW90
# through the PXP; the golden still checksums the presented buffer, which is
# now the rotated portrait frame. The scanned buffer was pmemsaved from a no-touch boot, its FNV
# recomputed to the same value, and viewed upright: "ACID BOX" top-left,
# -/128.0/+ centred, PLAY/STOP top-right; the 2x8 lane top-left matching the
# preset (accent dots 0,7,12; slide bars 3,10,15; rests 2,5,9,14 dark; cell 0
# selected); pitch knob at A1, ACC lit, SLD dark, SAW; the reserved empty band
# (logical y 379..519); the eight knobs CUTOFF..SLIDE T along the bottom at
# their boot angles; nothing mirrored. Bit-identical across two runs.
# Re-goldened 2026-09-16 (SynthUI editor rework, plan
# docs/superpowers/plans/2026-09-16-acid-box-synthui-editor.md): the step row
# grew prev/next press feedback, a shared helper, a bounded select_step index
# and its own STEP caption. The scanned buffer (0x80684080, unchanged) was
# pmemsaved from a no-touch boot -- see transcript_qemu.txt for the recipe --
# and viewed upright, with the frame confirmed correct by the controller
# before this golden was pinned. Checked: "ACID BOX" top-left; -/128.0/+
# centred; PLAY and STOP top-right, all unchanged; the 2x8 lane, ivory keys,
# red LED lit on the gated steps and dark on the four rests (keys 03, 06, 10,
# 15 -- preset indices 2, 5, 9, 14); amber accent lamps under keys 01, 08, 13
# (indices 0, 7, 12) and blue slide lamps under 04, 11, 16 (indices 3, 10,
# 15), matching kPreset; step numbers 01..16 under the lamps, 01 brightened as
# the selected one, key 01 visibly sunk against its neighbours; no key shows a
# red cue bezel (the transport is stopped at boot); the editor column: pitch
# knob at A1, ACC lit amber, SLD dark, SAW, and "01" on the seven-segment
# (with its ghost segments) between two panel buttons whose double chevrons
# now read clearly at 0.85 glyph scale; the eight sound knobs along the
# bottom at their boot angles, nothing mirrored or on its side. Cosmetic, and
# accepted as-is: at 44 px wide the panel buttons' bezel radius rounds to 1
# px, so prev/next are the only square-cornered objects on a panel of rounded
# ones. Measured extents (prev 1080..1123, seven-segment ink 1130..1212 of an
# 84 px box, next 1220..1263, row y 300..355, STEP caption 1152..1187 at y
# 369..378, lane ink to y 360, reserved band from y 379) match the values
# already recorded in acid_box.cpp's own comment. Bit-identical across two
# runs.
grep -qE "ACIDBOX_UI_SUM=0xBB2AEE59\r?$" "$OUT" || { echo "FAIL: UI golden"; exit 1; }
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
# the loop seam and reads 0.1843 in bar 1 in every run, idle or loaded.  Later
# bars read 0.41+ in almost every reading, idle or loaded, with an occasional
# dip (as low as 0.3205 under load -- see MARGINS below): the tick-0 shortfall
# is structural, the dips are not.  Asserting bar 1 would either fail honestly
# or invite someone to lower the margin until it passed, which is how a real
# threshold gets destroyed.
PRE_N=$(printf '%s\n' "$PRE" | sed 's/^ACIDBOX_BAR=//; s/ .*//')
[ "$PRE_N" -ge 2 ] || { echo "FAIL: pre-edit window is bar $PRE_N -- bar 1 is the transport's tick-0 outlier, pad 2 too short"; exit 1; }

PRE_RMS=$( printf '%s\n' "$PRE"  | sed 's/.*RMS=\[//; s/\].*//')
POST_RMS=$(printf '%s\n' "$POST" | sed 's/.*RMS=\[//; s/\].*//')
echo "pre-edit  window: $PRE"
echo "post-edit window: $POST"

# MARGINS: sounding > 0.02, rest < 0.005 -- a 4x separation between the two
# thresholds, the acid_bass_test convention for float DSP (windows with margin,
# never bit-goldens).  Measured room either side is far larger.  Re-measured
# 2026-09-15 after the equality guard went incremental (four idle runs, two
# under 6 spinners on 8 CPUs): in the gate's pre/post windows every gated step
# read 0.3438..0.4297 -- 17x the sounding floor -- and every rest 0.0001..0.0005
# (10x under the rest ceiling).  Each cell is the PEAK of RMS reads polled from
# loop(), so anything that stalls polling at a note's onset lowers that one
# cell.  ★ STEP 0 WAS THE CELL AFTER A ~31 ms (silicon) SEAM STALL until the
# guard stopped running at the seam, and an A/B under the same 6 spinners shows
# it: with the old seam guard step 0 fell below 0.36 in all three runs (0.3205
# at bar 4 every time, 0.3188 in the PRE-EDIT window of one); with the
# incremental guard it held 0.40+ in all three.  Not every dip was the guard's:
# across the six runs above, step 0 of bars 2+ read 0.4147..0.4298 in all but
# two readings, both at bar 4 -- 0.3765 idle and 0.3205 loaded, the old build's
# exact value, so something else still stalls polling at that seam at times
# (unidentified).  The window minimum, 0.3438, was step 8 -- where the check is
# now armed -- in one loaded pre-edit window; step 8 also read 0.3735 and 0.3751
# in old-build runs, so these runs do not settle whether the chunks lower it.
# (Numbers and runs: transcript_qemu.txt.)  Neither threshold may be moved to
# make a run pass.

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

echo "PASS: acid box -- boot golden, rotated present + equality guard, injected play/edit/drag, step 2 silent before the tap and sounding after"
