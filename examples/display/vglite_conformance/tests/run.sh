#!/bin/sh
# Host unit tests for the conformance probe's instrument code. Runs on the
# development machine's own cc/c++ -- no toolchain, no board, no QEMU.
#
# Three suites, all PASS:/FAIL:-per-check with a count at the end (the
# convention CLAUDE.md treats as authoritative: `grep -c "^PASS:"` on a live
# run is the case count):
#   predicates_test       the pure pixel predicates      (C -- vgc_predicates.h is pure C)
#   arena_test            the shared path arena          (C++ -- see its header)
#   cases_path_geom_test  the twelve path cases' geometry, sample points,
#                         tolerances and predicates, against a model of a
#                         correct GPU AND a model of this GC355's known
#                         one-contour-per-path defect (C++)
#
# ★ WHAT A GREEN RUN HERE DOES NOT SAY: nothing about the real silicon. No GPU
# is involved. cases_path_geom_test in particular calibrates the instrument
# against two MODELS; the silicon's answers live in the example's
# transcript_hw_evkb.txt and expected_silicon.txt. That file's header says so
# at length, and it is worth repeating at the entry point.
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

rc=0
failed_suites=''

note_fail() {
    rc=1
    failed_suites="$failed_suites $1"
}

# --- predicates_test --------------------------------------------------------
if cc -std=c11 -Wall -Wextra -Werror -O1 \
      -o "$OUT/predicates_test" "$DIR/predicates_test.c"; then
    "$OUT/predicates_test" || note_fail predicates_test
else
    echo "BUILD-FAILED: predicates_test"
    note_fail predicates_test
fi

# --- arena_test -------------------------------------------------------------
if c++ -std=c++14 -Wall -Wextra -Werror -O1 -I "$DIR/stub" \
       -o "$OUT/arena_test" "$DIR/arena_test.cpp" "$DIR/../vgc_arena.cpp"; then
    "$OUT/arena_test" || note_fail arena_test
else
    echo "BUILD-FAILED: arena_test"
    note_fail arena_test
fi

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

# --- trailer ----------------------------------------------------------------
echo "=="
if [ "$rc" -ne 0 ]; then
    echo "run.sh: FAILED --$failed_suites"
else
    echo "run.sh: OK (3 suites)"
fi
exit "$rc"
