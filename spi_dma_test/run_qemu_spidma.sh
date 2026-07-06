#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/spi_dma_test.elf"; OUT="$DIR/spidma.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/spidma.dbg" &
P=$!; sleep 5; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "STAGE_BLOCKING=PASS" "$OUT" || { echo "FAIL: blocking"; exit 1; }
grep -q "STAGE_ASYNC=PASS"    "$OUT" || { echo "FAIL: async";    exit 1; }
grep -q "SPI_DMA_ALL=PASS"    "$OUT" || { echo "FAIL: overall";  exit 1; }
echo "PASS: SPI_DMA_ALL"
