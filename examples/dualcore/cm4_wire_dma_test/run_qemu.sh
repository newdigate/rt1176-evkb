#!/bin/sh
# QEMU gate for Phase-4 eDMA_LPSR DMA-Wire: the CM4 self-configures LPI2C5 +
# eDMA_LPSR and DMA-reads the WM8962 R15 device ID, taking the eDMA_LPSR
# channel-0 completion IRQ on its OWN NVIC (CM4 IRQ 0, native — the main eDMA's
# channel IRQs are CM7-domain, so this is the true CM4 interrupt-DMA proof). The
# runner asserts dmairq>0 (the CM4 took the eDMA_LPSR IRQ) and err=0 (DMA OK).
# rdv is WORLD-SPLIT by design: QEMU's wm8962-stub reads 0x0000; the HW check
# asserts the WM8962 device ID (rdv=00006243) — so rdv is printed but not
# asserted here.
#
# Task 3 is the RED scaffold: the CM4 emits only READY, so croot/rdv/dmairq/err/
# done TIMEOUT and this gate ends WIRE_DMA_CM4=FAIL / GATE FAILED (exit 1). That
# RED is the Task-3 pass criterion; Task 4 turns it GREEN.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_wire_dma_test.elf"
OUT="$DIR/cm4_wire_dma.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_wire_dma.dbg" &
P=$!; gate_pid $P
# RED-scaffold budget: with only READY emitted, the CM7 reporter spins through
# 5 token-timeouts (5 x WAIT_LONG) before printing the FAIL verdict + DONE
# (~13 s), vs. 4.1's instant token stream. The stop-grep breaks early once the
# CM4 eDMA_LPSR firmware lands (Task 4, tokens arrive fast), so the larger
# budget costs nothing in GREEN.
for _ in $(seq 1 100); do
    [ -f "$OUT" ] && grep -q "CM4WIREDMA-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4WIREDMA-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "ready=CAFE0001"
grep -q "^dmairq=00000000" "$OUT" && { echo "FAIL: dmairq is 0 (no CM4 eDMA_LPSR IRQ)"; fail=1; }
check "err=00000000"                 # DMA/transaction OK (no NDF/ALF/FEF)
check "done=00000001"
# croot= is printed for HW diagnosis but intentionally NOT asserted; rdv= is
# WORLD-SPLIT (stub 0x0000 / HW 0x6243) so it is printed but not asserted here.
check "WIRE_DMA_CM4=PASS"
check "CM4WIREDMA-DONE"

if [ $fail -eq 0 ]; then
    echo "PASS: CM4 eDMA_LPSR DMA-Wire verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
