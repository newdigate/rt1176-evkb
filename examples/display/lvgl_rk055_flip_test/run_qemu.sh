#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/lvgl_rk055_flip_test.elf"; OUT="$DIR/lvgl_rk055_flip.uart"
rm -f "$OUT" "$DIR/lvgl_rk055_flip.dbg"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
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

# DEMONSTRATED RED (NEW-55, 2026-09-19) for the FLIP_B_SUM pin below, which is
# this gate's only witness of the sync copy's 32-bit byte contract.  Deleting
# `if (bpp == 4u) op.alphaOut(0xFF);` from LVGL's port/lvgl_pxp_copy.cpp and
# rebuilding made this gate exit 1 with
#   FAIL: buffer-B golden moved -- the sync copy's byte contract changed
# and the mutant's sum came back as EXACTLY v6's old value, 0xB90DE065, while
# FLIP_A_SUM (the un-synced control) did not move -- so the A pin is not
# merely duplicating the B pin.  Without these two greps the same mutant left
# the gate fully GREEN.
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
# The byte contract, pinned directly.  B is the first refresh WITH a previous
# frame to sync from, so its sum is the only token in the tree that moved when
# the copy's byte 3 went 0 -> 0xFF (NEW-55); A is the control -- first refresh,
# no sync, so a copy defect must move B and leave A alone.  Deterministic:
# two runs bit-identical, 2026-09-19.  Was 0xB90DE065 under v6's X:=0.
grep -q "^FLIP_A_SUM=0x1C8E7D65 PANEL_A_SUM=0x1C8E7D65$" "$OUT" || { echo "FAIL: buffer-A golden moved (the un-synced control)"; exit 1; }
grep -q "^FLIP_B_SUM=0x4B7E8C65 PANEL_B_SUM=0x4B7E8C65$" "$OUT" || { echo "FAIL: buffer-B golden moved -- the sync copy's byte contract changed"; exit 1; }
# v6 adoption corroboration (the IDLE_POLLS idiom): the PXP sync-copy handler
# must exist AND have engaged.  NOT pinned exactly -- the copy count tracks
# the animation's invalidation pattern, and a pinned value here would be
# vacuous precision.  Correctness is carried by the two goldens just pinned:
# FLIP_B_SUM is the DIRECT witness of the copy's byte contract (a wrong copy
# moves it, with FLIP_A_SUM as the un-synced control), and the MATCH pair is
# the separate firmware-vs-model cross-check -- it says the panel really
# scanned the bytes the firmware summed, not that those bytes are right.
# ★ NEW-55, 2026-09-19: the handler moved into lvgl_mipi_panel_create_db()
# (this example no longer installs it) and its 32-bit copy now carries
# alphaOut(0xFF), so byte 3 of a sync-copied pixel is 0xFF where v6's copy
# wrote 0.  FLIP_B_SUM/PANEL_B_SUM therefore MOVED, 0xB90DE065 ->
# 0x4B7E8C65, on two bit-identical runs; the fixture is re-captured.
# FLIP_A_SUM is UNCHANGED (0x1C8E7D65) because buffer A is the FIRST
# refresh, which has no previous frame to sync from.  Until the pin above
# was added this gate had NO assertion on either sum, and so went green
# straight through that deliberate change of contract -- which is why the
# pin exists.  The contract's silicon evidence is separate
# (lvgl_pxp_copy_bench, NEW-55 PROBE, arm=pxp_aff, two boots).
grep -q "^PXP_COPIES=" "$OUT" || { echo "FAIL: pxp copy count missing"; exit 1; }
grep -q "^PXP_COPIES=0$" "$OUT" && { echo "FAIL: handler installed but never engaged"; exit 1; }
# A dying PXP is loud by name, not a drifting fallback ratio.
grep -q "^PXP_ERRORS=0$" "$OUT" || { echo "FAIL: the PXP itself errored"; exit 1; }
grep -q "FLIP_OK"              "$OUT" || { echo "FAIL: firmware verdict withheld"; exit 1; }
[ -f "$DIR/lvgl_rk055_flip.dbg" ] || { echo "FAIL: no guest-error log"; exit 1; }
grep -q "guest" "$DIR/lvgl_rk055_flip.dbg" && { echo "FAIL: guest errors logged"; exit 1; }
echo "PASS: the panel scanned buffer A, then buffer B"
