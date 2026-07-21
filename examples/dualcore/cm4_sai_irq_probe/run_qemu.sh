#!/bin/sh
# QEMU gate for cm4_sai_irq_probe: does SAI1's FIFO-request IRQ (76) reach the
# CM4 NVIC? EXPECTED-FAIL in QEMU today (irqcnt=00000000): qemu2 wires the SAI
# IRQs to the CM7 NVIC only (fsl-imxrt1170.c SAI wiring). The EVKB is the
# oracle; the qemu2 fan-out task flips this gate green.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_sai_irq_probe.elf"
OUT="$DIR/cm4_sai_irq.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_sai_irq.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4SAIIRQ-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"
grep -q "CM4SAIIRQ-DONE" "$OUT" || { echo "FAIL: no done"; exit 1; }
grep -q "SAI_IRQ_CM4=PASS" "$OUT" || { echo "FAIL: no CM4 SAI IRQ"; exit 1; }
echo "PASS: SAI_IRQ_CM4"
