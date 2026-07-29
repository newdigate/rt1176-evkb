#!/bin/sh
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
ELF="$DIR/build/irq_attach_test.elf"; OUT="$DIR/irq.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/irq.dbg" &
P=$!; gate_pid $P
# Poll for the run's verdict token instead of guessing a duration. The fixed
# `sleep 3` this replaces made the gate LOAD-SENSITIVE: on a busy machine QEMU
# had not written a byte before the reap, and the run failed with "no UART
# capture" -- a red that says nothing about the firmware and passes on retry.
# Observed repeatedly during the 2026-07-29 sweep. Same idiom as the dualcore
# gates. 40 x 0.25s bounds it at 10s; a healthy run breaks out well inside 3s.
#
# IRQ=PASS is the firmware's last line and the one thing asserted below, so
# waiting for it is exactly waiting for the run to finish.
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "IRQ=PASS" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "IRQ=PASS" "$OUT" || { echo "FAIL: attachInterrupt"; exit 1; }
echo "PASS: attachInterrupt verified (RISING/CHANGE/detach)"
