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
ELF="$DIR/build/interval_timer_test.elf"; OUT="$DIR/it.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/it.dbg" &
P=$!; gate_pid $P; sleep 20; gate_reap $P   # -icount couples delay() (DWT) and the PIT (QEMU_CLOCK_VIRTUAL) so counts are deterministic
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "IT=PASS" "$OUT" || { echo "FAIL: IntervalTimer"; exit 1; }
echo "PASS: IntervalTimer verified"
