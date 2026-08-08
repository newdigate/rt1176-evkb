#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/lvgl_pxp_copy_bench.elf"; OUT="$DIR/pxp_copy_bench.uart"
rm -f "$OUT" "$DIR/pxp_copy_bench.dbg"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/pxp_copy_bench.dbg" &
P=$!; gate_pid $P
# Poll for the DONE token rather than burning a fixed window; ceiling 20 s
# (80 x 0.25).
i=0
while [ $i -lt 80 ]; do
    [ -f "$OUT" ] && grep -q "PXP_COPY_BENCH_DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
    i=$((i+1))
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

grep -q "ALLOC_OK" "$OUT" || { echo "FAIL: extmem alloc"; exit 1; }
grep -q "PXP_OK"   "$OUT" || { echo "FAIL: PXP bring-up"; exit 1; }
# Every case must MATCH, by name, in BOTH formats -- a dropped case or a
# dropped format cannot hide behind a count.  Space-anchored so f=565 cannot
# satisfy f=8888 and i=1 cannot alias i=10..14.
for f in 565 8888; do
  for i in $(seq 1 14); do
    grep -q "^CASE i=$i f=$f " "$OUT" && grep "^CASE i=$i f=$f " "$OUT" | grep -q " MATCH " || { echo "FAIL: case f=$f i=$i did not match"; exit 1; }
  done
done
grep -q "MISMATCH" "$OUT" && { echo "FAIL: at least one case mismatched"; exit 1; }
# The count pin catches a matrix edit that forgot the loops above.
grep -q "^CASES=28$" "$OUT" || { echo "FAIL: case count"; exit 1; }
grep -q "COPY_BENCH_OK" "$OUT" || { echo "FAIL: bench verdict withheld"; exit 1; }
# Timings are NOT asserted anywhere: hardware-only, vacuous in QEMU -- the
# NOTE token in the transcript says so and this comment is the gate's half.
[ -f "$DIR/pxp_copy_bench.dbg" ] || { echo "FAIL: no guest-error log"; exit 1; }
grep -q "guest" "$DIR/pxp_copy_bench.dbg" && { echo "FAIL: guest errors"; exit 1; }
echo "PASS: PXP copy == CPU copy across the matrix"
