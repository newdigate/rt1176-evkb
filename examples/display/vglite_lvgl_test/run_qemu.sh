#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location (a
# hardcoded path silently loads a different tree's gate-lib.sh from a worktree).
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/vglite_lvgl_test.elf"
# Run artifacts go through gate_capture_path, never "$DIR/<name>".
OUT=$(gate_capture_path "$DIR" vglite_lvgl_test.uart)
DBG=$(gate_capture_path "$DIR" vglite_lvgl_test.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# 30s: this renders a 16-knob scene in SOFTWARE under QEMU, which is the slow
# path by construction. synthui_knob_test's grid needs ~16 s of gate time on the
# same machine; the margin is for load, not for hope.
sleep 30; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "PANEL_OK" "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }

# ★ WHAT THIS GATE PROVES, AND WHAT IT DOES NOT.
#
# QEMU has no GC355 model, so this asserts the SOFTWARE path: that a binary
# built WITH LVGL's VG_LITE draw unit compiled in still detects an absent GPU
# and renders the scene in software. That is the one-image-two-paths contract,
# and it is a real assertion -- vg_lite_init() SPINS rather than failing on
# absent hardware, so a regression that dropped the chip-ID probe would hang
# here and fail by timeout.
#
# It proves NOTHING about the GPU path. That is silicon-only and lives in
# transcript_hw_evkb.txt. See docs/KNOWN-BROKEN-GATES.md.
grep -qE "VGLITE_CHIP_ID=0x[0-9A-F]{8}\r?$" "$OUT" || \
    { echo "FAIL: no chip-ID probe result"; exit 1; }
grep -q "VGLITE_INIT=ABSENT" "$OUT" || \
    { echo "FAIL: expected VGLITE_INIT=ABSENT under QEMU (no GC355 model)"; exit 1; }
grep -q "VGLITE_LVGL=SOFTWARE" "$OUT" || \
    { echo "FAIL: expected the SOFTWARE render path under QEMU"; exit 1; }

# The scene actually painted: whole-screen flush, not a partial repaint.
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: LVGL never flushed a frame"; exit 1; }
grep -q "LVGL_BYTES=3686400" "$OUT" || \
    { echo "FAIL: first refresh did not paint the whole 720x1280x4 screen"; exit 1; }

# ★ SOFTWARE golden. The GPU path produces DIFFERENT pixels by construction --
# hardware antialiasing is not LVGL's mask arithmetic, and this build runs with
# LV_USE_FLOAT=1 so coordinates round differently. Two goldens, never one; the
# GPU value is recorded in transcript_hw_evkb.txt and must NOT be copied here.
grep -q "KNOB_GRID_SUM_SW=0x1C5D9DE4" "$OUT" || \
    { echo "FAIL: software grid golden"; exit 1; }

grep -q "VGLITE_LVGL_DONE" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
echo "PASS: LVGL VG_LITE backend, software fallback verified"
