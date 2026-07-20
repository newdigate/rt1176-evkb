#!/bin/sh
# QEMU gate for Phase-2D capstone: dual-sketch blink + IPC. The CM7 blinks its
# LED and drives an MU IPC exchange; the CM4 drives its own GPIO and computes
# the IPC responses.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_dual_test.elf"
OUT="$DIR/cm4_dual.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_dual.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4DUAL-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4DUAL-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "cm7led=00000001"    # CM7 blinked its LED (GPIO3.3), left high
check "cm4gpio=00001000"   # CM4 drove its own GPIO5.12 high (its DR readback)
check "cm7sees=00001000"   # CM7 reads the same GPIO5.DR cross-core (shared peripheral)
check "ipc=00000037"       # bidirectional IPC: CM4 computed f(0x10)=0x37 in its ISR
grep -q "CM4DUAL-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: dual-sketch blink + IPC capstone verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
