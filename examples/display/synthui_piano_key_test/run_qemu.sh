#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
export REAL_QEMU="${REAL_QEMU:-/Users/moolet/Development/qemu-rt1170/build/qemu-system-arm}"
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/synthui_piano_key_test.elf"
OUT=$(gate_capture_path "$DIR" synthui_piano_key.uart)
DBG=$(gate_capture_path "$DIR" synthui_piano_key.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
sleep 20; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "PANEL_OK"          "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "piano_key_scene=keyboard whites=14 blacks=10 states=6" "$OUT" || { echo "FAIL: scene line missing"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
grep -qE "LVGL_BYTES=3686400\r?$" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }

# GOLDEN CHECKSUM -- FNV-1a over the whole 720x1280 PRESENTED buffer, the
# 2-octave keyboard scene (14 white keys, 10 black keys, 6 state demo cards,
# Play/Stop buttons, SevenSegment readout).
# Recorded across consecutive QEMU runs (2026-09-07, NEW-28, SynthUI piano_key,
# vendored LVGL 9.4.0, XRGB8888).
# Demonstrated RED 2026-09-07: CRC altered to 0xDEADBEEF -> "FAIL: piano_key checksum".
grep -qE "piano_key_crc=0x73288DB0\r?$" "$OUT" || { echo "FAIL: piano_key checksum"; exit 1; }

# DELTA EQUALITY: 64-step sequence delta CRC must be PIXEL-IDENTICAL to fresh render
DSEQ=$(grep -a -oE "piano_key_delta_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
DFUL=$(grep -a -oE "piano_key_fresh_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DSEQ" ] && [ -n "$DFUL" ] || { echo "FAIL: delta guard tokens missing"; exit 1; }
[ "$DSEQ" = "$DFUL" ] || { echo "FAIL: delta render differs from full render ($DSEQ vs $DFUL)"; exit 1; }
grep -qE "piano_key_delta_eq=PASS\r?$" "$OUT" || { echo "FAIL: delta equality"; exit 1; }

# ENGAGEMENT: largest single invalidated area must stay small (bound 15000 px)
DAREA=$(grep -a -oE "piano_key_damage max=[0-9]+" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DAREA" ] && [ "$DAREA" -gt 0 ] || { echo "FAIL: delta damage guard missing or zero"; exit 1; }
[ "$DAREA" -le 15000 ] || { echo "FAIL: delta damage not engaged (max=$DAREA)"; exit 1; }

# VSYNC health
grep -qE "piano_key_vsync flips=[0-9]+ isrs=[0-9]+ timeouts=0\r?$" "$OUT" || { echo "FAIL: vsync fence unhealthy or missing"; exit 1; }
grep -q "crc_done" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
grep -q "PASS: SynthUI piano_key render verified" "$OUT" || { echo "FAIL: render verification token missing"; exit 1; }
echo "PASS: SynthUI piano_key render verified"
