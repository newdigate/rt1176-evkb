#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/build/lvgl_rk055_touch_test.elf"; OUT="$DIR/lvgl_rk055_touch.uart"
rm -f "$OUT" "$DIR/lvgl_rk055_touch.dbg"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/lvgl_rk055_touch.dbg" &
P=$!; gate_pid $P
# Poll for the DONE token rather than burning a fixed window; ceiling 20 s
# (80 x 0.25).  A healthy run is done in ~6 s wall: panel bring-up + one
# 720x1280 render + the 27-instant script at 2 instants per ~40 ms.
i=0
while [ $i -lt 80 ]; do
    [ -f "$OUT" ] && grep -q "LVGL_RK055_TOUCH_DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
    i=$((i+1))
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

# --- the scene, before any touch --------------------------------------------
grep -q "PANEL_OK"           "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "LVGL_FLUSHED=PASS"  "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
grep -q "LVGL_BYTES=1843200" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }
# Pre-touch golden: taken BEFORE the indev exists (the model stalls its script
# until the first ack, and nobody polls until the indev is created), so it is
# static on QEMU and glass alike.  It asserts the scene BUILT correctly and
# says nothing about touch -- no post-touch checksum exists in this gate.
# Golden re-recorded for the v5 double-buffer migration: the checksum source
# moved to the SCANNED buffer (scanned_fb() after flip_sync); the pixel
# content is unchanged, and the value came back identical to v3's -- recorded
# (stable x2), not assumed.
# Provenance: recorded 2026-07-30, LVGL 9.4.0, montserrat 14/28.  Re-record
# rules as in lvgl_rk055_panel_test/run_qemu.sh.
grep -q "LVGL_SUM=0xE1559496" "$OUT" || { echo "FAIL: scene checksum"; exit 1; }

# --- touch bring-up (v2 owns the depth here; these are preconditions) --------
grep -q "I2C_OK"    "$OUT" || { echo "FAIL: touch bring-up"; exit 1; }
grep -q "ADDR=0x5D" "$OUT" || { echo "FAIL: wrong latched address"; exit 1; }
grep -q "GT911_OK"  "$OUT" || { echo "FAIL: product ID"; exit 1; }
grep -q "CFG_OK"    "$OUT" || { echo "FAIL: config blob"; exit 1; }

# --- LVGL's REACTION -- the claims this gate exists for ----------------------
# Five checkable buttons under the five scripted taps.  Real LVGL hit-testing
# and click delivery: a stuck/mirrored/swapped/unscaled coordinate checks at
# most one of them.
for t in 1 2 3 4 5; do
  grep -q "^BTN$t=CHECKED$" "$OUT" || { echo "FAIL: button $t never checked"; exit 1; }
done
# THE STAR WITNESS.  The handle follows the pointer only while LVGL believes
# the press CONTINUES across the Idle polls between drag samples (the binding
# runs at 10 ms; the model publishes every 20 ms).  Forward Idle as released
# and the handle stays at the left edge.  Pinned exactly: the script's last
# sample is x=648 -> handle 598; 9 of 10 samples change x (the first lands
# where the handle already is).  Legitimate changes to script or geometry
# re-derive this comment or the pin is meaningless.
grep -q "^DRAG_END x=598 moves=9$" "$OUT" || { echo "FAIL: drag did not track (Idle latch?)"; exit 1; }
# ASSERTED AS A PAIR, deliberately: TRAP=UNCHECKED alone is satisfied by a
# dead touch path.  HOLD=CHECKED proves the phase-3 press reached LVGL;
# TRAP=UNCHECKED then proves the pointer never re-adopted the finger that
# remained after the primary lifted (script phase 3b).
grep -q "^HOLD=CHECKED$"   "$OUT" || { echo "FAIL: phase-3 press never reached LVGL"; exit 1; }
grep -q "^TRAP=UNCHECKED$" "$OUT" || { echo "FAIL: pointer re-adopted the surviving finger"; exit 1; }

# --- honesty guards ----------------------------------------------------------
# IDLE_POLLS>0: the drag assertion above is about behaviour ACROSS idle polls;
# if timing drift ever removes them, this trips rather than letting the latch
# assertion pass vacuously (spec 8.2).
grep -q "^IDLE_POLLS=" "$OUT" || { echo "FAIL: idle-poll count missing"; exit 1; }
grep -q "^IDLE_POLLS=0$" "$OUT" && { echo "FAIL: no idle polls -- latch untested, re-tune the period"; exit 1; }
grep -q "^POLL_FAILS=0$" "$OUT" || { echo "FAIL: failed poll(s) -- QEMU cannot fault I2C, unmodelled"; exit 1; }
# Flip corroboration (the touch phases are the claim; these say the fence ran).
# NOT pinned exactly: refresh count under touch load is input-dependent, and a
# pinned value here would be vacuous precision.
grep -q "^FLIPS=" "$OUT" || { echo "FAIL: flip count missing"; exit 1; }
grep -q "^FLIPS=0$" "$OUT" && { echo "FAIL: no flips -- the db path is not live"; exit 1; }
grep -q "^VSYNC_TIMEOUTS=0$" "$OUT" || { echo "FAIL: a vsync wait gave up"; exit 1; }
# Every scripted instant consumed, exactly: 10 taps+releases, 10 drag samples,
# 1 release, 3 two-contact holds, 2 phase-3b, 1 final release = 27.
grep -q "^BUFFERS=27$" "$OUT" || { echo "FAIL: buffer count moved -- script/phase boundary shifted"; exit 1; }
grep -q "LVGL_TOUCH_OK" "$OUT" || { echo "FAIL: firmware verdict withheld"; exit 1; }

# --- the model's own guards --------------------------------------------------
[ -f "$DIR/lvgl_rk055_touch.dbg" ] || { echo "FAIL: no guest-error log"; exit 1; }
if grep -q "gt911: guest wrote config" "$DIR/lvgl_rk055_touch.dbg"; then
  echo "FAIL: firmware wrote the GT911 config space"; exit 1
fi
echo "PASS: LVGL reacted to every scripted contact"
