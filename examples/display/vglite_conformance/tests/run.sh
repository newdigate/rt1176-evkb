#!/bin/sh
# Host unit tests for the conformance probe's instrument code. Runs on the
# development machine's own cc/c++ -- no toolchain, no board, no QEMU.
#
# Five suites, all PASS:/FAIL:-per-check with a count at the end (the
# convention CLAUDE.md treats as authoritative: `grep -c "^PASS:"` on a live
# run is the case count):
#   predicates_test       the pure pixel predicates      (C -- vgc_predicates.h is pure C)
#   color_test            the pure colour predicates     (C -- vgc_color.h is pure C)
#   arena_test            the shared path arena          (C++ -- see its header)
#   cases_path_geom_test  the fifteen path cases' geometry, sample points,
#                         tolerances and predicates, against a model of a
#                         correct GPU AND a model of this GC355's known
#                         one-contour-per-path defect (C++)
#   cases_color_test      the five colour/blend cases' sample point, tolerances,
#                         reading bands and alpha checks, against SEVEN models:
#                         correct / draws-nothing / double-premultiplying /
#                         R-B-permuting / alpha-ignoring / reading-A /
#                         BLEND_NONE-modulating (C++)
#
# ★ WHAT A GREEN RUN HERE DOES NOT SAY: nothing about the real silicon. No GPU
# is involved. cases_path_geom_test calibrates the instrument against FOUR
# MODELS (correct / first-contour-only / draws-nothing / stray-ink) and
# cases_color_test against SEVEN; the silicon's answers live in the example's
# transcript_hw_evkb.txt and expected_silicon.txt. Those files' headers say so
# at length, and it is worth repeating at the entry point.
#
# ★ cases_color_test's models are models of a BLEND, and model.h implements
# reading B of SRC_OVER BECAUSE THE HARDWARE WAS MEASURED DOING READING B (two
# boots, 2026-09-02, in the example's transcript_hw_evkb.txt). So arm 1 is a
# model of that measurement, never a second vote for it -- and arms 6 and 7 are
# models of the OTHER admissible readings, which the colour cases must now
# report BROKEN. A green colour suite still says nothing about any GPU.
#
# ★ WHY THIS SUITE EXISTS AT ALL: the QEMU gate cannot reach the pixel logic.
# QEMU has no GC355, so every case there reports pixel=skip -- a green gate
# says nothing about a sample point, a tolerance or a predicate. Between the
# gate and the one silicon boot, these tests are the only check on any of it.
#
# The C++ suites compile against tests/stub/vg_lite.h, a minimal stand-in for
# the driver header. That stub dir is on the include path HERE ONLY; the
# target build never sees it.
#
# ★ EVERY SUITE RUNS AND IS REPORTED, EVEN AFTER ONE FAILS -- deliberately not
# `set -e`. A red suite that aborts the script hides the state of the ones
# after it, and "the arena is fine" is exactly the thing you want to know while
# reading a predicates failure. The exit status is non-zero if ANY suite fails
# to build or fails a check.
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/vgc-tests.XXXXXX")
trap 'rm -rf "$OUT"' EXIT INT TERM HUP

# ★ DERIVED, not written down. The trailer said "3 suites" as a literal, so a
# fourth suite would have made it print a lie -- and a count is the one thing in
# a test trailer a reader takes on trust.
rc=0
nsuites=0
failed_suites=''

suite() { nsuites=$((nsuites + 1)); }

note_fail() {
    rc=1
    failed_suites="$failed_suites $1"
}

suite
# --- predicates_test --------------------------------------------------------
if cc -std=c11 -Wall -Wextra -Werror -O1 \
      -o "$OUT/predicates_test" "$DIR/predicates_test.c"; then
    "$OUT/predicates_test" || note_fail predicates_test
else
    echo "BUILD-FAILED: predicates_test"
    note_fail predicates_test
fi

suite
# --- color_test -------------------------------------------------------------
# Pure C over vgc_color.h alone, exactly as predicates_test is pure over
# vgc_predicates.h -- deliberately NOT linked against vgc_harness.h, which
# pulls in vg_lite.h and would force this suite to C++ plus the stub. The
# VGC_ABGR_A/VGC_ABGR identity that lives in the harness will be pinned by the
# colour case-geometry suite instead, which includes the harness anyway.
if cc -std=c11 -Wall -Wextra -Werror -O1 \
      -o "$OUT/color_test" "$DIR/color_test.c"; then
    "$OUT/color_test" || note_fail color_test
