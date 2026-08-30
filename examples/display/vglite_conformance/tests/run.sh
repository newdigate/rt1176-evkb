#!/bin/sh
# Host unit test for the conformance probe's pixel predicates. Runs on the
# development machine's own cc -- no toolchain, no board, no QEMU.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/vgc-tests.XXXXXX")
trap 'rm -rf "$OUT"' EXIT INT TERM HUP
cc -std=c11 -Wall -Wextra -Werror -O1 -o "$OUT/predicates_test" "$DIR/predicates_test.c"
"$OUT/predicates_test"
