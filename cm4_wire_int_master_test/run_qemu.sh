#!/bin/sh
# QEMU gate for Phase-4.1: the CM4 self-configures LPI2C5 (as cm4_wire_test)
# and runs an INTERRUPT-DRIVEN master — it NVIC-enables IRQ 36 on its own NVIC
# (the first non-MU peripheral IRQ routed to the CM4, via the qemu2 split-IRQ)
# and services LPI2C5 in its own ISR to read the WM8962 R15 ID. The runner
# asserts irqcnt>0 (the CM4 took the IRQ) and err=0 (transaction OK).
# rdv is WORLD-SPLIT by design: this runner asserts the stub contract
# (rdv=00000000); the HW check asserts the WM8962 device ID (rdv=00006243),
# same as cm4_wire_test's precedent.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_wire_int_master_test.elf"
OUT="$DIR/cm4_wire_int_master.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_wire_int_master.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4WIREINT-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4WIREINT-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "mcr=00000001"
check "rdv=00000000"                 # stub contract (HW asserts 00006243)
check "err=00000000"                 # ISR transaction OK (no NDF/ALF/FEF)
check "done=00000001"
grep -q "^irqcnt=00000000" "$OUT" && { echo "FAIL: irqcnt is 0 (no CM4 IRQ)"; fail=1; }
check "WIRE_INT_MASTER_CM4=PASS"
# lpcg= / croot= are printed for HW diagnosis but intentionally NOT asserted.
grep -q "CM4WIREINT-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: CM4 interrupt-driven I2C master verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
