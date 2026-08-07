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
ELF="$DIR/$(gate_build_dir)/extmem_test.elf"; OUT="$DIR/extmem.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/extmem.dbg" &
P=$!; gate_pid $P; sleep 6; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "EXTMEM_ALLOC=PASS"    "$OUT" || { echo "FAIL: malloc/IS_EXTMEM";   exit 1; }
grep -q "EXTMEM_CALLOC=PASS"   "$OUT" || { echo "FAIL: calloc-zero";        exit 1; }
grep -q "EXTMEM_REALLOC=PASS"  "$OUT" || { echo "FAIL: realloc-preserve";   exit 1; }
grep -q "EXTMEM_FREE=PASS"     "$OUT" || { echo "FAIL: free/re-malloc";     exit 1; }
grep -q "EXTMEM_FALLBACK=PASS" "$OUT" || { echo "FAIL: oversize fallback";  exit 1; }
grep -q "EXTMEM_TEST=PASS"     "$OUT" || { echo "FAIL: overall";            exit 1; }
echo "PASS: extmem_malloc verified (alloc/calloc/realloc/free in SDRAM + graceful fallback)"
