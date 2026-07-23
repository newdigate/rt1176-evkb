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
# TEMP(Task 7) -- assert the QEMU LCDIFv2 model scanned our SDRAM framebuffer
# correctly (its debug scan-checksum tap == the software FNV-1a). This is NOT
# LCDIFV2_OK (the firmware LCDIFv2 driver, now live as of Task 8): the probe
# pokes the layer-0 registers by hand and reads the model's tap, independently
# of the real driver above. Removed at Task 11 when the checksum tap moves to
# the TC358762 bridge.
grep -Eq "PROBE_LCDIF=.*PASS" "$OUT" || { echo "FAIL: lcdifv2 scan probe"; exit 1; }
# Task 10 -- the firmware MIPI-DSI host driver (RPiDisplay mipi_dsi.cpp) brought
# the D-PHY up (PLL dividers + HS timing + bounded lock poll), powered the PHY
# and configured the DPI video mode, with every register reading back what it
# wrote. This REPLACES the Task-9 PROBE_DSI/PROBE_DSI_SHORT hand-poked probes.
# NOTE the QEMU model reports D-PHY LOCK the instant PD_PLL clears and stores
# every DPI register as plain RW, so DSI_OK proves the bring-up SEQUENCE, not
# that the D-PHY locks at the right bit rate -- that is Task 14 (silicon).
grep -q "DSI_OK"        "$OUT" || { echo "FAIL: dsi";     exit 1; }
# FB_SUM (+ PANEL_SUM) deferred: begin() doesn't return true overall until the
# TC358762 stage lands, so fillScreen() never runs yet.
# Restored at Task 13, which also adds the PANEL_SUM bridge-checksum oracle.
echo "PASS: RPi panel Task 10 (ATtiny + VIDEO_PLL + LCDIFv2 + MIPI-DSI host) verified"
