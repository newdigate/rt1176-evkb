#!/bin/sh
# lvgl_pxp_copy_bench -- five sync-copy arms against LVGL's lv_draw_buf_copy,
# each on its own byte contract (NEW-55; v7's PXP contract kept as arm=pxp_x0).
# Demonstrated RED (NEW-55): the edma arm's lines stripped from the fixture
# and a MISMATCH injected on a pxp_aff line both fail by name (vacuity suite).
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
# 140 cases, each a 3.7 MB fill + copy + checksum twice over: ~35 s in QEMU.
QRUN_TIMEOUT="${QRUN_TIMEOUT:-90}"; export QRUN_TIMEOUT
ELF="$DIR/$(gate_build_dir)/lvgl_pxp_copy_bench.elf"; OUT="$DIR/pxp_copy_bench.uart"
rm -f "$OUT" "$DIR/pxp_copy_bench.dbg"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/pxp_copy_bench.dbg" &
P=$!; gate_pid $P
# Poll for the DONE token; ceiling 75 s (300 x 0.25).
i=0
while [ $i -lt 300 ]; do
    [ -f "$OUT" ] && grep -q "PXP_COPY_BENCH_DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
    i=$((i+1))
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

grep -q "^SCANOUT=1" "$OUT" || { echo "FAIL: panel not scanning out -- the timings would not be the pipeline's"; exit 1; }
grep -q "ALLOC_OK" "$OUT" || { echo "FAIL: extmem alloc"; exit 1; }
grep -q "^PXP_OK" "$OUT" || { echo "FAIL: PXP bring-up"; exit 1; }
grep -q "^EDMA_OK" "$OUT" || { echo "FAIL: eDMA channel not claimed"; exit 1; }
# Every arm must MATCH its contract on every case, by name.  Space-anchored so
# f=565 cannot satisfy f=8888, i=1 cannot alias i=10..14, and src=xff cannot
# satisfy src=mixed.
check() { # <i> <fmt> <arm> <src>
    grep -q "^CASE i=$1 f=$2 arm=$3 src=$4 " "$OUT" && \
        grep "^CASE i=$1 f=$2 arm=$3 src=$4 " "$OUT" | grep -q " MATCH " || \
        { echo "FAIL: case f=$2 arm=$3 src=$4 i=$1 did not match"; exit 1; }
}
for i in $(seq 1 14); do
    for f in 565 8888; do
        check $i $f cpu_clib mixed
        check $i $f pxp_x0   mixed
        check $i $f edma     mixed
    done
    check $i 8888 pxp_aff  xff
    check $i 8888 pxp_aff  mixed
    check $i 8888 pxp_affa xff
    check $i 8888 pxp_affa mixed
done
grep -q "MISMATCH" "$OUT" && { echo "FAIL: at least one case mismatched"; exit 1; }
grep -q " ERR=" "$OUT" && { echo "FAIL: at least one arm errored"; exit 1; }
# The count pin catches a matrix edit that forgot the loops above.
grep -q "^CASES=140$" "$OUT" || { echo "FAIL: case count"; exit 1; }
grep -q "COPY_BENCH_OK" "$OUT" || { echo "FAIL: bench verdict withheld"; exit 1; }
# Timings are NOT asserted anywhere: hardware-only, vacuous in QEMU.
[ -f "$DIR/pxp_copy_bench.dbg" ] || { echo "FAIL: no guest-error log"; exit 1; }
grep -q "guest" "$DIR/pxp_copy_bench.dbg" && { echo "FAIL: guest errors"; exit 1; }
echo "PASS: five sync-copy arms match their contracts across the matrix"
