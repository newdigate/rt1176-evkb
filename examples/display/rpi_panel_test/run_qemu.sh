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
P=$!; gate_pid $P; sleep 5; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "ATTINY_OK"     "$OUT" || { echo "FAIL: attiny"; exit 1; }
grep -q "PLL_OK"        "$OUT" || { echo "FAIL: pll";    exit 1; }
# TEMP(Task 7) -- assert the QEMU LCDIFv2 model scanned our SDRAM framebuffer
# correctly (its debug scan-checksum tap == the software FNV-1a). This is NOT
# LCDIFV2_OK (the firmware LCDIFv2 driver is Task 8): the probe pokes the layer-0
# registers by hand and reads the model's tap. Removed at Task 11 when the
# checksum tap moves to the TC358762 bridge.
grep -Eq "PROBE_LCDIF=.*PASS" "$OUT" || { echo "FAIL: lcdifv2 scan probe"; exit 1; }
# FB_SUM (+ PANEL_SUM) deferred: begin() doesn't return true overall until the
# LCDIFv2/DSI/TC358762 stages land, so fillScreen() never runs yet.
# Restored at Task 13, which also adds the PANEL_SUM bridge-checksum oracle.
echo "PASS: RPi panel Task 7 (ATtiny + VIDEO_PLL + LCDIFv2 model scanout probe) verified"
