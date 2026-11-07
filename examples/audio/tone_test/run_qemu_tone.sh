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
ELF="$DIR/$(gate_build_dir)/tone_test.elf"
OUT=$(gate_capture_path "$DIR" tone.uart)
DBG=$(gate_capture_path "$DIR" tone.dbg)
rm -f "$OUT" "$DBG"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -icount shift=auto \
    -display none $(gate_console "$OUT") -d guest_errors -D "$DBG" &
P=$!; gate_pid $P; sleep 20; gate_reap $P   # -icount: deterministic delay()/PIT coupling
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "TONE=PASS" "$OUT" || { echo "FAIL: tone"; exit 1; }
echo "PASS: tone verified"
