#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/extmem_test.elf"; OUT="$DIR/extmem.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/extmem.dbg" &
P=$!; gate_pid $P; sleep 6; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "EXTMEM_ALLOC=PASS"    "$OUT" || { echo "FAIL: malloc/IS_EXTMEM";   exit 1; }
grep -q "EXTMEM_CALLOC=PASS"   "$OUT" || { echo "FAIL: calloc-zero";        exit 1; }
grep -q "EXTMEM_REALLOC=PASS"  "$OUT" || { echo "FAIL: realloc-preserve";   exit 1; }
grep -q "EXTMEM_FREE=PASS"     "$OUT" || { echo "FAIL: free/re-malloc";     exit 1; }
grep -q "EXTMEM_FALLBACK=PASS" "$OUT" || { echo "FAIL: oversize fallback";  exit 1; }
grep -q "EXTMEM_TEST=PASS"     "$OUT" || { echo "FAIL: overall";            exit 1; }
echo "PASS: extmem_malloc verified (alloc/calloc/realloc/free in SDRAM + graceful fallback)"
