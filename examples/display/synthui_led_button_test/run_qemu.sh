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
#      bigger box is still a correct box, so only the per-op bound can see it)
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
# ENGAGEMENT, PER OP. The single max catches a setter reverting to FULL-SCREEN
# invalidation (921600 px); it cannot see a lit/colour/press setter reverting
# to WHOLE-KEY invalidation, because a cue change legitimately repaints a whole
# 100 px key (10000).  So each op has its own bound, equal to its box on the
# largest key the sequence touches: lit and colour = the halo, 58x25 = 1450;
# pressed = the cap group at both offsets, 80x83 = 6640; cue = the key, 10000.
# These are EXACT measured boxes, re-derived from synthui_led_button_math.h:
# a deliberate layout change trips them loudly and they must then be
# re-derived from the math, never loosened to whatever the run printed.
DAREA=$(grep -a -oE "led_button_damage max=[0-9]+" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DAREA" ] && [ "$DAREA" -gt 0 ] || { echo "FAIL: delta damage guard missing or zero"; exit 1; }
[ "$DAREA" -le 10000 ] || { echo "FAIL: delta damage not engaged (max=$DAREA)"; exit 1; }
grep -qE "led_button_damage max=[0-9]+ total=[0-9]+ steps=64 tail=6\r?$" "$OUT" || { echo "FAIL: delta sequence length changed (expected steps=64 tail=6)"; exit 1; }
OPLINE=$(grep -a -oE "led_button_damage_op lit=[0-9]+ press=[0-9]+ cue=[0-9]+ color=[0-9]+" "$OUT" | head -1)
[ -n "$OPLINE" ] || { echo "FAIL: per-op damage line missing"; exit 1; }
op() { echo "$OPLINE" | grep -oE "$1=[0-9]+" | cut -d= -f2; }
LIT=$(op lit); PRS=$(op press); CUE=$(op cue); COL=$(op color)
[ "$LIT" -gt 0 ] && [ "$PRS" -gt 0 ] && [ "$CUE" -gt 0 ] && [ "$COL" -gt 0 ] \
    || { echo "FAIL: an op was never exercised ($OPLINE)"; exit 1; }
[ "$LIT" -le 1450 ]  || { echo "FAIL: lit damage above its box (lit=$LIT > 1450)"; exit 1; }
[ "$COL" -le 1450 ]  || { echo "FAIL: colour damage above its box (color=$COL > 1450)"; exit 1; }
[ "$PRS" -le 6640 ]  || { echo "FAIL: press damage above its box (press=$PRS > 6640)"; exit 1; }
[ "$CUE" -le 10000 ] || { echo "FAIL: cue damage above its box (cue=$CUE > 10000)"; exit 1; }
# vsync-fence health (db pipeline): a timeout means the tear-free property
# silently degraded with every golden still green.
# A vsync-fence red during a full sweep is the documented load-sensitivity class
# (docs/KNOWN-BROKEN-GATES.md): re-run this gate idle before treating it as a
# regression.
grep -qE "led_button_vsync flips=[0-9]+ isrs=[0-9]+ timeouts=0\r?$" "$OUT" || { echo "FAIL: vsync fence unhealthy or missing"; exit 1; }
grep -q "crc_done" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
grep -q "PASS: SynthUI led_button render verified" "$OUT" || { echo "FAIL: render verification token missing"; exit 1; }
echo "PASS: SynthUI led_button render verified"
