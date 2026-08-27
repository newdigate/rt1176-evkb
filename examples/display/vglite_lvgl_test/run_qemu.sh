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
# It runs the SOFTWARE build (build/, EVKB_VGLITE=OFF). The GPU build lives in
# build-vglite/ and is silicon-only, because LVGL's VG_LITE draw unit registers
# in lv_init() whether or not a GPU exists and then claims tasks it cannot
# execute -- see the example's header comment for the measurement.
#
# So this gates the SCENE and the software renderer, and it is the baseline the
# fps comparison is measured against. It proves NOTHING about the GPU path;
# that lives in transcript_hw_evkb.txt. See docs/KNOWN-BROKEN-GATES.md.
grep -q "VGLITE_INIT=NOTBUILT" "$OUT" || \
    { echo "FAIL: expected the software build (EVKB_VGLITE=OFF)"; exit 1; }
grep -q "VGLITE_LVGL=SOFTWARE" "$OUT" || \
    { echo "FAIL: expected the SOFTWARE render path"; exit 1; }

# The scene painted the whole screen rather than a partial repaint.
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: LVGL never flushed a frame"; exit 1; }
grep -q "LVGL_BYTES=3686400" "$OUT" || \
    { echo "FAIL: first refresh did not paint the whole 720x1280x4 screen"; exit 1; }

# ★ THE BLACK-SCREEN GUARD, and it is not hypothetical -- it is the defect this
# example was written on top of. 0x9BC99DC5 is exactly the FNV-1a of 3686400
# zero bytes, and the first version of this scene produced it while reporting
# LVGL_FLUSHED=PASS and a full-screen LVGL_BYTES. Those two prove a flush
# HAPPENED and covered the screen, never that anything was DRAWN in it. Assert
# the blank value by name so the failure says what it is.
grep -q "KNOB_GRID_SUM_SW=0x9BC99DC5" "$OUT" && \
    { echo "FAIL: framebuffer is ALL ZEROS -- flushed, but nothing drawn"; exit 1; }

# ★ SOFTWARE golden. The GPU build produces DIFFERENT pixels by construction
# (hardware AA is not LVGL's masks, and that build carries LV_USE_FLOAT=1), so
# its value is recorded in transcript_hw_evkb.txt and must NOT be copied here.
# Re-goldened 2026-08-27 (NEW-20 Phase 2): synthui_rotary_knob replaced the
# old knob, rows became mode x theme (0x513C4DB8 was the old knob's scene).
# Bit-identical across two runs; the frame was dumped via the QEMU monitor
# (pmemsave recipe in acid_box/transcript_qemu.txt, fb 0x80100040 for this
# elf) and inspected -- its own FNV-1a equals this value.
grep -q "KNOB_GRID_SUM_SW=0x579E5810" "$OUT" || \
    { echo "FAIL: software grid golden"; exit 1; }

grep -q "VGLITE_LVGL_DONE" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
echo "PASS: knob grid renders, software path"
