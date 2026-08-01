#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init

# v9 P1 dual-depth gate: BOTH ELFs run sequentially -- the default RGB565
# build (build/) and the XRGB8888 build (build-32/, -DDRAW_BENCH_32=ON).
# Correctness is the QEMU claim; the CPU_us/PXP_us numbers are VACUOUS here
# (no timing model) and are NOT asserted -- the hardware table (P2) is the
# timing claim, exactly like every sibling bench.
#
# Poll ceiling 30 s per build (120 x 0.25): measured 3.0-4.0 s wall to
# PXP_DRAW_BENCH_DONE, but the original 6 s (+50%) ceiling produced a real
# 1-in-6 timeout on comp_720x1280 under transient host load (v9 Task-1
# review) -- polls are free on green runs, so the margin is now ~8x, the
# load-tolerance lesson KNOWN-BROKEN-GATES teaches.  QRUN_TIMEOUT=60 is the
# hard per-run backstop.

FILL_NAMES="fill_32x32 fill_120x40 fill_240x160 fill_400x300 fill_720x80 \
            fill_720x1280"
BLIT_NAMES="blit_32x32 blit_120x40 blit_240x160 blit_400x300 blit_720x80 \
            blit_720x1280"
COMP_NAMES="comp_32x32 comp_120x40 comp_240x160 comp_400x300 comp_720x80 \
            comp_720x1280"

# run_build <elf> <uart> <dbg> <depth> <draws> <case names...>
run_build() {
    _elf="$1"; _out="$2"; _dbg="$3"; _depth="$4"; _draws="$5"
    shift 5
    rm -f "$_out" "$_dbg"
    QRUN_TIMEOUT=60 "$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on \
        -kernel "$_elf" -display none -serial file:"$_out" \
        -d guest_errors -D "$_dbg" &
    _p=$!; gate_pid $_p
    i=0
    while [ $i -lt 120 ]; do
        [ -f "$_out" ] && grep -q "PXP_DRAW_BENCH_DONE" "$_out" 2>/dev/null && break
        sleep 0.25
        i=$((i+1))
    done
    gate_reap $_p
    gate_require_capture "$_out" "DEPTH=$_depth build"
    echo "==== captured UART (DEPTH=$_depth) ===="; cat "$_out"

    grep -q "^DEPTH=$_depth$" "$_out" || { echo "FAIL($_depth): wrong or missing DEPTH token"; exit 1; }
    grep -q "^SW_PATH=lvgl$"  "$_out" || { echo "FAIL($_depth): SW comparator is not LVGL's own code"; exit 1; }
    grep -q "PANEL_OK"    "$_out" || { echo "FAIL($_depth): panel bring-up"; exit 1; }
    grep -q "PXP_OK"      "$_out" || { echo "FAIL($_depth): PXP bring-up"; exit 1; }
    grep -q "ALLOC_OK"    "$_out" || { echo "FAIL($_depth): extmem alloc"; exit 1; }
    grep -q "^SW_SMOKE=OK$" "$_out" || { echo "FAIL($_depth): fabricated-task smoke fill"; exit 1; }
    # Every case must be OK, BY NAME -- a dropped case cannot hide behind a
    # passing sweep.  Name-anchored with a trailing space so fill_32x32
    # cannot satisfy a hypothetical fill_32x320.
    for n in "$@"; do
        grep -q "^DRAW n=$n " "$_out" && grep "^DRAW n=$n " "$_out" | grep -q " OK$" \
            || { echo "FAIL($_depth): case $n not OK"; exit 1; }
    done
    grep -q " BAD$" "$_out" && { echo "FAIL($_depth): at least one case BAD"; exit 1; }
    grep -q "PXP_ERR" "$_out" && { echo "FAIL($_depth): a PXP op errored"; exit 1; }
    # The count pin catches a case-table edit that forgot the loop above.
    grep -q "^DRAWS=$_draws$" "$_out" || { echo "FAIL($_depth): DRAWS count != $_draws"; exit 1; }
    grep -q "DRAW_BENCH_OK" "$_out" || { echo "FAIL($_depth): bench verdict withheld"; exit 1; }
    [ -f "$_dbg" ] || { echo "FAIL($_depth): no guest-error log"; exit 1; }
    grep -q "guest" "$_dbg" && { echo "FAIL($_depth): guest errors"; exit 1; }
    # Explicit success: on a healthy run the guest grep above returns 1 (found
    # nothing), and WITHOUT this line that 1 becomes the FUNCTION's return
    # status -- the caller runs under `set -e`, which ignores a failing
    # AND-OR list inline but does NOT ignore a function call returning
    # nonzero, so the gate would die silently right here, green output and
    # exit 1 with no FAIL message.  Observed 2026-08-01 before this line
    # existed.
    return 0
}

# --- build 1: RGB565 (default) -- fills + blits + composites + overhead ----
run_build "$DIR/build/pxp_draw_bench.elf" "$DIR/pxp_draw_565.uart" \
          "$DIR/pxp_draw_565.dbg" 16 19 \
          $FILL_NAMES $BLIT_NAMES $COMP_NAMES overhead_16x2
# The two-engine composite divergence must be REPORTED per case (a measured
# fact; its value is not pinned -- LVGL's rounding != silicon's truncate).
for n in $COMP_NAMES; do
    grep -q "^COMP n=$n ENGINE_DELTA=" "$DIR/pxp_draw_565.uart" \
        || { echo "FAIL(16): ENGINE_DELTA report missing for $n"; exit 1; }
done

# --- build 2: XRGB8888 -- fills + blits + overhead; NO composites ----------
run_build "$DIR/build-32/pxp_draw_bench.elf" "$DIR/pxp_draw_32.uart" \
          "$DIR/pxp_draw_32.dbg" 32 13 \
          $FILL_NAMES $BLIT_NAMES overhead_16x2
# Composites are DECLINED at 32 bpp by construction (spec 1.4: the output X
# byte would carry silicon's underived computed alpha) -- their ABSENCE is
# part of the contract, so assert it.
grep -q "n=comp_" "$DIR/pxp_draw_32.uart" \
    && { echo "FAIL(32): composite cases present in the 32-bpp build"; exit 1; }

echo "PASS: PXP draw bench -- both depths, all cases OK, counts pinned 19/13"
# NEGATIVE-TEST RECORD (red-first, per the two-gate rule), both observed
# 2026-08-01 on the 565 build and hand-reverted before the committed greens:
#  1. composite-oracle sabotage: o_blend_channel's (256u - a) -> (255u - a)
#     in pxp_draw_bench.cpp made ALL SIX comp_* cases BAD; this gate went RED
#     with "FAIL(16): case comp_32x32 not OK" (first of the six).
#  2. fill-expected sabotage: fill_expected_565's red mask 0xF8 -> 0xF0 made
#     every fill case whose colour has red bit 3 set go BAD (fill_120x40,
#     fill_240x160, overhead_16x2 -- the other fill colours are unchanged by
#     that particular mask, honestly reported); this gate went RED with
#     "FAIL(16): case fill_120x40 not OK".
# Both prove the per-path assertions bite: a wrong oracle or a wrong expected
# constant cannot pass.  Greens x2 re-confirmed after each hand-revert.
