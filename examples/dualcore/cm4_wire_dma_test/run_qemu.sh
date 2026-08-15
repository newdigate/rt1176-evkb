#!/bin/sh
# QEMU gate for Phase-4 eDMA_LPSR DMA-Wire: the CM4 self-configures LPI2C5 +
# eDMA_LPSR and DMA-reads the WM8962 R15 device ID, taking the eDMA_LPSR
# channel-0 completion IRQ on its OWN NVIC (CM4 IRQ 0, native — the main eDMA's
# channel IRQs are CM7-domain, so this is the true CM4 interrupt-DMA proof). The
# runner asserts dmairq>0 (the CM4 took the eDMA_LPSR IRQ) and err=0 (DMA OK).
# rdv is printed but NOT asserted here. It used to be world-split -- QEMU's
# wm8962 stub read 0x0000 while HW read the 0x6243 device ID -- and that split
# was the whole reason for not asserting it. The split is gone as of 2026-08-15:
# the stub was replaced by a real WM8962 control-interface model (qemu2
# 23a86da9c3) that returns the true 0x6243 for R15, so this gate now reports
# rdv=00006243 in BOTH worlds, the same as cm4_wire_test and
# cm4_wire_int_master_test, which do assert it.
# ★ Asserting rdv here is now possible and would strengthen this gate; it is
# left unasserted only because nobody has reviewed that change, not because of
# the split. Do not re-add a world-split rationale -- it is no longer true.
#
# Task 3 is the RED scaffold: the CM4 emits only READY, so croot/rdv/dmairq/err/
# done TIMEOUT and this gate ends WIRE_DMA_CM4=FAIL / GATE FAILED (exit 1). That
# RED is the Task-3 pass criterion; Task 4 turns it GREEN.
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
ELF="$DIR/$(gate_build_dir)/cm4_wire_dma_test.elf"
OUT="$DIR/cm4_wire_dma.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
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
gate_reap $P
gate_require_capture "$OUT"

echo "==== captured UART ===="; cat "$OUT"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4WIREDMA-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "ready=CAFE0001"
# dmairq>0 is this gate's whole point -- the CM4 took the eDMA_LPSR completion
# IRQ on its own NVIC.  The zero-check alone could not carry that: `grep -q
# "^dmairq=00000000" && fail` scores a MISSING dmairq as "not zero, therefore the
# IRQ fired", so the assertion passed most confidently in the one case where it
# knew nothing.  Not hypothetical here -- the RED scaffold described in the
# header TIMES THIS TOKEN OUT (the CM4 emits only READY), which is exactly that
# case.  Require the token PRESENT, then require it non-zero.
grep -q "^dmairq=" "$OUT" || { echo "FAIL: dmairq not reported -- the CM4 eDMA_LPSR IRQ assertion cannot be checked"; fail=1; }
grep -q "^dmairq=00000000" "$OUT" && { echo "FAIL: dmairq is 0 (no CM4 eDMA_LPSR IRQ)"; fail=1; }
check "err=00000000"                 # DMA/transaction OK (no NDF/ALF/FEF)
check "done=00000001"
# croot= is printed for HW diagnosis but intentionally NOT asserted. rdv= is
# also printed and not asserted -- see the header: it reads 0x6243 in both
# worlds now, and asserting it is an available strengthening, not a split.
check "WIRE_DMA_CM4=PASS"
check "CM4WIREDMA-DONE"

if [ $fail -eq 0 ]; then
    echo "PASS: CM4 eDMA_LPSR DMA-Wire verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
