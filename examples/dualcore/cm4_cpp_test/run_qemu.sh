#!/bin/sh
# QEMU gate for cm4_cpp_test: the first C++ CM4 image (g++ path in
# teensy_add_cm4_image). Pure-CPU proof — static ctors via the image's
# .init_array walk, virtual dispatch, freestanding -nostdlib stubs — so a
# green QEMU run is meaningful; the EVKB run confirms on silicon.
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
ELF="$DIR/$(gate_build_dir)/cm4_cpp_test.elf"
OUT="$DIR/cm4_cpp.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_cpp.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4CPP-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"

echo "==== captured ===="; cat "$OUT"
grep -q "CM4CPP-DONE" "$OUT" || { echo "FAIL: no done"; exit 1; }
grep -q "ctor=CAFEC201" "$OUT" || { echo "FAIL: static ctor did not run"; exit 1; }
grep -q "virt=CAFEC202" "$OUT" || { echo "FAIL: virtual dispatch broken"; exit 1; }
grep -q "done=D0DE0002" "$OUT" || { echo "FAIL: wrong done marker"; exit 1; }
grep -q "CPP_CM4=PASS" "$OUT" || { echo "FAIL: no CPP_CM4=PASS"; exit 1; }
echo "PASS: CPP_CM4"
