#!/bin/sh
# Host unit tests for the conformance probe's instrument code. Runs on the
# development machine's own cc/c++ -- no toolchain, no board, no QEMU.
#
# Two suites, both PASS:/FAIL:-per-check with a count at the end (the
# convention CLAUDE.md treats as authoritative: `grep -c "^PASS:"` on a live
# run is the case count):
#   predicates_test  the pure pixel predicates       (C -- vgc_predicates.h is pure C)
#   arena_test       the shared path arena           (C++ -- see its header)
#
# The arena suite compiles vgc_arena.cpp against tests/stub/vg_lite.h, a
# minimal stand-in for the driver header. That stub dir is on the include path
# HERE ONLY; the target build never sees it.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/vgc-tests.XXXXXX")
trap 'rm -rf "$OUT"' EXIT INT TERM HUP

cc -std=c11 -Wall -Wextra -Werror -O1 -o "$OUT/predicates_test" "$DIR/predicates_test.c"
"$OUT/predicates_test"

c++ -std=c++14 -Wall -Wextra -Werror -O1 -I "$DIR/stub" \
    -o "$OUT/arena_test" "$DIR/arena_test.cpp" "$DIR/../vgc_arena.cpp"
"$OUT/arena_test"
