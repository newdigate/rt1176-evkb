#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/build/lvgl_rk055_flip_test.elf"; OUT="$DIR/lvgl_rk055_flip.uart"
rm -f "$OUT" "$DIR/lvgl_rk055_flip.dbg"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/lvgl_rk055_flip.dbg" &
P=$!; gate_pid $P
# Poll for the DONE token rather than burning a fixed window; ceiling 20 s
# (80 x 0.25).  A healthy run: panel bring-up + 120 ack-driven 720x1280
# refreshes, each flip landing at the next vsync.
i=0
while [ $i -lt 80 ]; do
    [ -f "$OUT" ] && grep -q "LVGL_RK055_FLIP_DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
    i=$((i+1))
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

grep -q "PANEL_OK"            "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "MODE=DOUBLE_BUFFER"  "$OUT" || { echo "FAIL: wrong build variant in the gate"; exit 1; }
# THE CORE CLAIM -- the panel SCANNED buffer A, then buffer B (model latches
# the flip at vsync; the tap sums the latched address).  MATCH is printed by
# firmware only when fw-sum == tap-sum for that frame.
grep -q "FLIP_A=MATCH" "$OUT" || { echo "FAIL: panel did not scan buffer A"; exit 1; }
grep -q "FLIP_B=MATCH" "$OUT" || { echo "FAIL: panel did not scan buffer B"; exit 1; }
# Vacuity guard: identical frames would make the alternation proof unfalsifiable.
grep -q "DISTINCT=OK" "$OUT" || { echo "FAIL: frames identical -- alternation unproven"; exit 1; }
# Discipline: one flip per refresh, no dead waits.  Pinned exactly because
# the flow is ack-driven and deterministic.  Derivation (measured, then
# re-derived from source -- the plan's nominal was VSYNCS=120):
#   REFRESHES=120  TOTAL_FRAMES=120 includes the 2 assertion frames;
#                  frames_done is read from lvgl_mipi_panel_flips().
#   FLIPS=120      db_flush_cb issues one shadow-load per full refresh.
#   VSYNCS=3       s_db_vsyncs counts vsyncs consumed inside
#                  lvgl_mipi_panel_flip_sync() with a flip pending.  Only the
#                  firmware's three explicit calls qualify (after frame A,
#                  after frame B, after the discipline loop): db_flush_cb
#                  calls lv_display_flush_ready() synchronously, so LVGL's
#                  flush_wait_cb never observes a flush in progress and never
#                  reaches flip_sync with a flip pending.  The remaining 117
#                  flips land at vsync unobserved by this counter (LVGL's
#                  33 ms refresh period > the 17 ms frame period, so each
#                  lands before the next is issued -- but nobody consumes
#                  the landing).  A change that makes the fence per-frame
#                  must re-pin this to 120 and delete this paragraph.
grep -q "^REFRESHES=120$"      "$OUT" || { echo "FAIL: refresh count"; exit 1; }
grep -q "^FLIPS=120$"          "$OUT" || { echo "FAIL: flip count"; exit 1; }
grep -q "^VSYNCS=3$"           "$OUT" || { echo "FAIL: vsync count"; exit 1; }
grep -q "^VSYNC_TIMEOUTS=0$"   "$OUT" || { echo "FAIL: a vsync wait gave up"; exit 1; }
grep -q "FLIP_OK"              "$OUT" || { echo "FAIL: firmware verdict withheld"; exit 1; }
[ -f "$DIR/lvgl_rk055_flip.dbg" ] || { echo "FAIL: no guest-error log"; exit 1; }
grep -q "guest" "$DIR/lvgl_rk055_flip.dbg" && { echo "FAIL: guest errors logged"; exit 1; }
echo "PASS: the panel scanned buffer A, then buffer B"
