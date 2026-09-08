#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/synthui_panel_button_test.elf"
OUT=$(gate_capture_path "$DIR" synthui_panel_button.uart)
DBG=$(gate_capture_path "$DIR" synthui_panel_button.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# 20s: bring-up margin plus headroom for the 64 delta steps and 64 fps loop frames.
# Tokens land ~3s in on an idle machine.
sleep 20; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
# Panel chain first: a framebuffer no display owns would still checksum.
grep -q "PANEL_OK"          "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "panel_button_scene=16 grid=4x4" "$OUT" || { echo "FAIL: scene line missing"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
# Flushed AREA of the first refresh -- 720*1280*4 at XRGB8888. Value greps
# are ANCHORED (CR-tolerant \r?$): a golden must match the WHOLE value.
grep -qE "LVGL_BYTES=3686400\r?$" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }
# GOLDEN CHECKSUM -- FNV-1a over the whole 720x1280 PRESENTED buffer, the
# 16-button bank (4x4 grid: Play, Stop, Record, Rewind, Forward, Up, Down, Bar,
# Dot, Blank, Large Play, Large Stop, Disabled Play, Disabled Record, Custom Pink Dot, Custom Cyan Bar).
# Recorded across consecutive QEMU runs (2026-09-07, NEW-27, SynthUI panel_button,
# vendored LVGL 9.4.0, XRGB8888).
# On a mismatch work out WHICH of {SynthUI pin, LVGL pin, lv_conf.h, fonts,
# scene} changed; do NOT paste in whatever the board printed.
# Demonstrated RED 2026-09-07: CRC altered to 0xDEADBEEF -> "FAIL: panel_button checksum".
grep -qE "panel_button_crc=0xF90B7996\r?$" "$OUT" || { echo "FAIL: panel_button checksum"; exit 1; }
# DELTA EQUALITY: a 64-step LCG sequence rendered via the widget's delta
# damage must be PIXEL-IDENTICAL to a fresh full render of the final state.
DSEQ=$(grep -a -oE "panel_button_delta_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
DFUL=$(grep -a -oE "panel_button_fresh_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DSEQ" ] && [ -n "$DFUL" ] || { echo "FAIL: delta guard tokens missing"; exit 1; }
[ "$DSEQ" = "$DFUL" ] || { echo "FAIL: delta render differs from full render ($DSEQ vs $DFUL)"; exit 1; }
grep -qE "panel_button_delta_eq=PASS\r?$" "$OUT" || { echo "FAIL: delta equality"; exit 1; }
# ENGAGEMENT: the largest single invalidated area during the recorded
# 64-step segment must stay button-sized (bound 10000 px; measured max 7200 px; a full
# 720x1280 screen is 921600 px) -- a change that quietly reverts set_on to
# full invalidation fails HERE and nowhere else.
DAREA=$(grep -a -oE "panel_button_damage max=[0-9]+" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DAREA" ] && [ "$DAREA" -gt 0 ] || { echo "FAIL: delta damage guard missing or zero"; exit 1; }
[ "$DAREA" -le 10000 ] || { echo "FAIL: delta damage not engaged (max=$DAREA)"; exit 1; }
# vsync-fence health (db pipeline): a timeout means the tear-free property
# silently degraded with every golden still green.
grep -qE "panel_button_vsync flips=[0-9]+ isrs=[0-9]+ timeouts=0\r?$" "$OUT" || { echo "FAIL: vsync fence unhealthy or missing"; exit 1; }
grep -q "crc_done" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
grep -q "PASS: SynthUI panel_button render verified" "$OUT" || { echo "FAIL: render verification token missing"; exit 1; }
echo "PASS: SynthUI panel_button render verified"
