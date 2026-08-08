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
ELF="$DIR/$(gate_build_dir)/stream_test.elf"; OUT="$DIR/stream.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -icount shift=auto \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/stream.dbg" &
P=$!; gate_pid $P; sleep 20; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "STREAM_ALL=PASS" "$OUT" || { echo "FAIL: stream"; exit 1; }
grep -q "GATE=DONE" "$OUT" || { echo "FAIL: completion"; exit 1; }
echo "PASS: stream gate"
