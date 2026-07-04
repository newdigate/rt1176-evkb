#!/bin/sh
set -e
QEMU=~/Development/qemu2/build/qemu-system-arm
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/irq_attach_test.elf"; OUT="$DIR/irq.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/irq.dbg" &
P=$!; sleep 3; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "IRQ=PASS" "$OUT" || { echo "FAIL: attachInterrupt"; exit 1; }
echo "PASS: attachInterrupt verified (RISING/CHANGE/detach)"
