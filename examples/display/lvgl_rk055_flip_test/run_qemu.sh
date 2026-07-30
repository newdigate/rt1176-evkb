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
# Discipline: one flip per refresh, EVERY landing consumed exactly once, no
# dead waits.  Pinned exactly because the flow is ack-driven and
# deterministic.  Derivation:
#   REFRESHES=120  TOTAL_FRAMES=120 includes the 2 assertion frames;
#                  frames_done is read from lvgl_mipi_panel_flips().
#   FLIPS=120      db_flush_cb issues one shadow-load per full refresh.
#   VSYNCS=120     one consumed landing per flip.  This number is the direct
#                  witness of the binding's invariant enforcement: the last
#                  flush DEFERS lv_display_flush_ready(), so LVGL invokes
#                  flush_wait_cb (which waits out the vsync) before it next
#                  touches the buffer.  HISTORY, kept on purpose: the first
#                  build called flush_ready synchronously in flush_cb, the
#                  wait_cb never engaged, and this counter read 3 (only the
#                  example's own explicit flip_sync calls) -- the invariant
#                  was resting on the 33 ms refresh period exceeding the
#                  17 ms flip latency, timing luck the gate correctly
#                  refused.  If this pin ever reads low again, that
#                  regression is back.  As of v5 the wait CONSUMES the
#                  ISR-set retire flag rather than polling INT_STATUS
#                  itself, so "if this reads low" now covers a dead ISR
#                  too: an interrupt that never fires leaves nothing to
#                  consume and the waits all time out.
grep -q "^REFRESHES=120$"      "$OUT" || { echo "FAIL: refresh count"; exit 1; }
grep -q "^FLIPS=120$"          "$OUT" || { echo "FAIL: flip count"; exit 1; }
grep -q "^VSYNCS=120$"         "$OUT" || { echo "FAIL: vsync count -- the flush_wait fence is not engaging"; exit 1; }
# One retire per flip, BY THE ISR -- interrupt delivery end-to-end
# (INT_ENABLE, NVIC, vector, W1C) under the modelled level IRQ.  Retires,
# not ISR entries: the ISR runs on every ~60 Hz vsync and raw entry counts
# are runtime-dependent -- pinning them would flake by design.
grep -q "^VSYNC_ISRS=120$" "$OUT" || { echo "FAIL: ISR did not retire every flip"; exit 1; }
grep -q "^VSYNC_TIMEOUTS=0$"   "$OUT" || { echo "FAIL: a vsync wait gave up"; exit 1; }
# v6 adoption corroboration (the IDLE_POLLS idiom): the PXP sync-copy handler
# must exist AND have engaged.  NOT pinned exactly -- the copy count tracks
# the animation's invalidation pattern, and a pinned value here would be
# vacuous precision.  Correctness is carried by the byte-identical tokens
# above: a wrong copy moves the FLIP sums and the MATCH pair.
grep -q "^PXP_COPIES=" "$OUT" || { echo "FAIL: pxp copy count missing"; exit 1; }
grep -q "^PXP_COPIES=0$" "$OUT" && { echo "FAIL: handler installed but never engaged"; exit 1; }
grep -q "FLIP_OK"              "$OUT" || { echo "FAIL: firmware verdict withheld"; exit 1; }
[ -f "$DIR/lvgl_rk055_flip.dbg" ] || { echo "FAIL: no guest-error log"; exit 1; }
grep -q "guest" "$DIR/lvgl_rk055_flip.dbg" && { echo "FAIL: guest errors logged"; exit 1; }
echo "PASS: the panel scanned buffer A, then buffer B"
