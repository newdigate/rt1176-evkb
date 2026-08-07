#!/bin/sh
# QEMU gate for Phase-2C: the CM4 stands up its own DWT timing + SysTick + NVIC
# (external MU IRQ 118) and reports over the MU.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location. The old
# hardcoded ~/Development/rt1170/evkb/tools/... meant a worktree or a clone at
# any other path silently loaded a DIFFERENT tree's gate-lib.sh -- which surfaces
# as "gate_reap: command not found", or worse, as a gate quietly running against
# the wrong library.
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/cm4_intr_test.elf"
OUT="$DIR/cm4_intr.uart"
rm -f "$OUT"
# -icount gives SysTick/DWT a deterministic time base (see IntervalTimer gates).
"$QEMU" $(gate_qemu_machine) -icount shift=2 -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_intr.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 60); do
    [ -f "$OUT" ] && grep -q "CM4INTR-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"

echo "==== captured UART ===="; cat "$OUT"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4INTR-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "boot=00000001"     # CM4 left reset
check "run=00000001"      # Multicore.running()
check "dwt=D00D0001"      # CM4 DWT CYCCNT advanced (timing base)
check "irqecho=ABCD0001"  # CM4 handled MU IRQ 118 in its own ISR (NVIC dispatch)
grep -q "CM4INTR-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

# systick is a characterisation probe (not asserted): report whatever it read.
echo "--- characterisation (not asserted) ---"
grep "^systick=" "$OUT" || echo "systick=(missing)"

if [ $fail -eq 0 ]; then
    echo "PASS: CM4 NVIC + DWT timing verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
