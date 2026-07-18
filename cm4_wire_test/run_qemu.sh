#!/bin/sh
# QEMU gate for Phase-3.2: the CM4 self-configures LPI2C5 and runs polled I2C
# transactions against the wm8962-stub; the CM7 reports over the MU on LPUART1.
# NOTE: the LPI2C model + stub respond on MCR.MEN alone (clock/pins ignored),
# so QEMU proves the register/transfer SEQUENCE only — the wiring-free HW run
# proves the CM4's clock-gating + LPSR pin-mux (see README / spec).
# rdv is WORLD-SPLIT by design: this runner asserts the stub contract
# (rdv=00000000); the HW check asserts the WM8962 device ID (rdv=00006243).
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_wire_test.elf"
OUT="$DIR/cm4_wire.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_wire.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4WIRE-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4WIRE-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "mcr=00000001"      # LPI2C MCR.MEN set (master enabled)
check "ack=00000000"      # WM8962 reset-write ACKed (err 0)
check "nack=00000002"     # absent addr 0x2A -> address NACK (err 2)
check "rdn=00000002"      # ID read-back returned 2 bytes
check "rdv=00000000"      # stub contract: all reads 0x00 (HW expects 00006243)
check "done=00000001"     # CM4 sequence completed
check "WIRE_CM4=PASS"     # verdict
# lpcg= / croot= are printed for HW diagnosis but intentionally NOT asserted.
grep -q "CM4WIRE-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: CM4 self-configured polled I2C verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
