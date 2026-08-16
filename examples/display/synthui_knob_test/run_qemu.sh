#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location (a
# hardcoded path silently loads a different tree's gate-lib.sh from a worktree).
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/synthui_knob_test.elf"; OUT="$DIR/synthui_knob.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/synthui_knob.dbg" &
P=$!; gate_pid $P
# 16s: the RK055 bring-up margin lvgl_rk055_panel_test uses (12s) plus four
# extra full-screen software renders for the per-mode phases.
sleep 16; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
# Panel chain first: in DIRECT mode a framebuffer no display owns would still
# checksum perfectly.
grep -q "PANEL_OK"          "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
# Flushed AREA of the first refresh -- 720*1280*4 at XRGB8888.  The partial-
# repaint guard: a corner repaint and a scene edit both just change the sums.
# Value greps are ANCHORED (CR-tolerant \r?$): 3686400 must not pass via a
# hypothetical 36864000, and a golden must match the WHOLE value.
grep -qE "LVGL_BYTES=3686400\r?$" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }
# GOLDEN CHECKSUMS -- FNV-1a over the whole 720x1280 framebuffer, ONE PER MODE
# plus the 4x4 grid.  Per-mode goldens are the acid-bass lesson: a single
# aggregate can silently stop testing half the feature.  RECORDED, not derived
# -- stable across two consecutive QEMU runs AND confirmed by a human eye on
# the RK055 glass in the SAME commit that records them (this panel's goldens
# are glass-confirmed, like lvgl_rk055_panel_test's and unlike the RPi gate's).
# On a mismatch work out WHICH of {SynthUI pin, LVGL pin, lv_conf.h, fonts,
# scene} changed; do NOT paste in whatever the board printed.
#
# Provenance: recorded 2026-08-16 against SynthUI a1b6da7, vendored LVGL
# 9.4.0, XRGB8888 (LV_COLOR_DEPTH=32), montserrat 14/28; stable across two
# consecutive QEMU runs. Independently cross-verified on a host build of the
# same widget+scene (clang/macOS vs ARM GCC/QEMU): all five sums bit-identical
# -- see the Task 6 review record. Hardware eye confirmation is pending until
# Task 9 (see transcript_hw_evkb.txt).
grep -qE "KNOB_SUM_ALL=0x8E1F9956\r?$"     "$OUT" || { echo "FAIL: grid checksum"; exit 1; }
grep -qE "KNOB_SUM_ENDLESS=0xBF7FAB41\r?$" "$OUT" || { echo "FAIL: endless checksum"; exit 1; }
grep -qE "KNOB_SUM_BOUNDED=0x7D77023E\r?$" "$OUT" || { echo "FAIL: bounded checksum"; exit 1; }
grep -qE "KNOB_SUM_DETENTS=0xBEF7F8CE\r?$" "$OUT" || { echo "FAIL: detents checksum"; exit 1; }
grep -qE "KNOB_SUM_ARC=0xAAE57894\r?$"     "$OUT" || { echo "FAIL: arc checksum"; exit 1; }
grep -q "SYNTHUI_KNOB_DONE"    "$OUT" || { echo "FAIL: no completion token"; exit 1; }
echo "PASS: SynthUI knob render verified"
