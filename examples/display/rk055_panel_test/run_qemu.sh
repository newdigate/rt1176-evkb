#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location. The old
# hardcoded ~/Development/rt1170/evkb/tools/... meant a worktree or a clone at
# any other path silently loaded a DIFFERENT tree's gate-lib.sh -- which surfaces
# as "gate_reap: command not found", or worse, as a gate quietly running against
# the wrong library.
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/build/rk055_panel_test.elf"; OUT="$DIR/rk055_panel.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/rk055_panel.dbg" &
P=$!; gate_pid $P
# 8s: the same cold-binary margin the sibling display gates use -- the first run
# of a freshly-built image can otherwise be cut off before setup() finishes
# (SEMC init + a 1.8 MB extmem framebuffer + LCDIFv2 config).
sleep 8; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
# M1 -- the SoC chain. CLK_OK covers the three CCM roots. There is no VIDEO_PLL
# on this panel: it sources both the pixel and escape clocks from PLL_528, which
# the boot ROM already locked, so displayClockInit() verifies SYS_PLL2 directly
# rather than bringing a PLL up.
grep -q "CLK_OK"     "$OUT" || { echo "FAIL: clocks";  exit 1; }
grep -q "LCDIFV2_OK" "$OUT" || { echo "FAIL: lcdifv2"; exit 1; }
# NOTE the QEMU model reports D-PHY LOCK as soon as PD_PLL clears and stores
# every DPI register as plain RW, so DSI_OK proves the bring-up SEQUENCE is
# well-formed and correctly ordered -- NOT that the D-PHY locks at ~792 MHz over
# two lanes. That is silicon-only and is what the M1 hardware run settles.
grep -q "DSI_OK"     "$OUT" || { echo "FAIL: dsi";     exit 1; }
# M2 -- the HX8394 driver sent the fsl_hx8394.c sequence over the DSI link and
# the virtual panel accepted it. NOTE the QEMU model checks the ORDER and the
# lane agreement, never the 21 tuning commands' VALUES (they are panel
# calibration data it has no way to judge) -- silicon is the only oracle for
# those.
grep -q "PANEL_OK" "$OUT" || { echo "FAIL: hx8394"; exit 1; }
# M3 -- pixels. FRAME_OK: a whole LCDIFv2 frame scanned out after the paint.
# Asserted separately from the checksum because the checksum cannot see it:
# pixels read the same whether the display is scanning or stopped dead.
grep -q "FRAME_OK"      "$OUT" || { echo "FAIL: no scanout frame"; exit 1; }
# FB_SUM: FNV-1a of the SDRAM framebuffer vs a software-computed expectation.
# Proves drawTestPattern() wrote what it was asked to.
grep -q "FB_SUM=.*PASS" "$OUT" || { echo "FAIL: fb_sum"; exit 1; }
# PANEL_SUM: the virtual HX8394's received-pixel checksum vs the SAME
# expectation. QEMU-ONLY BY CONSTRUCTION -- the tap is emulator fiction with no
# silicon counterpart. On hardware the gate prints PANEL_SUM_HW=TAP_ABSENT and
# the panel is verified by eye.
grep -q "PANEL_SUM=.*PASS" "$OUT" || { echo "FAIL: panel_sum"; exit 1; }
# UNDERRUN: LCDIFV2_INT_STATUS_D0[1] sampled over N frames. HARDWARE-ONLY BY
# CONSTRUCTION -- QEMU has no timing model, so a QEMU UNDERRUN=0/N proves
# NOTHING about the ~108 MB/s scanout this panel demands. Asserted PRESENT here
# so it can never be silently dropped; its VALUE only means something in
# transcript_hw_evkb.txt.
grep -q "UNDERRUN=" "$OUT" || { echo "FAIL: underrun token missing"; exit 1; }
echo "PASS: RK055 panel M3 (clocks + LCDIFv2 + MIPI-DSI host + HX8394 panel init"
echo "      + colour-bar/diagonal test pattern on glass at 720x1280, 2 lanes:"
echo "      scanout frame, FB_SUM == PANEL_SUM == software expectation,"
echo "      underrun sampler present)"
