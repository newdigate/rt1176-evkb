#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/rpi_panel_test.elf"; OUT="$DIR/rpi_panel.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/rpi_panel.dbg" &
P=$!; gate_pid $P
# sleep 8 (was 5 through Task 7): known rt1170-qemu-class timing flake -- the
# first run of a freshly-`ninja`-built binary can be cut off before setup()
# finishes (SEMC init + extmem + LCDIFv2 config takes longer on a cold
# binary). 8s gives margin.
sleep 8; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "ATTINY_OK"     "$OUT" || { echo "FAIL: attiny"; exit 1; }
grep -q "PLL_OK"        "$OUT" || { echo "FAIL: pll";    exit 1; }
grep -q "LCDIFV2_OK"    "$OUT" || { echo "FAIL: lcdifv2"; exit 1; }
# Task 10 -- the firmware MIPI-DSI host driver (RPiDisplay mipi_dsi.cpp) brought
# the D-PHY up (PLL dividers + HS timing + bounded lock poll), powered the PHY
# and configured the DPI video mode, with every register reading back what it
# wrote. This REPLACES the Task-9 PROBE_DSI/PROBE_DSI_SHORT hand-poked probes.
# NOTE the QEMU model reports D-PHY LOCK the instant PD_PLL clears and stores
# every DPI register as plain RW, so DSI_OK proves the bring-up SEQUENCE, not
# that the D-PHY locks at the right bit rate -- that is Task 14 (silicon).
grep -q "DSI_OK"        "$OUT" || { echo "FAIL: dsi";     exit 1; }
# Task 12 -- the firmware TC358762 bridge driver (RPiDisplay tc358762.cpp) sent
# all eleven init writes of NXP's transcribed RPi-panel sequence as DSI generic
# long writes, and every one was accepted by the DSI host's APB packet path.
# This REPLACES the Task-11 PROBE_BRIDGE hand-poked probe (that block is gone).
# NOTE the QEMU bridge never interprets a register address or its data -- its
# required-init contract is region+order based -- so TC358762_OK proves the
# sequence is well-formed and correctly ORDERED, never that the ADDRESSES or
# VALUES are right. Silicon is the only oracle for those (Task 14).
grep -q "TC358762_OK"   "$OUT" || { echo "FAIL: tc358762"; exit 1; }
# FB_SUM (+ PANEL_SUM) still deferred, and deliberately NOT asserted here:
# begin() now returns true, so the harness's if(ok) block runs, but fillScreen()
# is a Task-13 stub -- it prints FB_SUM_PENDING / PANEL_SUM_PENDING rather than
# checksumming an unpainted framebuffer. Task 13 implements the PXP fill and
# restores both comparisons (including the PANEL_SUM bridge oracle).
echo "PASS: RPi panel Task 12 (ATtiny + VIDEO_PLL + LCDIFv2 + MIPI-DSI host + TC358762 bridge init) verified"
