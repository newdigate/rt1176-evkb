#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/synthui_slide_toggle_test.elf"
OUT=$(gate_capture_path "$DIR" synthui_slide_toggle.uart)
DBG=$(gate_capture_path "$DIR" synthui_slide_toggle.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
sleep 20; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "PANEL_OK"          "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "slide_toggle_scene=gallery toggles=14 states=14" "$OUT" || { echo "FAIL: scene line missing"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
grep -qE "LVGL_BYTES=3686400\r?$" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }

# GOLDEN CHECKSUM -- FNV-1a over the whole 720x1280 PRESENTED buffer, the
# SlideToggle gallery scene (14 toggles, 2/3/4 positions, waveform glyphs,
# panel color variations, disabled states).
# Recorded across consecutive QEMU runs (2026-09-07, NEW-30, SynthUI slide_toggle,
# vendored LVGL 9.4.0, XRGB8888).
# Demonstrated RED 2026-09-07: CRC altered to 0xDEADBEEF -> "FAIL: slide_toggle checksum".
grep -qE "slide_toggle_crc=0x6F58B112\r?$" "$OUT" || { echo "FAIL: slide_toggle checksum"; exit 1; }

# DELTA EQUALITY: 64-step sequence delta CRC must be PIXEL-IDENTICAL to fresh render
DSEQ=$(grep -a -oE "slide_toggle_delta_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
DFUL=$(grep -a -oE "slide_toggle_fresh_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DSEQ" ] && [ -n "$DFUL" ] || { echo "FAIL: delta guard tokens missing"; exit 1; }
[ "$DSEQ" = "$DFUL" ] || { echo "FAIL: delta render differs from full render ($DSEQ vs $DFUL)"; exit 1; }
grep -qE "slide_toggle_delta_eq=PASS\r?$" "$OUT" || { echo "FAIL: delta equality"; exit 1; }

# ENGAGEMENT: largest single invalidated area must stay small (bound 15000 px)
DAREA=$(grep -a -oE "slide_toggle_damage max=[0-9]+" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DAREA" ] && [ "$DAREA" -gt 0 ] || { echo "FAIL: delta damage guard missing or zero"; exit 1; }
[ "$DAREA" -le 15000 ] || { echo "FAIL: delta damage not engaged (max=$DAREA)"; exit 1; }

# VSYNC health
grep -qE "slide_toggle_vsync flips=[0-9]+ isrs=[0-9]+ timeouts=0\r?$" "$OUT" || { echo "FAIL: vsync fence unhealthy or missing"; exit 1; }
grep -q "crc_done" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
grep -q "PASS: SynthUI slide_toggle render verified" "$OUT" || { echo "FAIL: render verification token missing"; exit 1; }
echo "PASS: SynthUI slide_toggle render verified"
