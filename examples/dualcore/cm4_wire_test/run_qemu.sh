#!/bin/sh
# QEMU gate for Phase-3.2: the CM4 self-configures LPI2C5 and runs polled I2C
# transactions against the wm8962-stub; the CM7 reports over the MU on LPUART1.
# NOTE: the LPI2C model + stub respond on MCR.MEN alone (clock/pins ignored),
# so QEMU proves the register/transfer SEQUENCE only — the wiring-free HW run
# proves the CM4's clock-gating + LPSR pin-mux (see README / spec).
# rdv WAS world-split, and no longer is (2026-08-15). It used to assert
# rdv=00000000 here and rdv=00006243 on HW, because the QEMU side was a stub
# whose registers all read back 0. That stub was replaced by a real WM8962
# control-interface model (qemu2 23a86da9c3, hw/audio/wm8962.c), which returns
# the true 0x6243 device ID for R15 -- so both worlds now report the same value
# and the split has collapsed. Asserting it is STRONGER than what it replaced:
# rdv=00000000 was also what a read that moved no data would leave behind, since
# the firmware zero-initialises rdv, whereas 0x6243 can only come off the wire.
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
ELF="$DIR/$(gate_build_dir)/cm4_wire_test.elf"
OUT="$DIR/cm4_wire.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_wire.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4WIRE-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"

echo "==== captured UART ===="; cat "$OUT"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4WIRE-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "mcr=00000001"      # LPI2C MCR.MEN set (master enabled)
check "ack=00000000"      # WM8962 reset-write ACKed (err 0)
check "nack=00000002"     # absent addr 0x2A -> address NACK (err 2)
check "rdn=00000002"      # ID read-back returned 2 bytes
check "rdv=00006243"      # WM8962 device ID off the wire -- same on HW
check "done=00000001"     # CM4 sequence completed
check "WIRE_CM4=PASS"     # verdict
# lpcg= / croot= are printed for HW diagnosis but intentionally NOT asserted.
grep -q "CM4WIRE-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: CM4 self-configured polled I2C verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
