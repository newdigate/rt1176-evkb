#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/pxp_composite_test.elf"; OUT="$DIR/pxp_composite.uart"
rm -f "$OUT" "$DIR/pxp_composite.dbg"
# A full run measures 31 s wall to PXP_COMPOSITE_DONE (the four 6 s ritual
# holds + the fade dominate), so the qrun ceiling is raised above its 60 s
# default and the poll ceiling is 50 s (200 x 0.25 -- measured + ~60% margin).
QRUN_TIMEOUT=90 "$QEMU" $(gate_qemu_machine) \
    -kernel "$ELF" -display none -serial file:"$OUT" \
    -d guest_errors -D "$DIR/pxp_composite.dbg" &
P=$!; gate_pid $P
i=0
while [ $i -lt 200 ]; do
    [ -f "$OUT" ] && grep -q "PXP_COMPOSITE_DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
    i=$((i+1))
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

grep -q "PANEL_OK" "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "PXP_OK"   "$OUT" || { echo "FAIL: PXP bring-up"; exit 1; }
grep -q "ALLOC_OK" "$OUT" || { echo "FAIL: extmem alloc"; exit 1; }
# Every case must MATCH, by name -- a dropped case cannot hide behind a
# passing sweep.  Name-anchored with a trailing space so rop_not cannot
# satisfy rop_notxoras (nor vice versa).
for n in argb_embedded argb_override_80 argb_override_00 argb_multiply_80 \
         argb_invert_embedded argb_askey argb_pskey argb_bothkeys \
         rgba_embedded rgba_askey rgb565_override_80 rgb565_askey \
         rgb565_pskey rgb888x_override_80 rgb565_embedded_opaque \
         rop_maskas rop_masknotas rop_maskasnot rop_mergeas rop_mergenotas \
         rop_mergeasnot rop_notcopyas rop_not rop_notmaskas rop_notmergeas \
         rop_xoras rop_notxoras argb_embedded_offs13_7 argb_embedded_edge_br \
         override_center gradient_embedded; do
    grep -q "^CASE n=$n " "$OUT" && grep "^CASE n=$n " "$OUT" | grep -q " MATCH$" \
        || { echo "FAIL: case $n did not match"; exit 1; }
done
grep -q "MISMATCH" "$OUT" && { echo "FAIL: at least one case mismatched"; exit 1; }
# The count pin catches a case-table edit that forgot the loop above.
grep -q "^CASES=31$" "$OUT" || { echo "FAIL: case count"; exit 1; }
grep -q "COMPOSITE_OK" "$OUT" || { echo "FAIL: composite verdict withheld"; exit 1; }
# ALPHA_OUT is a PRESENCE check only: silicon writes a computed alpha whose
# formula is deliberately unmodelled (v8 transcript RESOLVED AMBIGUITIES
# item 5 -- a stated asymmetry, like the UNDERRUN vacuity; UNDERRUNS is NOT
# asserted here for the same reason).
grep -q "^ALPHA_OUT dst " "$OUT" || { echo "FAIL: ALPHA_OUT dump missing"; exit 1; }
[ -f "$DIR/pxp_composite.dbg" ] || { echo "FAIL: no guest-error log"; exit 1; }
grep -q "guest" "$DIR/pxp_composite.dbg" && { echo "FAIL: guest errors"; exit 1; }
echo "PASS: PXP compositing == the silicon-measured oracle across all 31 cases"
# NEGATIVE-TEST RECORD (red-first, per the two-gate rule): sabotaging the
# oracle's blend divisor (256 -> /255-rounded) makes this gate FAIL naming
# the blend-path cases (argb_embedded first of 15) -- observed at Task 4 and
# reproduced independently by the v8 final review.  The gradient case alone
# covers all 256 alpha values, so any wrong blend formula fails immediately.
