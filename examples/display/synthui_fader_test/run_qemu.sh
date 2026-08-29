#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location (a
# hardcoded path silently loads a different tree's gate-lib.sh from a worktree).
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/synthui_fader_test.elf"
OUT=$(gate_capture_path "$DIR" synthui_fader.uart)
DBG=$(gate_capture_path "$DIR" synthui_fader.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# 20s: synthui_knob_test's 16s bring-up margin plus headroom for the 66
# lv_refr_now delta steps. Tokens land ~3s in on an idle machine.
sleep 20; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
# Panel chain first: a framebuffer no display owns would still checksum.
grep -q "PANEL_OK"          "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "fd_scene=16 grid=2x8" "$OUT" || { echo "FAIL: scene line missing"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
# Flushed AREA of the first refresh -- 720*1280*4 at XRGB8888. Value greps
# are ANCHORED (CR-tolerant \r?$): a golden must match the WHOLE value.
grep -qE "LVGL_BYTES=3686400\r?$" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }
# GOLDEN CHECKSUM -- FNV-1a over the whole 720x1280 PRESENTED buffer, the
# spec section-9 bank (all config axes inside the one scene: three states,
# center on/off, four panel greys, three tick counts). RECORDED, not
# derived -- bit-identical across three consecutive QEMU runs (Task 3,
# 2026-08-29, SynthUI ff6fa12+, vendored LVGL 9.4.0, XRGB8888, montserrat
# 14/28), and the frame was LOOKED AT via -DFD_EYEBALL_HOLD=1 + pmemsave
# (fb address 0x80484080 for that elf, read from LCDIFv2 0x4080820c; the
# dump's own FNV-1a equalled this value while the eye checked the bank,
# states, panels and tick axes). sw-only widget: silicon must reproduce
# THIS value (no second golden set).
# On a mismatch work out WHICH of {SynthUI pin, LVGL pin, lv_conf.h, fonts,
# scene} changed; do NOT paste in whatever the board printed.
# Demonstrated RED 2026-08-29: last digit flipped -> "FAIL: bank checksum".
grep -qE "fd_crc=0xAB66DE0D\r?$" "$OUT" || { echo "FAIL: bank checksum"; exit 1; }
# DELTA EQUALITY (spec section 9): a 66-step value sequence rendered via
# the widget's cap-extent delta damage must be PIXEL-IDENTICAL to a fresh
# full render of the final state. The GATE compares the two printed sums --
# never re-goldened. A too-tight extent (stale shadow/stroke pixels) fails
# here. Demonstrated RED 2026-08-29: cap-extent shadow term 2.5 -> -3.5
# (a 6-unit / ~4.7px reduction at this widget's 78px-wide 0.78 px/unit
# scale) -> "FAIL: delta render differs from full render (0x6EACD7B3 vs
# 0xEA9A04AB)". NOTE: a smaller 2.5 -> 0.5 edit (2 units / ~1.56px) does
# NOT fail -- fd_cap_extent's own +2px rounding slack absorbs any deficit
# under ~1.6px at this scale, so what these two probes establish is
# 2 units < sensitivity floor <= 6 units: the +2 px inflation absorbs any
# cut smaller than 2 px (2.56 units at this scale), and the 6-unit cut
# demonstrably exceeds it.
DSEQ=$(grep -a -oE "fd_delta_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
DFUL=$(grep -a -oE "fd_fresh_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DSEQ" ] && [ -n "$DFUL" ] || { echo "FAIL: delta guard tokens missing"; exit 1; }
[ "$DSEQ" = "$DFUL" ] || { echo "FAIL: delta render differs from full render ($DSEQ vs $DFUL)"; exit 1; }
# ENGAGEMENT: the largest single invalidated area during the recorded
# 64-step segment must stay cap-sized (measured 3234 px; bound 6000; a
# whole 78x210 fader is 16380) -- a change that quietly reverts set_value
# to full invalidation fails HERE and nowhere else, because delta and
# fresh are then both full renders and the equality guard stays green.
# Demonstrated RED 2026-08-29: set_value scratch-reverted to
# lv_obj_invalidate -> "FAIL: delta damage not engaged (max=16380)" with
# the equality guard still PASS on the same capture.
DAREA=$(grep -a -oE "fd_damage max=[0-9]+" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DAREA" ] && [ "$DAREA" -gt 0 ] || { echo "FAIL: delta damage guard missing or zero"; exit 1; }
[ "$DAREA" -le 6000 ] || { echo "FAIL: delta damage not engaged (max=$DAREA)"; exit 1; }
# vsync-fence health (db pipeline): a timeout means the tear-free property
# silently degraded with every golden still green.
grep -qE "fd_vsync flips=[0-9]+ isrs=[0-9]+ timeouts=0\r?$" "$OUT" || { echo "FAIL: vsync fence unhealthy or missing"; exit 1; }
grep -q "crc_done" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
echo "PASS: SynthUI fader render verified"
