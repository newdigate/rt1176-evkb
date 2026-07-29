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
ELF="$DIR/build/eeprom_test.elf"; OUT="$DIR/eeprom.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/eeprom.dbg" &
P=$!; gate_pid $P
# Poll for EEPROM_ALL -- the capture's last line, printed after every token
# asserted below -- instead of always burning a fixed window. Same 8 s CEILING
# (32 x 0.25 s) as the `sleep 4` this replaces, doubled because the sketch now
# does ~4.3k extra reads for the iteration stage and a full 63-sector rescan for
# the persist stage on top of the 2100 wear writes. A healthy run exits as soon
# as the token lands; a hung one is still killed here and still goes red on the
# missing token below, never on a silent timeout.
for _ in $(seq 1 32); do
    [ -f "$OUT" ] && grep -q "EEPROM_ALL=" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "EEPROM_RW=PASS"      "$OUT" || { echo "FAIL: RW"; exit 1; }
grep -q "EEPROM_WEAR=PASS"    "$OUT" || { echo "FAIL: wear-leveling"; exit 1; }
grep -q "EEPROM_API=PASS"     "$OUT" || { echo "FAIL: API surface (update/operator[]/iteration)"; exit 1; }
grep -q "EEPROM_PERSIST=PASS" "$OUT" || { echo "FAIL: cold-start rescan"; exit 1; }
# FIRST, not merely "present": QEMU has no backing store behind the FlexSPI
# window, so a RETURN here would mean the model had grown persistence and this
# assertion had stopped testing what it claims. The RETURN case is proven on
# hardware instead -- see transcript_hw_evkb.txt.
grep -q "EEPROM_BOOT=FIRST"   "$OUT" || { echo "FAIL: boot marker not FIRST (QEMU cannot persist)"; exit 1; }
# 4284 = EEPROM.length() = E2END(0x10BB=4283)+1 — proves the emulated region is sized right
grep -q "EEPROM_LENGTH=4284"  "$OUT" || { echo "FAIL: length"; exit 1; }
grep -q "EEPROM_ALL=PASS"     "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: EEPROM flash-emulation verified (RW + wear + API + cold-start rescan + length)"
