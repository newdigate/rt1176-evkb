#!/bin/sh
# Host test for acid_box's ACIDBOX_LOOPSTAT percentile helper (NEW-33). No
# toolchain, no board, no QEMU: a p95 index is exactly where an off-by-one
# hides, and nothing on the bench or in the QEMU gate can see it.
# Demonstrated RED (2026-09-04): flooring the rank ((p*n)/100 instead of
# ceil) fails "n=4 p95" and "n=10 p95" by name -- the 3rd/9th smallest
# instead of the 4th/10th.  22 checks.
set -e
DIR=$(cd "$(dirname "$0")" && pwd); OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
CC=${CC:-cc}
$CC -std=c99 -Wall -Wextra -Werror -I"$DIR/.." "$DIR/loopstat_pct_test.c" -o "$OUT/loopstat_pct_test"
"$OUT/loopstat_pct_test"
echo "ACIDBOX-HOST-TESTS: PASS"
