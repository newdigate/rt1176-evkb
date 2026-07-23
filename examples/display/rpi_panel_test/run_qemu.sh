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
# FB_SUM (+ PANEL_SUM) deferred: begin() doesn't return true overall until the
# DSI/TC358762 stages land, so fillScreen() never runs yet.
# Restored at Task 13, which also adds the PANEL_SUM bridge-checksum oracle.
echo "PASS: RPi panel Task 8 (ATtiny + VIDEO_PLL + LCDIFv2 driver) verified"
