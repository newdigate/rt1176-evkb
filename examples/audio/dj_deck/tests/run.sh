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
PREVIEW="$AC/src/preview/preview_stream.cpp $AC/src/preview/preview_reader.cpp $AC/src/preview/preview_generator.cpp"
$CXX $FLAGS "$DIR/peaks_test.cpp" "$DIR/../peaks.cpp" $PREVIEW -o "$OUT/peaks_test"
"$OUT/peaks_test"
DEC="$AC/src/core/bit_reader.cpp $AC/src/wav/g711.cpp $AC/src/wav/sample_converter.cpp $AC/src/wav/decoder/wav_parser.cpp $AC/src/wav/decoder/wav_decoder.cpp \
$AC/src/aiff/ieee80.cpp $AC/src/aiff/sample_converter.cpp $AC/src/aiff/decoder/aiff_parser.cpp $AC/src/aiff/decoder/aiff_decoder.cpp \
$AC/src/flac/crc.cpp $AC/src/flac/md5.cpp $AC/src/flac/metadata.cpp $AC/src/flac/decoder/rice_decoder.cpp $AC/src/flac/decoder/subframe_decoder.cpp \
$AC/src/flac/decoder/channel_decorrelator.cpp $AC/src/flac/decoder/flac_decoder.cpp $AC/src/flac/encoder/fixed_predictor.cpp \
$AC/src/mp3/mp3_tables.cpp $AC/src/mp3/decoder/bit_reservoir.cpp $AC/src/mp3/decoder/huffman_decoder.cpp $AC/src/mp3/decoder/requantizer.cpp \
$AC/src/mp3/decoder/imdct.cpp $AC/src/mp3/decoder/synthesis_filter.cpp $AC/src/mp3/decoder/mp3_decoder.cpp"
CLIPS="$DIR/../../audio_codecs_probe/clips"
# the decoders are built once, optimised and without the sanitizers' -Werror noise: they have their own tests
for f in $DEC; do $CXX -std=c++17 -O1 -g -I$AC/include -I$AC -c "$f" -o "$OUT/$(echo "$f" | tr '/' '_').o"; done
$CXX $FLAGS -I"$DIR/../../audio_codecs_probe" "$DIR/track_source_test.cpp" "$DIR/../track_source.cpp" $AC/src/preview/preview_stream.cpp "$OUT"/*.o -o "$OUT/track_source_test"
"$OUT/track_source_test" "$CLIPS"
echo "DJ-DECK-HOST-TESTS: PASS"
