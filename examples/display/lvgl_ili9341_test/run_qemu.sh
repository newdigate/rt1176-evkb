#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/lvgl_ili9341_test.elf"; OUT="$DIR/lvgl_ili9341.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/lvgl_ili9341.dbg" &
P=$!; gate_pid $P; sleep 10; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured UART ===="; cat "$OUT"
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
# Byte count proves a WHOLE 320x240 RGB565 frame reached flush_cb and that
# lvgl_sum_reset() was actually called. The checksum alone cannot tell a
# forgotten reset (or a never-fired flush_cb) from a different image.
grep -q "LVGL_BYTES=153600" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }
# GOLDEN CHECKSUM -- FNV-1a over every pixel LVGL's software renderer handed to
# flush_cb during one full refresh. QEMU models no ILI9341, so this is the ONLY
# automated signal here; the SPI transport is verified on silicon (Task 5).
#
# RECORDED, not derived: it legitimately changes when the LVGL pin, lv_conf.h,
# the fonts, or the scene change. On a mismatch work out WHICH of those changed.
# Do NOT paste in whatever the board printed -- that turns a real rendering
# regression into a green gate.
#
# Provenance: recorded 2026-07-27 against vendored LVGL 9.4.0, lv_conf.h as of
# that commit, montserrat 14/28. TO RE-RECORD when a scene/font/pin change
# legitimately invalidates it: confirm the new value is stable across two runs,
# AND confirm the panel looks right on real hardware, in the SAME commit that
# changes the number -- the checksum says "deterministic", not "correct", so
# only a human eye on the glass can re-establish what it is pinning.
grep -q "LVGL_SUM=0xA087211C" "$OUT" || { echo "FAIL: render checksum"; exit 1; }
echo "PASS: LVGL ILI9341 render verified"
