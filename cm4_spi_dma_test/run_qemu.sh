#!/bin/sh
# QEMU gate for Phase-4.3: the CM4 self-configures LPSPI1 and runs full-duplex
# DMA self-loopback in two stages (blocking poll + async IRQ on the CM4's own
# NVIC via the eDMA split). QEMU's ssi-loopback echoes on CR.MEN alone, so
# rxb/rxa=1 proves the TCD/DMA sequence; the real SDO->SDI jumper on HW proves
# clock+pins+SCK. dmairq>0 is the isolated split-IRQ proof.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_spi_dma_test.elf"
OUT="$DIR/cm4_spi_dma.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_spi_dma.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4SPIDMA-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4SPIDMA-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "ready=CAFE0001"
check "cr=00000001"
check "cfgr1=00000001"
check "rxb=00000001"
check "rxa=00000001"
check "done=00000001"
grep -q "^dmairq=00000000" "$OUT" && { echo "FAIL: dmairq is 0 (no CM4 eDMA IRQ)"; fail=1; }
check "SPI_DMA_CM4=PASS"
grep -q "CM4SPIDMA-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: CM4 DMA SPI verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
