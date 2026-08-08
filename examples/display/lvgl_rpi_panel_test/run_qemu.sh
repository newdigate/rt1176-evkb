#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location. The old
# hardcoded ~/Development/rt1170/evkb/tools/... meant a worktree or a clone at
# any other path silently loaded a DIFFERENT tree's gate-lib.sh -- which surfaces
# as "gate_reap: command not found", or worse, as a gate quietly running against
# the wrong library.
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/lvgl_rpi_panel_test.elf"; OUT="$DIR/lvgl_rpi.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/lvgl_rpi.dbg" &
P=$!; gate_pid $P
# sleep 12 (vs 10 for the ILI9341 gate): this image also runs the whole RPi panel
# bring-up (ATtiny/I2C + VIDEO_PLL + LCDIFv2 + MIPI-DSI + TC358762) before LVGL
# gets a look in, and then renders 800x480 instead of 320x240. Same cold-binary
# timing flake rpi_panel_test documents; 12s gives margin.
sleep 12; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
# The panel chain itself must still come up before LVGL means anything: in DIRECT
# mode LVGL renders straight into the LCDIFv2's live scanout buffer, so a
# framebuffer that no display owns would still checksum perfectly.
grep -q "DISPLAY_OK"        "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
# LVGL_BYTES is the total AREA LVGL handed to flush_cb, summed over the refresh
# (800*480*2 = 768000 only if it redrew the whole screen). It is NOT the extent
# of the checksummed feed -- that is one unconditional PANEL_FB_BYTES call and
# asserting it would be a check with no reachable failure.
#
# This is the one regression the golden checksum below cannot name: if LVGL
# invalidated only a corner, the frame would still hash to *a* number, and a
# partial repaint and a deliberate scene edit are indistinguishable from it.
# The flushed area tells them apart.
#
# NOTE the ILI9341 gate's identically-named token means something else: there
# partial mode accumulates the SUM slice-by-slice inside flush_cb, so the byte
# count is the checksum's own extent. Same token, different oracle -- read the
# comment in lvgl_rpi_panel_test.cpp before "fixing" one into the other.
grep -q "LVGL_BYTES=768000" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }
# GOLDEN CHECKSUM -- FNV-1a over the whole 800x480 framebuffer after LVGL's
# software renderer has drawn the scene into it. QEMU does model this panel, but
# what it models is the transport (rpi_panel_test's FB_SUM/PANEL_SUM assert
# that); this number is the RENDERER's output, and it is the only automated
# signal that LVGL painted the right pixels. Glass is NOT verified: the RPi panel
# is not currently connected, so this golden pins REPRODUCIBILITY only, never
# correctness. (Contrast the ILI9341 gate, whose golden a human has confirmed on
# real glass.) See the capability table in the repo README.
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
grep -q "LVGL_SUM=0xB220E6E4" "$OUT" || { echo "FAIL: render checksum"; exit 1; }
echo "PASS: LVGL RPi panel render verified"
