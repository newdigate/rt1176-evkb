#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/rk055_panel_test.elf"; OUT="$DIR/rk055_panel.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/rk055_panel.dbg" &
P=$!; gate_pid $P
# 8s: the same cold-binary margin the sibling display gates use -- the first run
# of a freshly-built image can otherwise be cut off before setup() finishes
# (SEMC init + a 1.8 MB extmem framebuffer + LCDIFv2 config).
sleep 8; kill $P 2>/dev/null; wait $P 2>/dev/null || true
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
echo "PASS: RK055 panel M2 (clocks + LCDIFv2 + MIPI-DSI host + HX8394 panel init at 720x1280, 2 lanes)"
