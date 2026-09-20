#!/bin/sh
# Host test for acid_box's ACIDBOX_LOOPSTAT percentile helper (NEW-33). No
# toolchain, no board, no QEMU: a p95 index is exactly where an off-by-one
# hides, and nothing on the bench or in the QEMU gate can see it.
# Demonstrated RED (2026-09-04): flooring the rank ((p*n)/100 instead of
# ceil) fails "n=4 p95" and "n=10 p95" by name -- the 3rd/9th smallest
# instead of the 4th/10th.  22 checks.
#
# ...and for keyboard_map.h (spec 2026-09-20): the top-C boundary, the
# keep-or-snap view rule and the key rects, plus the gate's tap landings pinned
# to those rects.  Demonstrated RED (2026-09-20), each by name: top C dropped
# (d <= 11) fails "note36 in view 1 is the TOP C", "C4 in view 3 is the top C"
# and "top C keeps view 1" while every geometry check stays green; the snap
# ignoring an already-visible note fails "top C keeps view 1" (got 2); the
# clamp off by one fails "C4 snaps view 1 -> 3 (clamped)" and "note127 clamps
# high to view 3".  ★ The second mutant must be `(void)view;` in place of the
# early return -- DELETING the line leaves `view` unused, -Werror stops the
# build, and a mutant that does not compile has demonstrated nothing.  40 checks.
set -e
DIR=$(cd "$(dirname "$0")" && pwd); OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
CC=${CC:-cc}
$CC -std=c99 -Wall -Wextra -Werror -I"$DIR/.." "$DIR/loopstat_pct_test.c" -o "$OUT/loopstat_pct_test"
"$OUT/loopstat_pct_test"
$CC -std=c99 -Wall -Wextra -Werror -I"$DIR/.." "$DIR/keyboard_map_test.c" -o "$OUT/keyboard_map_test"
"$OUT/keyboard_map_test"
echo "ACIDBOX-HOST-TESTS: PASS"
