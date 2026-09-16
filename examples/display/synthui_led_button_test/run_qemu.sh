#!/bin/sh
# synthui_led_button_test -- the SynthUI LedButton (NEW-25) rendered on the
# RK055 through the db pipeline, checksummed.  Spec:
# docs/superpowers/specs/2026-09-15-synthui-led-button-design.md section 8.
#
# Demonstrated RED 2026-09-15:
#   - initial golden altered to 0xDEADBEEF (run_qemu.sh)
#       -> "FAIL: led_button checksum"
#   - final-state golden altered to 0xDEADBEEF (run_qemu.sh)
#       -> "FAIL: led_button final-state checksum (0xB34EDF19)"
#   - synthui_led_button_set_lit: led_invalidate_lit_box(obj) -> lv_obj_invalidate(obj)
#     (SynthUI src/synthui_led_button.cpp)
#       -> "FAIL: lit damage above its box (lit=10000 > 1450)"
#     (delta equality stays GREEN for this one -- led_button_delta_eq=PASS,
#      led_button_damage_op lit=10000 press=6640 cue=10000 color=1450 -- a
#      bigger box is still a correct box, so only the per-op bound can see it.
#      ★ That reading was true when cue's own bound was 10000 and the overall
#      max had to be 10000 to suit it.  Since NEW-50 cue damages only the
#      bezel ring and the overall max is press's 6640, so lit=10000 would trip
#      that too -- the per-op bound is now what NAMES the guilty setter rather
#      than the only thing that notices.  The demo above was run against the
#      pre-NEW-50 bounds and is left as recorded.)
#   - synthui_led_button_lit_box: rect_px(&L.halo, L.dy_px) -> rect_px(&L.led, L.dy_px)
#     (SynthUI src/synthui_led_button_math.h)
#       -> "FAIL: delta render differs from full render (0xBFBA0971 vs 0xB34EDF19)"
#     (both demonstrated against the ORIGINAL 4-step tail/goldens, before the
#      final-review pass below added steps (e)/(f) and moved the final golden)
#
# Demonstrated RED 2026-09-16 (final review, NEW-25): the scripted-indev tail
# added below closes a real coverage hole -- nothing before this reached
# led_on_press_edge()'s RELEASED/PRESS_LOST branch (SynthUI
# src/synthui_led_button.cpp).  Deleting that branch (leaving PRESSED) on a
# scratch SynthUI checkout, rebuilt and booted bounded:
#     led_button_delta_crc=0xB34EDF19 led_button_fresh_crc=0x463C3371
#     led_button_delta_eq=FAIL -> "FAIL: SynthUI led_button delta equality"
#   (0xB34EDF19 is exactly the PRE-fix final golden: with RELEASED/PRESS_LOST
#    deleted, key 12 never un-sinks, so the mutant's stuck pixels are
#    bit-identical to the old tail that never touched key 12 at all.)
#
# Demonstrated RED 2026-09-16 (NEW-50, cue band + clip-aware led_draw):
#   - synthui_led_button_set_cue left at lv_obj_invalidate(obj) (the pre-NEW-50
#     widget, no mutant needed) against the re-derived bound
#       -> "FAIL: cue damage above its box (cue=10000 > 900)"
#   - cue_boxes corner term dropped (band = bw + 1; SynthUI
#     src/synthui_led_button_math.h) -- the strips miss the ring on the corner
#     diagonals, and the stale corners of every key whose cue toggled an odd
#     number of times leave the delta render different from a fresh one
#       -> "FAIL: delta render differs from full render (0xA70BCB79 vs 0x463C3371)"
#   - led_in_clip made `return true` (SynthUI src/synthui_led_button.cpp):
#     four strips x 12 unconditional layers
#       -> "FAIL: cue draw tasks above its bound (cue=48 > 23)"
#     with led_button_crc=0xD474F06D, led_button_fresh_crc=0x463C3371 and
#     led_button_delta_eq=PASS all still GREEN -- the whole argument for the
#     task counter existing.  Baseline MEASURED before NEW-50 (the counter
#     landed one commit ahead of the widget change, against the whole-key
#     set_cue, and 9a34fd2 records it): tasks_op 12/12/12/12.
#     (lit and color rose to 12 too in this run -- every op loses its clip,
#     not just cue -- but the cue check runs first and exits the script, so
#     only the cue FAIL prints; full line was led_button_tasks_op lit=12
#     press=12 cue=48 color=12.)
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/synthui_led_button_test.elf"
OUT=$(gate_capture_path "$DIR" synthui_led_button.uart)
DBG=$(gate_capture_path "$DIR" synthui_led_button.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# 20s: bring-up margin plus headroom for the 64 delta steps, the tail and 64 fps frames.
sleep 20; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
# Panel chain first: a framebuffer no display owns would still checksum.
grep -q "PANEL_OK"          "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "led_button_scene=16 grid=4x4" "$OUT" || { echo "FAIL: scene line missing"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
grep -qE "LVGL_BYTES=3686400\r?$" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }
# INITIAL GOLDEN -- FNV-1a over the whole 720x1280 PRESENTED buffer: the
# 16-key bank (off; lit red/amber/green/blue; latched; lit+latched; cue;
# cue+lit; disabled; disabled+lit; 32 px; 34 px; 150 px; 120x80; LV pressed).
# Recorded across two consecutive bit-identical QEMU runs (2026-09-15, NEW-25,
# vendored LVGL 9.4.0, XRGB8888).  On a mismatch work out WHICH of {SynthUI
# pin, LVGL pin, lv_conf.h, fonts, scene} changed; do NOT paste in whatever
# the board printed.
grep -qE "led_button_crc=0xD474F06D\r?$" "$OUT" || { echo "FAIL: led_button checksum"; exit 1; }
# DELTA EQUALITY: the 64-step LCG sequence (lit / pressed latch / cue / colour
# on keys 0..12) plus the 6-step scripted tail (key 14 pressed then unlit at
# its centring offset; key 15 recoloured then unlit at the LV_STATE_PRESSED
# offset; key 12 pressed then released through a SCRIPTED lv_indev_t --
# reaching led_on_press_edge()'s real PRESSED/RELEASED path, which nothing
# else in this suite touches), rendered through the widget's delta damage,
# must be PIXEL-IDENTICAL to a fresh full render of the final state.
DSEQ=$(grep -a -oE "led_button_delta_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
DFUL=$(grep -a -oE "led_button_fresh_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DSEQ" ] && [ -n "$DFUL" ] || { echo "FAIL: delta guard tokens missing"; exit 1; }
[ "$DSEQ" = "$DFUL" ] || { echo "FAIL: delta render differs from full render ($DSEQ vs $DFUL)"; exit 1; }
grep -qE "led_button_delta_eq=PASS\r?$" "$OUT" || { echo "FAIL: delta equality"; exit 1; }
# FINAL-STATE GOLDEN: equality proves delta == fresh, not that either is right.
# The final state draws combinations the initial golden never does
# (disabled+pressed, cue+pressed, pressed 32/34 px keys, amber LED off), so a
# palette wiring bug for those would pass both of the checks above.  This
# golden moved from 0xB34EDF19 (final review, NEW-25): steps (e)/(f) leave
# key 12 genuinely unlatched at the end -- the ONLY way to make its final
# press+release a real test of led_on_press_edge() rather than a no-op behind
# its own-latch early return -- so key 12 is legitimately UP in this state
# where it used to be left sunk. Verified via the two QEMU runs and the
# mutant demo above, not merely accepted because the run printed it.
[ "$DFUL" = "0x463C3371" ] || { echo "FAIL: led_button final-state checksum ($DFUL)"; exit 1; }
# ENGAGEMENT, PER OP. Each op has its own bound, equal to its box on the
# largest key the sequence touches (keys 0..12: 100 px and 32/34 px): lit and
# colour = the halo, 58x25 = 1450; pressed = the cap group at both offsets,
# 80x83 = 6640; cue = ONE of the four bezel-ring strips (NEW-50), the top or
# bottom one, 100 x band 9 = 900 (band = ceil(R - (R-bw)/sqrt2) + 1 with
# R=17, bw=4).  These are EXACT boxes re-derived from
# synthui_led_button_math.h: a deliberate layout change trips them loudly and
# they must then be re-derived from the math, never loosened to whatever the
# run printed.  The per-op checks come BEFORE the overall max so a regression
# is named by its op.
OPLINE=$(grep -a -oE "led_button_damage_op lit=[0-9]+ press=[0-9]+ cue=[0-9]+ color=[0-9]+" "$OUT" | head -1)
[ -n "$OPLINE" ] || { echo "FAIL: per-op damage line missing"; exit 1; }
op() { echo "$OPLINE" | grep -oE "$1=[0-9]+" | cut -d= -f2; }
LIT=$(op lit); PRS=$(op press); CUE=$(op cue); COL=$(op color)
[ "$LIT" -gt 0 ] && [ "$PRS" -gt 0 ] && [ "$CUE" -gt 0 ] && [ "$COL" -gt 0 ] \
    || { echo "FAIL: an op was never exercised ($OPLINE)"; exit 1; }
[ "$LIT" -le 1450 ] || { echo "FAIL: lit damage above its box (lit=$LIT > 1450)"; exit 1; }
[ "$COL" -le 1450 ] || { echo "FAIL: colour damage above its box (color=$COL > 1450)"; exit 1; }
[ "$PRS" -le 6640 ] || { echo "FAIL: press damage above its box (press=$PRS > 6640)"; exit 1; }
[ "$CUE" -le 900 ]  || { echo "FAIL: cue damage above its box (cue=$CUE > 900)"; exit 1; }
# Overall max: the largest legitimate box is now press (6640).  Redundant
# with the per-op bounds above, kept because it is the FULL-SCREEN tripwire
# (921600) and the vacuity suite pins its presence.
DAREA=$(grep -a -oE "led_button_damage max=[0-9]+" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DAREA" ] && [ "$DAREA" -gt 0 ] || { echo "FAIL: delta damage guard missing or zero"; exit 1; }
[ "$DAREA" -le 6640 ] || { echo "FAIL: delta damage not engaged (max=$DAREA)"; exit 1; }
grep -qE "led_button_damage max=[0-9]+ total=[0-9]+ steps=64 tail=6\r?$" "$OUT" || { echo "FAIL: delta sequence length changed (expected steps=64 tail=6)"; exit 1; }
# DRAW TASKS, PER OP (NEW-50).  The mechanism the area bounds cannot see:
# LVGL renders one pass per invalidated area, and lv_draw_rect allocates a
# task before any clip test (lv_draw_sw_fill.c / lv_draw_sw_border.c reject it
# afterwards, against t->clip_area) out of a heap in uncached SDRAM.  A cue
# change is four strips, so a led_draw that stopped clipping its layers costs
# 4 x 12 = 48 tasks (12 = led_draw's task count for a LIT key: the bezel's
# single lv_draw_rect yields both a FILL and a BORDER task, one of twelve
# layers; 11 for an unlit key, which skips the halo) -- with every golden and
# the delta-equality guard still GREEN.  cue's bound is the one with teeth;
# lit, press and colour are single-pass ops whose counts can only rise if a
# setter starts emitting more than one box.  Bounds are pinned from the
# measured run WITH their derivation (Task 4 of the plan), and re-derived,
# never loosened.
TLINE=$(grep -a -oE "led_button_tasks_op lit=[0-9]+ press=[0-9]+ cue=[0-9]+ color=[0-9]+" "$OUT" | head -1)
[ -n "$TLINE" ] || { echo "FAIL: per-op draw-task line missing"; exit 1; }
tk() { echo "$TLINE" | grep -oE "$1=[0-9]+" | cut -d= -f2; }
TLIT=$(tk lit); TPRS=$(tk press); TCUE=$(tk cue); TCOL=$(tk color)
[ "$TLIT" -gt 0 ] && [ "$TPRS" -gt 0 ] && [ "$TCUE" -gt 0 ] && [ "$TCOL" -gt 0 ] \
    || { echo "FAIL: an op created no draw task ($TLINE)"; exit 1; }
# Pinned 2026-09-16 from the measured run, derivation in the comment above
# each: a layout change trips these loudly; re-derive, never loosen.
#   cue=23: key 12 (the 34 px key) at LCG step 62, cued while LATCHED
#     pressed (dy_px=1 there).  Unpressed the four strips are top=5
#     bottom=3 left=7 right=7 (band=4, cap.x1=3 -- the cap group IS
#     reached, unlike the 100 px keys where band=9 stops short of
#     cap.x1=10); pressed, cap/highlight/halo/led/dots/base shift down 1 px
#     off the CUE box (which is always computed unpressed -- the bezel
#     never moves) so top loses its cap_top hit (5->3) and base's rect
#     (unpressed y=26..27, just short of the bottom strip's y=28..31) slides
#     into it (bottom 3->6): 3+6+7+7=23.  The 32 px key (key 11) has the
#     identical unpressed/pressed totals (22/23) by the same mechanism but
#     the LCG never latches it before cueing it, so 23 is what prints.
#   lit=10, color=10: the halo box on a 100 px OR the 34 px key hits
#     bezel_fill+bezel_border+well+cap+cap_top+highlight+halo+led+dot1+dot2
#     (cap_low and base fall outside it) = 10; the 32 px key gives only 8
#     (no dots below 34 px).  Translation-invariant with dy, so pressed and
#     unpressed agree.
#   press=12: the press box (the union of cap/highlight/halo at dy=0 and
#     cap/base at the pressed dy, per synthui_led_button_press_box()) spans
#     every layer on a 100 px OR the 34 px key -- all twelve hit, unlike
#     lit/color's ten, because its y-range reaches down to base and up past
#     highlight; the 32 px key gives 10 (no dots).
[ "$TCUE" -le 23 ] || { echo "FAIL: cue draw tasks above its bound (cue=$TCUE > 23)"; exit 1; }   # key 12 (34 px) latched, LCG step 62: 3+6+7+7
[ "$TLIT" -le 10 ] || { echo "FAIL: lit draw tasks above its bound (lit=$TLIT > 10)"; exit 1; }
[ "$TCOL" -le 10 ] || { echo "FAIL: colour draw tasks above its bound (color=$TCOL > 10)"; exit 1; }
[ "$TPRS" -le 12 ] || { echo "FAIL: press draw tasks above its bound (press=$TPRS > 12)"; exit 1; }
# vsync-fence health (db pipeline): a timeout means the tear-free property
# silently degraded with every golden still green.
# A vsync-fence red during a full sweep is the documented load-sensitivity class
# (docs/KNOWN-BROKEN-GATES.md): re-run this gate idle before treating it as a
# regression.
grep -qE "led_button_vsync flips=[0-9]+ isrs=[0-9]+ timeouts=0\r?$" "$OUT" || { echo "FAIL: vsync fence unhealthy or missing"; exit 1; }
grep -q "crc_done" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
grep -q "PASS: SynthUI led_button render verified" "$OUT" || { echo "FAIL: render verification token missing"; exit 1; }
echo "PASS: SynthUI led_button render verified"
