#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location (a
# hardcoded path silently loads a different tree's gate-lib.sh from a worktree).
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/vglite_conformance.elf"
# Run artifacts go through gate_capture_path, never "$DIR/<name>" -- see its
# comment in gate-lib.sh.
OUT=$(gate_capture_path "$DIR" vglite_conformance.uart)
DBG=$(gate_capture_path "$DIR" vglite_conformance.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# 10s. This example brings up NO PANEL and no LVGL, so it has none of
# vglite_probe's 12s RK055 margin to allow for: every token this gate reads is
# printed inside setup(), ~1s in on an idle machine, and the heartbeat that
# follows is not asserted. 10s is ~10x that, which is the headroom for sweep
# load. It stays well under qrun's 60s QRUN_TIMEOUT, so the reap below -- not
# the wrapper's kill -- is what ends the run.
sleep 10; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

# ★ WHAT THIS GATE PROVES, AND WHAT IT DOES NOT.
#
# QEMU has no GC355 model, so this asserts the HONEST NEGATIVE: the firmware
# ASKS whether a GPU is present, is told no, and reports every case as `skip` --
# cleanly, without hanging, and WITHOUT CLAIMING ANY CASE PASSED. It says
# NOTHING about whether the GC355 renders any of these cases correctly. That
# answer is silicon-only: it lives in transcript_hw_evkb.txt and is checked
# against expected_silicon.txt by tools/vglite-conformance-check.sh.
# See docs/KNOWN-BROKEN-GATES.md and the sibling display/vglite_probe gate,
# whose header makes the same distinction for the same reason.
#
# The negative is a real assertion, not a formality. Two regressions make it
# red rather than silently green: a harness that reports a verdict it never
# measured (the tripwires below), and a matrix that shrinks or a case that
# starts and never finishes (the counts below) -- which is how a GPU-side hang
# would present on silicon.

grep -aq "VGC_BEGIN" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
# Value greps are ANCHORED (CR-tolerant \r?$): a partial match is not a match,
# so a non-zero chip ID cannot satisfy the zero one.
grep -aqE "vgc_chip_id=0x00000000\r?$" "$OUT" || \
    { echo "FAIL: chip-ID probe missing or non-zero under QEMU"; exit 1; }
grep -aqE "vgc_engine=absent reason=no_chip_id\r?$" "$OUT" || \
    { echo "FAIL: expected vgc_engine=absent reason=no_chip_id under QEMU (no GC355 model)"; exit 1; }

# ★ THE TRIPWIRES THIS GATE EXISTS FOR. With no GPU present, no case may claim
# it passed, no case may claim it measured a broken result, and no vg_lite call
# may claim it succeeded. Without these the gate would go green against a
# harness that reports verdicts for work it never ran -- precisely the vacuous
# pass this tree's gates are written to refuse.
#
# `api2?=success` covers BOTH verdict columns deliberately: a case line carries
# api= and api2=, and a fabricated `api2=success` does not contain the substring
# "api=success", so the narrower grep would let half the surface through.
#
# The `grep && { echo; exit 1; }` idiom is safe under `set -e`: the grep is not
# the LAST command of the AND-list, so errexit is suppressed for it and a
# no-match simply falls through to the next line. (This is why the tree spells
# tripwires this way rather than `if grep ...`.)
#
# Demonstrated RED 2026-08-30: a fabricated
#   `vgc case=path/single-contour-rect api=success pixel=ok detail=fill=6400 repeat=same`
# appended to a real capture and replayed through this gate via qrun's
# REAL_QEMU hook -> "FAIL: TRIPWIRE a case reported ok in QEMU" (rc=1).
# Demonstrated RED 2026-08-30: the same line with pixel=ok -> pixel=broken ->
# "FAIL: TRIPWIRE a case reported broken in QEMU".
# Demonstrated RED 2026-08-30: the same line with `api2=success` only (and
# pixel=skip) -> "FAIL: TRIPWIRE an API call succeeded in QEMU" -- the case
# that the narrower `api=success` grep would have missed.
grep -aq  "pixel=ok"       "$OUT" && { echo "FAIL: TRIPWIRE a case reported ok in QEMU"; exit 1; }
grep -aq  "pixel=broken"   "$OUT" && { echo "FAIL: TRIPWIRE a case reported broken in QEMU"; exit 1; }
grep -aqE "api2?=success"  "$OUT" && { echo "FAIL: TRIPWIRE an API call succeeded in QEMU"; exit 1; }

# ★ THE MATRIX MUST BE NON-EMPTY AND COMPLETE. Every tripwire above is
# satisfied VACUOUSLY by an empty matrix -- the harness skeleton did exactly
# that before the case table landed, and it "passed" all three. So the case
# lines are COUNTED, and the count of case_begin lines must EQUAL it: a case
# that announces itself and never reports is how a GPU-side hang presents, and
# a truncated capture would otherwise read as a smaller matrix that happened
# to behave.
#
# `grep -c` exits 1 on a zero count, which under `set -e` would kill the gate
# before its named failure; `|| true` neutralises that and the printed "0" is
# still the substitution's value. (The empty-string case that would break
# `[ -eq ]` cannot arise: gate_require_capture has already proved the file
# exists and is non-empty.)
#
# `^vgc case=` cannot match a `vgc case_begin=` line: the anchor is followed by
# a literal `=` where the begin lines have `_begin=`.
#
# Demonstrated RED 2026-08-30: one `^vgc case=` line deleted from a real
# capture -> "FAIL: expected 13 case lines, got 12" (rc=1).
# Demonstrated RED 2026-08-30: a 14th `vgc case_begin=` line appended -- the
# hang shape, a case that starts and never reports -> "FAIL: 14 case_begin
# lines but 13 case lines (a case did not finish)".
CASES=$(grep -a -c "^vgc case=" "$OUT" || true)
BEGINS=$(grep -a -c "^vgc case_begin=" "$OUT" || true)
[ "$CASES" -eq 26 ] || { echo "FAIL: expected 26 case lines, got $CASES"; exit 1; }
[ "$BEGINS" -eq "$CASES" ] || \
    { echo "FAIL: $BEGINS case_begin lines but $CASES case lines (a case did not finish)"; exit 1; }

# Every Phase-1 case id must be PRESENT BY NAME and in the skip shape. The
# count alone would stay green if a case were renamed while another appeared in
# its place; this names the drift.
#
# The line is anchored END TO END, with every verdict column pinned to `skip`.
# The detail text is matched as `[^ ]+` rather than spelled out: it is the one
# field a case author writes, and it is Task 7's checker that parses its
# content. What the gate pins here is the CONTRACT the checker depends on --
# non-empty and space-free (print_case_line's sanitise_detail enforces it) --
# so a detail that grew a space would split one field into two and fail here,
# where every field after it would otherwise misalign silently.
for id in path/single-contour-rect path/multi-contour-disjoint \
          path/multi-contour-close-padded \
          path/two-disjoint-bars path/four-nested-rings \
          path/two-contour-ring-nonzero path/two-draws-ring \
          path/evenodd-vs-nonzero path/self-intersecting \
          path/format-s8 path/format-s16 path/format-s32 path/format-fp32 \
          path/format-agreement path/degenerate-zero-area \
          color/solid-word-order color/premultiplied-srcover \
          blend/srcover-arithmetic blend/srcover-double \
          blend/none-honours-alpha \
          grad/legacy-linear grad/ext-linear-static grad/ext-linear-moved \
          grad/ext-linear-reupdate grad/ext-linear-rebuilt \
          grad/ramp-word-order; do
    grep -aqE "^vgc case=$id api=skip api2=skip pixel=skip detail=[^ ]+ repeat=skip\r?$" "$OUT" || \
        { echo "FAIL: missing/wrong case line for $id"; exit 1; }
done

# The summary must AGREE with the case lines it summarises, and must record
# which build produced the matrix (dangerous=off is the default build). A
# summary computed from different counters than the ones printed above is a
# real defect class, and only comparing the two can see it.
# Demonstrated RED 2026-08-30: skip=13 edited to skip=12 in a real capture ->
# "FAIL: summary line missing or disagrees with the case lines".
grep -aqE "^vgc_summary engine=absent cases=26 ok=0 broken=0 skip=26 dangerous=off repeat_differs=0\r?$" "$OUT" || \
    { echo "FAIL: summary line missing or disagrees with the case lines"; exit 1; }

# A bounded wait that gave up means the completion path is wrong even when the
# outcome looks right, and with no GPU present no GC355 IRQ can legitimately
# fire. Under QEMU neither counter should ever move.
grep -aqE "vgc_timeouts=0 vgc_irqs=0\r?$" "$OUT" || \
    { echo "FAIL: a bounded wait timed out or an IRQ fired with no GPU"; exit 1; }
grep -aq "VGC_DONE" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
echo "PASS: VGLite conformance harness negative verified"
