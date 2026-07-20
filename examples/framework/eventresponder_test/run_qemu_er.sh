#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/eventresponder_test.elf"; OUT="$DIR/er.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/er.dbg" &
P=$!; gate_pid $P; sleep 5; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "STAGE_IMMEDIATE=PASS" "$OUT" || { echo "FAIL: immediate"; exit 1; }
grep -q "STAGE_YIELD=PASS"     "$OUT" || { echo "FAIL: yield";     exit 1; }
grep -q "STAGE_CLEAR=PASS"     "$OUT" || { echo "FAIL: clear";     exit 1; }
grep -q "STAGE_STATUS=PASS"    "$OUT" || { echo "FAIL: status";    exit 1; }
grep -q "EVENTRESPONDER_ALL=PASS" "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: EVENTRESPONDER_ALL"
