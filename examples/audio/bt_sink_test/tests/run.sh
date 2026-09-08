#!/bin/sh
# Host tests for bt_sink_test's pure logic (NEW-41).  The clock servo is plain C precisely so it can be driven
# here against a closed-loop model -- QEMU has no PLL and no real audio clock, so nothing in a gate can measure
# convergence, offset or burst rejection.  Run: ./tests/run.sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${BT_SINK_TEST_OUT:-/private/tmp/claude-501/-Users-nicholasnewdigate-Development-rt1170-evkb/8e94b9bd-b541-41da-87f4-0f7c76be8fca/scratchpad/servo_test}
mkdir -p "$(dirname "$OUT")"
cc -std=c99 -Wall -Wextra -Werror -o "$OUT" "$DIR/servo_test.c"
"$OUT"
echo "BT-SINK-HOST-TESTS: PASS"