else
    echo "BUILD-FAILED: color_test"
    note_fail color_test
fi

suite
# --- arena_test -------------------------------------------------------------
if c++ -std=c++14 -Wall -Wextra -Werror -O1 -I "$DIR/stub" \
       -o "$OUT/arena_test" "$DIR/arena_test.cpp" "$DIR/../vgc_arena.cpp"; then
    "$OUT/arena_test" || note_fail arena_test
else
    echo "BUILD-FAILED: arena_test"
    note_fail arena_test
fi

suite
# --- cases_path_geom_test ---------------------------------------------------
# Links the REAL case table (vgc_cases_path.cpp) and the REAL arena against the
# suite's own reference rasteriser. -I "$DIR/.." so the case file's
# "vgc_harness.h" resolves the same way it does in the target build.
if c++ -std=c++14 -Wall -Wextra -Werror -O1 -I "$DIR/stub" -I "$DIR/.." \
       -o "$OUT/cases_path_geom_test" \
       "$DIR/cases_path_geom_test.cpp" \
       "$DIR/../vgc_cases_path.cpp" "$DIR/../vgc_arena.cpp"; then
    "$OUT/cases_path_geom_test" || note_fail cases_path_geom_test
else
    echo "BUILD-FAILED: cases_path_geom_test"
    note_fail cases_path_geom_test
fi

suite
# --- cases_color_test -------------------------------------------------------
# Same shape as cases_path_geom_test one suite up: the REAL colour case table
# (vgc_cases_color.cpp) and the REAL arena, linked against the shared model and
# the shared case-lifecycle mirror.
if c++ -std=c++14 -Wall -Wextra -Werror -O1 -I "$DIR/stub" -I "$DIR/.." \
       -o "$OUT/cases_color_test" \
       "$DIR/cases_color_test.cpp" \
       "$DIR/../vgc_cases_color.cpp" "$DIR/../vgc_arena.cpp"; then
    "$OUT/cases_color_test" || note_fail cases_color_test
else
    echo "BUILD-FAILED: cases_color_test"
    note_fail cases_color_test
fi

suite
# --- cases_grad_test --------------------------------------------------------
# The REAL gradient case table against the shared model, which for this suite
# also models the DRIVER's gradient entry points from their source -- see the
# GRADIENTS half of model.h. Six arms: correct / draws-nothing / draws-black /
# paint-follows-path / solid-first-stop / R-B-permuting-ramp-store.
if c++ -std=c++14 -Wall -Wextra -Werror -O1 -I "$DIR/stub" -I "$DIR/.." \
       -o "$OUT/cases_grad_test" \
       "$DIR/cases_grad_test.cpp" \
       "$DIR/../vgc_cases_grad.cpp" "$DIR/../vgc_arena.cpp"; then
    "$OUT/cases_grad_test" || note_fail cases_grad_test
else
    echo "BUILD-FAILED: cases_grad_test"
    note_fail cases_grad_test
fi

suite
# --- cases_blit_test --------------------------------------------------------
# The REAL Phase 3 case table against the shared model, which for this suite
# models the driver's scissor bookkeeping (two mechanisms, applied per regime)
# and its blit source-stride check from source. Six arms.
if c++ -std=c++14 -Wall -Wextra -Werror -O1 -I "$DIR/stub" -I "$DIR/.." \
       -o "$OUT/cases_blit_test" \
       "$DIR/cases_blit_test.cpp" \
       "$DIR/../vgc_cases_blit.cpp" "$DIR/../vgc_arena.cpp"; then
    "$OUT/cases_blit_test" || note_fail cases_blit_test
else
    echo "BUILD-FAILED: cases_blit_test"
    note_fail cases_blit_test
fi

# --- trailer ----------------------------------------------------------------
echo "=="
if [ "$rc" -ne 0 ]; then
    echo "run.sh: FAILED --$failed_suites"
else
    echo "run.sh: OK ($nsuites suites)"
fi
exit "$rc"
