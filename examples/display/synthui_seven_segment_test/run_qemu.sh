#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/synthui_seven_segment_test.elf"
OUT=$(gate_capture_path "$DIR" synthui_seven_segment.uart)
DBG=$(gate_capture_path "$DIR" synthui_seven_segment.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
sleep 20; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "PANEL_OK"          "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "seven_segment_scene=6 count=6" "$OUT" || { echo "FAIL: scene line missing"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
grep -qE "LVGL_BYTES=3686400\r?$" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }

# GOLDEN CHECKSUM -- FNV-1a over the whole 720x1280 PRESENTED buffer, the
# 6-readout bank:
#   1. Main BPM Readout: "140.0" (Blue)
#   2. Bar:Beat Readout: "01:04" (Cyan)
#   3. Pattern Readout: "P-A3" (Amber)
#   4. Track Status Readout: "REC.8" (Red)
#   5. Ghost-off Readout: "8888" (Cyan, ghost=false)
#   6. Disabled Readout: "OFF" (Red, disabled)
# Recorded across consecutive QEMU runs (2026-09-07, NEW-29, SynthUI seven_segment,
# vendored LVGL 9.4.0, XRGB8888).
# On a mismatch work out WHICH of {SynthUI pin, LVGL pin, lv_conf.h, fonts,
# scene} changed; do NOT paste in whatever the board printed.
# Demonstrated RED 2026-09-07: CRC altered to 0xDEADBEEF -> "FAIL: seven_segment checksum".
grep -qE "seven_segment_crc=0xA9629782\r?$" "$OUT" || { echo "FAIL: seven_segment checksum"; exit 1; }

# DELTA EQUALITY: 64-step sequence delta CRC must be PIXEL-IDENTICAL to fresh render
DSEQ=$(grep -a -oE "seven_segment_delta_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
DFUL=$(grep -a -oE "seven_segment_fresh_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DSEQ" ] && [ -n "$DFUL" ] || { echo "FAIL: delta guard tokens missing"; exit 1; }
[ "$DSEQ" = "$DFUL" ] || { echo "FAIL: delta render differs from full render ($DSEQ vs $DFUL)"; exit 1; }
grep -qE "seven_segment_delta_eq=PASS\r?$" "$OUT" || { echo "FAIL: delta equality"; exit 1; }

# ENGAGEMENT: largest single invalidated area must stay small (bound 20000 px)
DAREA=$(grep -a -oE "seven_segment_damage max=[0-9]+" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DAREA" ] && [ "$DAREA" -gt 0 ] || { echo "FAIL: delta damage guard missing or zero"; exit 1; }
[ "$DAREA" -le 20000 ] || { echo "FAIL: delta damage not engaged (max=$DAREA)"; exit 1; }

# VSYNC health
grep -qE "seven_segment_vsync flips=[0-9]+ isrs=[0-9]+ timeouts=0\r?$" "$OUT" || { echo "FAIL: vsync fence unhealthy or missing"; exit 1; }
grep -q "crc_done" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
grep -q "PASS: SynthUI seven_segment render verified" "$OUT" || { echo "FAIL: render verification token missing"; exit 1; }
echo "PASS: SynthUI seven_segment render verified"
