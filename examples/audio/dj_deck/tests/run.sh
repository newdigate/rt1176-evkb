#!/bin/sh
# Host tests for the dj_deck example: everything that runs without the board.
# No toolchain, no QEMU.  AUDIO_CODECS points at the audio-codecs checkout.
set -e
DIR=$(cd "$(dirname "$0")" && pwd); OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
CXX=${CXX:-c++}
AC=${AUDIO_CODECS:-$HOME/Development/audio-codecs}
FLAGS="-std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I$DIR/.. -I$AC/include -I$AC"
$CXX $FLAGS "$DIR/deck_math_test.cpp" -o "$OUT/deck_math_test"
"$OUT/deck_math_test"
echo "DJ-DECK-HOST-TESTS: PASS"
