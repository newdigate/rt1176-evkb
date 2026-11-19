#!/bin/bash
# audio_player_test gate: AudioPlayer (Audio/play_buffer.*) at unity, half
# rate, reverse, across a rate ramp, through a scripted scratch, starved by
# setAvailableFrames, and polled from the main loop.  The same checks run on
# the board (transcript_hw_nosh.txt); the engine's arithmetic is covered on the
# host by Audio/test/play_buffer_core_test.cpp.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/audio_player_test.elf"
OUT=$(gate_capture_path "$DIR" audio_player.uart)
DBG=$(gate_capture_path "$DIR" audio_player.dbg)
rm -f "$OUT" "$DBG"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
for _ in $(seq 1 240); do
    [ -f "$OUT" ] && grep -q "AUDIO_PLAYER_ALL=" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
for s in UNITY HALF REVERSE RAMP SCRATCH STARVE POLL; do
    grep -q "STAGE_$s=PASS" "$OUT" || { echo "FAIL: $s"; exit 1; }
done
grep -q "AUDIO_PLAYER_ALL=PASS" "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: AUDIO_PLAYER_ALL"
