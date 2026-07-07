#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/eeprom_test.elf"; OUT="$DIR/eeprom.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/eeprom.dbg" &
P=$!; gate_pid $P; sleep 4; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "EEPROM_RW=PASS"     "$OUT" || { echo "FAIL: RW"; exit 1; }
grep -q "EEPROM_WEAR=PASS"   "$OUT" || { echo "FAIL: wear-leveling"; exit 1; }
# 4284 = EEPROM.length() = E2END(0x10BB=4283)+1 — proves the emulated region is sized right
grep -q "EEPROM_LENGTH=4284" "$OUT" || { echo "FAIL: length"; exit 1; }
grep -q "EEPROM_ALL=PASS"    "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: EEPROM flash-emulation verified (RW + wear-leveling + length)"
