#!/bin/sh
# Host tests for bt_sink_test's own code (NEW-41).  Two binaries, both of which must pass:
#
#   servo_test  -- the clock servo (servo.h), plain C precisely so it can be driven here against a closed-loop
#                  model.  QEMU has no PLL and no real audio clock, so nothing in a gate can measure
#                  convergence, offset or burst rejection.
#   node_test   -- AudioInputBluetooth's RTP/SBC receive path, compiled from the REAL node source and the REAL
#                  M2Radio bt/SbcDecoder + bt/Sbc against the AudioStream shim in tests/shim/.  The gate drives
#                  the happy path with a real fake source; this drives the half no gate reaches -- malformed
#                  packets, ring overrun, fragment reassembly, SUSPEND, and the 4-block refusal.
#
# Run: ./tests/run.sh    (BT_SINK_TEST_OUT names the build DIRECTORY; default $TMPDIR, else /tmp)
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${BT_SINK_TEST_OUT:-${TMPDIR:-/tmp}/bt_sink_test_host}
mkdir -p "$OUT"
CC=${CC:-cc}
CXX=${CXX:-c++}

# M2Radio is a SIBLING checkout, resolved exactly as evkb.cmake resolves it: $TEENSY_LIB_ROOT, else ~/Development.
LIBROOT=${TEENSY_LIB_ROOT:-$HOME/Development}
BT="$LIBROOT/M2Radio/bt"
[ -f "$BT/SbcDecoder.cpp" ] || { echo "node_test: need M2Radio at $BT (set TEENSY_LIB_ROOT)" >&2; exit 1; }

$CC -std=c99 -Wall -Wextra -Werror -o "$OUT/servo_test" "$DIR/servo_test.c"
"$OUT/servo_test"

# ASan/UBSan when the compiler is clang: case 7 feeds seven hostile RTP headers whose whole claim is that the
# parser makes no read past p+len, and "frames()==0" alone is equally satisfied by a parser that read out of
# bounds and then rejected the packet.  The sanitizers are what turn that check into the claim it is written as.
SAN=
if $CXX --version 2>/dev/null | grep -qi clang >/dev/null 2>&1; then
    SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
fi
$CXX -std=c++11 -Wall -Wextra -Werror $SAN \
    -I"$DIR/shim" -I"$DIR/.." -I"$BT" \
    "$DIR/node_test.cpp" "$DIR/shim/shim.cpp" "$DIR/../AudioInputBluetooth.cpp" \
    "$BT/SbcDecoder.cpp" "$BT/Sbc.cpp" \
    -o "$OUT/node_test"
"$OUT/node_test"

echo "BT-SINK-HOST-TESTS: PASS"
