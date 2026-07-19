#!/bin/sh
# QEMU gate for Phase-4.2: the CM4 runs an interrupt-driven I2C slave @0x42 on
# the LPI2C2 persona (IRQ 33 on its own NVIC via the qemu2 split); the CM7
# masters LPI2C1 across the bridged bus with the shared polled core.
# Single-byte slave response only in QEMU (documented model limit); multi-byte
# and the external-master topology are HW-verified by the EVKB probe.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_wire_int_slave_test.elf"
OUT="$DIR/cm4_wire_int_slave.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_wire_int_slave.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4WIRESLV-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4WIRESLV-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "ready=CAFE0001"
check "wr=00000000"
check "mrd=0000003C"
check "b0=000000A5"
check "b1=0000005A"
check "b2=000000C3"
check "resp=0000003C"
check "err=00000000"
check "done=00000001"
grep -q "^irqcnt=00000000" "$OUT" && { echo "FAIL: irqcnt is 0 (no CM4 IRQ)"; fail=1; }
check "WIRE_INT_SLAVE_CM4=PASS"
grep -q "CM4WIRESLV-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: CM4 interrupt-driven I2C slave verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
