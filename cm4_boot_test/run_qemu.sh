#!/bin/sh
# QEMU gate for the Phase-1 dual-core library (Multicore + MessagingUnit).
# Boots an embedded CM4 blob and exercises the MU through the library API.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_boot_test.elf"
OUT="$DIR/cm4_boot.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_boot.dbg" &
P=$!; gate_pid $P
# Wait (bounded) for the run to finish rather than a fixed sleep, so a slow
# QEMU start can't truncate the transcript.
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4BOOT-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output captured)"

fail=0
check() {   # check TOKEN=VALUE
    if grep -q "^$1" "$OUT"; then
        echo "PASS: $1"
    else
        echo "FAIL: expected $1"; fail=1
    fi
}
grep -q "CM4BOOT-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "boot=00000001"     # CM4 left reset
check "run=00000001"      # Multicore.running()
check "hello=C0FFEE42"    # mailbox receive
check "dbell=D00DFEED"    # doorbell handshake
check "giraft=00000000"   # GIR auto-cleared on the CM4's ack
check "echo=12345679"     # interrupt-driven receive (NVIC 118)
check "irqcnt=00000001"   # exactly one MU receive IRQ
check "run2=00000001"     # CM4 running before restart
check "hello2=C0FFEE42"   # CM4 rebooted by Multicore.restart()
grep -q "CM4BOOT-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: CM4 boot + MU library verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
