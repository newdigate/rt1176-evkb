#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
export REAL_QEMU="${REAL_QEMU:-/Users/moolet/Development/qemu-rt1170/build/qemu-system-arm}"
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/synthui_level_meter_test.elf"
OUT=$(gate_capture_path "$DIR" synthui_level_meter.uart)
DBG=$(gate_capture_path "$DIR" synthui_level_meter.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
sleep 20; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "PANEL_OK"          "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "level_meter_scene=8 count=8" "$OUT" || { echo "FAIL: scene line missing"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
grep -qE "LVGL_BYTES=3686400\r?$" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }

# GOLDEN CHECKSUM -- FNV-1a over the whole 720x1280 PRESENTED buffer, the
# 8-meter synth console scene:
#   1-2. Master Stereo L/R: 24 segments, W=48, H=380, L at (80, 80), R at (144, 80)
#   3-6. Tracks 1-4: 16 segments, W=36, H=240, at (240, 80), (290, 80), (340, 80), (390, 80)
#   7-8. Aux 1-2: 10 segments, W=42, H=180, at (470, 80) and (530, 80) (Aux 2 clip=true)
# Recorded across consecutive QEMU runs (2026-09-07, NEW-26, SynthUI level_meter,
# vendored LVGL 9.4.0, XRGB8888).
# On a mismatch work out WHICH of {SynthUI pin, LVGL pin, lv_conf.h, fonts,
# scene} changed; do NOT paste in whatever the board printed.
# Demonstrated RED 2026-09-07: CRC altered to 0xDEADBEEF -> "FAIL: level_meter checksum".
grep -qE "level_meter_crc=0x23725035\r?$" "$OUT" || { echo "FAIL: level_meter checksum"; exit 1; }

# DELTA EQUALITY: 64-step sequence delta CRC must be PIXEL-IDENTICAL to fresh render
DSEQ=$(grep -a -oE "level_meter_delta_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
DFUL=$(grep -a -oE "level_meter_fresh_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DSEQ" ] && [ -n "$DFUL" ] || { echo "FAIL: delta guard tokens missing"; exit 1; }
[ "$DSEQ" = "$DFUL" ] || { echo "FAIL: delta render differs from full render ($DSEQ vs $DFUL)"; exit 1; }
grep -qE "level_meter_delta_eq=PASS\r?$" "$OUT" || { echo "FAIL: delta equality"; exit 1; }

# ENGAGEMENT: largest single invalidated area must stay small (bound 15000 px)
DAREA=$(grep -a -oE "level_meter_damage max=[0-9]+" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DAREA" ] && [ "$DAREA" -gt 0 ] || { echo "FAIL: delta damage guard missing or zero"; exit 1; }
[ "$DAREA" -le 15000 ] || { echo "FAIL: delta damage not engaged (max=$DAREA)"; exit 1; }

# VSYNC health
grep -qE "level_meter_vsync flips=[0-9]+ isrs=[0-9]+ timeouts=0\r?$" "$OUT" || { echo "FAIL: vsync fence unhealthy or missing"; exit 1; }
grep -q "crc_done" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
grep -q "PASS: SynthUI level_meter render verified" "$OUT" || { echo "FAIL: render verification token missing"; exit 1; }
echo "PASS: SynthUI level_meter render verified"
