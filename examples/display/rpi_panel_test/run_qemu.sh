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
grep -q "FB_SUM=.*PASS" "$OUT" || { echo "FAIL: fb_sum"; exit 1; }
echo "PASS: RPi panel v1 verified"
