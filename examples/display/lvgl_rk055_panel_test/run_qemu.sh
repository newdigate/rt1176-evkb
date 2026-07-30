#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location (a
# hardcoded path silently loads a different tree's gate-lib.sh from a worktree).
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/build/lvgl_rk055_panel_test.elf"; OUT="$DIR/lvgl_rk055_panel.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/lvgl_rk055_panel.dbg" &
P=$!; gate_pid $P
# 12s: panel bring-up (PLL roots + LCDIFv2 + MIPI-DSI + HX8394) plus a 720x1280
# software render -- the same cold-binary margin lvgl_rpi_panel_test uses for
# its 800x480.
sleep 12; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
# The panel chain itself must come up before LVGL means anything: in DIRECT
# mode LVGL renders straight into the LCDIFv2's live scanout buffer, so a
# framebuffer no display owns would still checksum perfectly.
grep -q "PANEL_OK"          "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
# The flushed AREA -- 720*1280*2 -- is the only evidence LVGL redrew the WHOLE
# screen; a partial repaint and a scene edit both just change LVGL_SUM.
grep -q "LVGL_BYTES=1843200" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }
# GOLDEN CHECKSUM -- FNV-1a over the whole 720x1280 framebuffer after LVGL's
# software renderer drew the scene.  RECORDED, not derived; provenance below.
# UNLIKE the RPi gate's golden, this one is CONFIRMED ON GLASS by a human
# (transcript_hw_evkb.txt), so it pins correctness, not just reproducibility.
# On a mismatch work out WHICH of {LVGL pin, lv_conf.h, fonts, scene} changed;
# do NOT paste in whatever the board printed.  TO RE-RECORD: stable across two
# runs AND a human eye on the glass, in the SAME commit.
#
# Provenance: recorded 2026-07-29 against vendored LVGL 9.4.0, lv_conf.h as of
# that commit, montserrat 14/28.
grep -q "LVGL_SUM=0xDF2A1A03" "$OUT" || { echo "FAIL: render checksum"; exit 1; }
echo "PASS: LVGL RK055 panel render verified"
