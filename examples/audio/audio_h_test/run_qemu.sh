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
ELF="$DIR/$(gate_build_dir)/audio_h_test.elf"; OUT="$DIR/audio_h.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/audio_h.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do [ -f "$OUT" ] && grep -q "AUDIOH-DONE" "$OUT" 2>/dev/null && break; sleep 0.25; done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "AUDIOH-GATE v1"  "$OUT" || { echo "FAIL: banner"; exit 1; }
grep -q "AUDIOH_CHAIN=PASS" "$OUT" || { echo "FAIL: chain"; exit 1; }
echo "PASS: AUDIOH_CHAIN"
