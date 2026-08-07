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
ELF="$DIR/$(gate_build_dir)/serial_test.elf"
OUT="$DIR/serial.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/serial.dbg" &
P=$!; gate_pid $P
# Poll for the last token this gate asserts instead of guessing a duration. The
# fixed `sleep 3` this replaces made the gate LOAD-SENSITIVE: on a busy machine
# QEMU had not written a byte before the reap, and the run failed with "no UART
# capture" -- a red that says nothing about the firmware and passes on retry.
# Observed repeatedly during the 2026-07-29 sweep. Same idiom as the dualcore
# gates. 40 x 0.25s bounds it at 10s, but a healthy run breaks out well inside
# the old 3s, so the sweep gets FASTER on average as well as steadier.
#
# count=3 and not a DONE marker: this firmware free-runs its counter and never
# finishes, so the terminal condition IS the last thing asserted below.
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "count=3" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 Serial1 up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "count=3" "$OUT" || { echo "FAIL: counter missing"; exit 1; }
echo "PASS: QEMU serial output verified"
