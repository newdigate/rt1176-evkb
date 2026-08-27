#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location (a
# hardcoded path silently loads a different tree's gate-lib.sh from a worktree).
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/synthui_knob_test.elf"
# Run artifacts go through gate_capture_path, NOT "$DIR/<name>" -- see its
# comment in gate-lib.sh. This gate is rt1176-only today, so it cannot lose the
# documented `-j` race with a second board half; the older display siblings
# spell the path out by hand only because they predate the helper.
OUT=$(gate_capture_path "$DIR" synthui_knob.uart)
DBG=$(gate_capture_path "$DIR" synthui_knob.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# 16s: the RK055 bring-up margin lvgl_rk055_panel_test uses (12s) plus four
# extra full-screen software renders for the per-mode phases. Measured
# 2026-08-16: all tokens land ~2s in on an idle machine, so this is ~8x margin
# -- deliberate headroom for a loaded `-j` sweep, not a tight bound.
sleep 16; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
# Panel chain first: in DIRECT mode a framebuffer no display owns would still
# checksum perfectly.
grep -q "PANEL_OK"          "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
# Engine honesty: QEMU has no GC355, so the run must SAY software. A GPU
# claim with no GPU present -- or the gpu error counter appearing at all --
# must fail by name (rotary_knob_bench's tripwire discipline). Demonstrated
# RED 2026-08-27: a fake "rk_engine=gpu" line appended to a passing capture
# failed here as "TRIPWIRE gpu engine claimed in QEMU".
grep -qE "rk_engine=sw\r?$" "$OUT" || { echo "FAIL: engine line missing or not sw"; exit 1; }
grep -q  "rk_engine=gpu" "$OUT" && { echo "FAIL: TRIPWIRE gpu engine claimed in QEMU"; exit 1; }
grep -q  "rk_gpu_err="   "$OUT" && { echo "FAIL: TRIPWIRE gpu error counter in QEMU"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
# Flushed AREA of the first refresh -- 720*1280*4 at XRGB8888.  The partial-
# repaint guard: a corner repaint and a scene edit both just change the sums.
# Value greps are ANCHORED (CR-tolerant \r?$): 3686400 must not pass via a
# hypothetical 36864000, and a golden must match the WHOLE value.
grep -qE "LVGL_BYTES=3686400\r?$" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }
# GOLDEN CHECKSUMS -- FNV-1a over the whole 720x1280 framebuffer, ONE PER
# FEATURE AXIS (mode x theme rows, plus the accent screen) plus the 4x4 grid.
# Per-axis goldens are the acid-bass lesson: a single aggregate can silently
# stop testing half the feature.  RECORDED, not derived -- stable across two
# consecutive QEMU runs.  On a mismatch work out WHICH of {SynthUI pin, LVGL
# pin, lv_conf.h, fonts, scene} changed; do NOT paste in whatever the board
# printed.
#
# Provenance: recorded 2026-08-27 (NEW-20 Phase 2, synthui_rotary_knob
# replacing the old knob) against SynthUI local 2610cb8+ (pin bumped at
# close-out), vendored LVGL 9.4.0, XRGB8888 (LV_COLOR_DEPTH=32), montserrat
# 14/28; bit-identical across two consecutive QEMU runs.  THE FRAMES WERE
# LOOKED AT, not merely reproduced: -DRK_EYEBALL_HOLD builds (see
# CMakeLists.txt) held the grid and accent screens for a QEMU-monitor
# pmemsave (framebuffer 0x80300040 for THIS elf -- read LCDIFv2 0x4080820c,
# do not reuse another example's address), and the dumps' own FNV-1a equalled
# the pinned sums while the eye checked: endless/bounded wells, light/dark
# palettes (dark index #ffd24a), focus rings/tracks in the index color,
# muted disabled column, per-column angles -105/-35/+35/+105, and the four
# DC accent colors.  These are the
# SOFTWARE renderer's sums: on silicon the same ELF composites rotors on the
# GC355 (rk_engine=gpu) and produces a DIFFERENT, silicon-only golden set in
# transcript_hw_evkb.txt -- two golden sets, never reconciled
# (vglite_lvgl_test's precedent).  Hardware confirmation of THIS scene: see
# transcript_hw_evkb.txt (Task 11 of the Phase-2 plan).
# Demonstrated RED 2026-08-27: KNOB_SUM_ENDLESS_DARK's last digit flipped ->
# "FAIL: endless/dark checksum".
grep -qE "KNOB_SUM_ALL=0xB4256FB2\r?$"           "$OUT" || { echo "FAIL: grid checksum"; exit 1; }
grep -qE "KNOB_SUM_ENDLESS_LIGHT=0x89E8CD9B\r?$" "$OUT" || { echo "FAIL: endless/light checksum"; exit 1; }
grep -qE "KNOB_SUM_BOUNDED_LIGHT=0xD9617577\r?$" "$OUT" || { echo "FAIL: bounded/light checksum"; exit 1; }
grep -qE "KNOB_SUM_ENDLESS_DARK=0x04C2E8F0\r?$"  "$OUT" || { echo "FAIL: endless/dark checksum"; exit 1; }
grep -qE "KNOB_SUM_BOUNDED_DARK=0xD8FA331E\r?$"  "$OUT" || { echo "FAIL: bounded/dark checksum"; exit 1; }
grep -qE "KNOB_SUM_ACCENT=0x2B17212F\r?$"        "$OUT" || { echo "FAIL: accent checksum"; exit 1; }
# Wedge-delta guards (spec 2026-08-27-rotary-knob-delta-damage). EQUALITY is
# computed by the GATE from the two printed sums -- not a pinned golden, so
# it never re-goldens; a too-tight bbox (stale wedge pixels) fails here.
# Demonstrated RED 2026-08-27: bbox pad scratch-built to 0 ->
# "FAIL: delta render differs from full render (0x60142FF0 vs 0x6F8E52AC)".
DSEQ=$(grep -a -oE "KNOB_DELTA_SEQ=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
DFUL=$(grep -a -oE "KNOB_DELTA_FULL=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DSEQ" ] && [ -n "$DFUL" ] || { echo "FAIL: delta guard tokens missing"; exit 1; }
[ "$DSEQ" = "$DFUL" ] || { echo "FAIL: delta render differs from full render ($DSEQ vs $DFUL)"; exit 1; }
# ENGAGEMENT: the recorded per-step damage must be wedge-sized (measured
# 3050 px; a full 250px control is 62500), not the whole control -- a change
# that quietly reverts set_angle to full invalidation fails HERE and nowhere
# else. Demonstrated RED 2026-08-27: set_angle scratch-reverted to
# lv_obj_invalidate -> "FAIL: delta damage not engaged (maxarea=62500)".
DAREA=$(grep -a -oE "KNOB_DELTA_MAXAREA=[0-9]+" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DAREA" ] && [ "$DAREA" -gt 0 ] || { echo "FAIL: delta area guard missing or zero"; exit 1; }
[ "$DAREA" -le 8000 ] || { echo "FAIL: delta damage not engaged (maxarea=$DAREA)"; exit 1; }
grep -q "SYNTHUI_KNOB_DONE"    "$OUT" || { echo "FAIL: no completion token"; exit 1; }
echo "PASS: SynthUI knob render verified"
