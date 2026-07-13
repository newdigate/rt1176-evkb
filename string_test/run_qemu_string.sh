#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/string_test.elf"; OUT="$DIR/string.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/string.dbg" &
P=$!; gate_pid $P; sleep 20; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "STRING_ALL=PASS" "$OUT" || { echo "FAIL: string"; exit 1; }
grep -q "PRINTSTR:print-me-via-Print-chunks-print-me-via-Print-chunks-END" "$OUT" || { echo "FAIL: print-string path"; exit 1; }
grep -q "GATE=DONE" "$OUT" || { echo "FAIL: completion"; exit 1; }
echo "PASS: string gate"
