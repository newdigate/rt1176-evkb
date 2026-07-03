#!/bin/sh
set -e
QEMU=~/Development/qemu2/build/qemu-system-arm
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/wire_slave_test.elf"; OUT="$DIR/wire_slave.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/wire_slave.dbg" &
P=$!; sleep 3; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "rx_count=3"        "$OUT" || { echo "FAIL: onReceive count"; exit 1; }
grep -q "rx=0xAA 0xBB 0xCC" "$OUT" || { echo "FAIL: onReceive data"; exit 1; }
grep -q "wr_status=0"       "$OUT" || { echo "FAIL: master write"; exit 1; }
grep -q "rd(1)=0x11"        "$OUT" || { echo "FAIL: onRequest read"; exit 1; }
echo "PASS: Wire I2C slave verified (onReceive multi-byte + onRequest via loopback)"
