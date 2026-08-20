#!/bin/sh
# run_qemu_txaggr.sh — GATE for W16, TX multiport aggregation (MPA).
#
# WHAT THIS PROVES
#   After both pre-W16 service windows have finished and printed everything
#   the older gates read, the image stages SIX frames with TX aggregation
#   enabled and then calls flushTx(). Three things have to hold together:
#
#     1. ONE COMMAND CARRIES THEM ALL. `txaggr_done ... c53tx=1 batches=1
#        slots=6` is the driver's own accounting: c53tx is the data-port CMD53
#        count for the TX direction across the burst, and batches/slots count
#        only CMD53s that carried more than one slot. Six frames written with
#        one command is arithmetically impossible for a driver that writes one
#        port per command.
#
#     2. THE CARD COULD ACTUALLY TAKE THEM APART — and this is the half the
#        image cannot fake. With `tx-loopback=on` the model splits the run by
#        each packet's own block-padded SDIOPkt size, hands each piece to the
#        ring slot the address said it belonged to (checking that slot's
#        WR_BITMAP credit as it goes), and sends each one back up the UPLOAD
#        ring. So six frames coming back, carrying the indices this image put
#        in them, in order, is the card confirming the layout of a write the
#        host never gets to inspect. A wrong pad, a wrong port_count, a wrong
#        start slot, or a frame written to a port with no credit all show up
#        here as missing frames or as a model guest-error, not as a passing
#        run with a good counter.
#
#     3. NOTHING IS LEFT HELD. `queued=0` after the flush: a TX aggregation
#        that silently keeps a frame is a stall, not a saving. This is the
#        specific hazard of the feature — sendDataFrame() no longer means "on
#        the wire" — which is why the driver ships it DEFAULT OFF and why this
#        assertion is here rather than implied.
#
#   MEASURED (2026-08-20):
#     txaggr_done sent=6 flush=ok c53tx=1 batches=1 slots=6 queued=0
#     txaggr_loop rx=6 c53rx=11 rxbatches=1 rxslots=6
#   — and note the second line: the six looped-back frames were then READ back
#   in one aggregated CMD53 too, so this run exercises both directions of the
#   encoding against each other.
#
# DEMONSTRATED TO FAIL, not asserted on faith (2026-08-20). Iw416's
# AGGR_PKT_LIMIT was set to 1 — the smallest change that stops the driver
# batching while leaving TX aggregation switched ON, so `txaggr_begin aggr=1`
# still holds and the gate has to catch this on the COUNTERS rather than on the
# mode flag. It did:
#
#     txaggr_done sent=6 flush=ok c53tx=6 batches=0 slots=0 queued=0
#     txaggr_loop rx=6 c53rx=14 rxbatches=0 rxslots=0
#     FAIL: 6 staged frames cost 6 TX data CMD53s (want 1)
#
#   All six frames still went out, and all six still came back through the
#   card's loopback (`rx=6`) — every delivery assertion in this gate passed.
#   That is the point of keeping both halves: one-command-per-frame is CORRECT,
#   just not aggregated, and a gate that only checked delivery would have
#   blessed it. The limit was restored and the gate went green again.
#   (run_qemu_rxaggr.sh went red in the same build, for the same reason.)
#
# WHAT THIS DOES *NOT* PROVE
#   * NOT firmware download (`fw-preboot=on`, the model's admitted NOTE 7
#     fiction), NOT Wi-Fi, NOT silicon.
#   * NOT that a real IW416 accepts this write. `tx-loopback` is itself an
#     explicit fiction (the model's NOTE 15) — a real card puts the frame on
#     the air — and the aggregated address encoding is read off NXP's driver,
#     not off our wire. What this gate proves is that OUR two ends agree with
#     NXP's contract as read; silicon is the second gate, and it is the one
#     that decides.
#   * NOT the RX direction on its own — that is run_qemu_rxaggr.sh.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || {
    echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board)) — the M.2 socket and the"
    echo "      iw416-sdio model both live on the MIMXRT1170-EVKB machine"; exit 1; }
ELF="$DIR/$(gate_build_dir)/m2_rx_demo.elf"
OUT=$(gate_capture_path "$DIR" txaggr.uart)
DBG=$(gate_capture_path "$DIR" txaggr.dbg)
rm -f "$OUT" "$DBG"
# No injection at all: every frame that comes up the RX ring in this run is one
# the HOST transmitted, which is what makes the loopback assertion below an
# argument about the write rather than about the model's injector.
"$QEMU" $(gate_qemu_machine) -machine m2-wifi=on \
    -global iw416-sdio.fw-preboot=on \
    -global iw416-sdio.inject-count=0 \
    -global iw416-sdio.tx-loopback=on \
    -global iw416-sdio.inject-slot-first=0 \
    -global iw416-sdio.inject-slot-last=31 \
    -kernel "$ELF" -display none $(gate_console "$OUT") \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# The burst runs only after BOTH service windows, so this waits considerably
# longer than the other gates on this example.
for _ in $(seq 1 320); do
    [ -f "$OUT" ] && grep -q "^txaggr_loop " "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 M.2 RX demo up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
if grep -q "^sdio_begin=cmd5-no-response" "$OUT"; then
    echo "FAIL: the card-absent fallback ran — the iw416-sdio model did not attach"
    exit 1
fi
grep -q "^sdio_begin=ok " "$OUT" || { echo "FAIL: SDIO enumeration did not succeed"; exit 1; }
grep -q "^demo_ready[[:space:]]*$" "$OUT" || {
    echo "FAIL: never reached demo_ready — the command port did not complete"; exit 1; }
grep -q "^txaggr_begin aggr=1 " "$OUT" || {
    echo "FAIL: the burst did not run with TX aggregation enabled — txAggregation() read"
    echo "      back false, so nothing below would be testing aggregation at all"; exit 1; }
DONE=$(grep "^txaggr_done " "$OUT" | head -1)
LOOP=$(grep "^txaggr_loop " "$OUT" | head -1)
[ -n "$DONE" ] && [ -n "$LOOP" ] || {
    echo "FAIL: the run did not reach the TX-aggregation burst and its drain window"
    exit 1; }

SENT=$( echo "$DONE" | sed -n 's/.*sent=\([0-9]*\) .*/\1/p')
C53TX=$(echo "$DONE" | sed -n 's/.* c53tx=\([0-9]*\) .*/\1/p')
BATCH=$(echo "$DONE" | sed -n 's/.* batches=\([0-9]*\) .*/\1/p')
SLOTS=$(echo "$DONE" | sed -n 's/.* slots=\([0-9]*\) .*/\1/p')
HELD=$( echo "$DONE" | sed -n 's/.* queued=\([0-9]*\).*/\1/p')
# Anchored at the start of the line: an unanchored `.*rx=` is greedy and
# happily matches the `c53rx=` field further along, which reported "11 of 6
# frames came back" on the first run of this gate.
RX=$(   echo "$LOOP" | sed -n 's/^txaggr_loop rx=\([0-9]*\) .*/\1/p')
for v in "$SENT" "$C53TX" "$BATCH" "$SLOTS" "$HELD" "$RX"; do
    [ -n "$v" ] || { echo "FAIL: could not parse the burst counters"; exit 1; }
done

echo "$DONE" | grep -q " flush=ok " || {
    echo "FAIL: flushTx() did not report ok — the staged batch never reached the bus"; exit 1; }
[ "$SENT" -eq 6 ] || {
    echo "FAIL: only $SENT of 6 frames were staged (want 6) — a send failed before the"
    echo "      batch was ever written, so the command count below means nothing"; exit 1; }
[ "$HELD" -eq 0 ] || {
    echo "FAIL: $HELD frames are still queued after flushTx() — TX aggregation that holds"
    echo "      a frame is a stall, not a saving"; exit 1; }
# THE WIN.
[ "$C53TX" -eq 1 ] || {
    echo "FAIL: 6 staged frames cost $C53TX TX data CMD53s (want 1)"
    echo "      Six frames below the driver's packet limit, flushed once, is exactly one"
    echo "      aggregated write. More than one means the batch was broken up; the"
    echo "      likely causes are a ring wrap mid-burst or a wr-bitmap wait that flushed."
    exit 1; }
[ "$BATCH" -eq 1 ] && [ "$SLOTS" -eq 6 ] || {
    echo "FAIL: txaggr batches/slots read $BATCH/$SLOTS (want 1/6) — these count only"
    echo "      CMD53s that carried more than one slot, so 0/0 means each frame went out"
    echo "      on its own command whatever c53tx says"; exit 1; }
# THE HALF THE IMAGE CANNOT FAKE: the card took the run apart and gave each
# piece its own slot. Six frames back, carrying the indices we put in them.
[ "$RX" -eq 6 ] || {
    echo "FAIL: $RX of 6 frames came back through the card's loopback (want 6) — the"
    echo "      aggregated write was accepted but the card could not split it into six"
    echo "      packets on six download slots. That is a disagreement about the layout"
    echo "      (per-slot block padding, port_count, or start slot), not about counting."
    exit 1; }
i=1
while [ $i -le 6 ]; do
    grep -q "^rx_frame [0-9]*: len=60 .* ethertype=0x88B6 seq=$i " "$OUT" || {
        echo "FAIL: looped-back frame $i never came back — the card's split of the"
        echo "      aggregated write did not reproduce the frame this image staged"
        exit 1; }
    i=$((i + 1))
done
# Every illegal aggregated access the model knows how to spot logs a guest
# error: a write to a slot with no WR_BITMAP credit, a packet running past the
# bytes the CMD53 carried, a start slot out of range.
if [ -f "$DBG" ] && grep -q "iw416-sdio" "$DBG"; then
    echo "FAIL: the card model reported guest errors — the aggregated write is malformed"
    echo "      even though the frames came back:"
    grep "iw416-sdio" "$DBG" | head -5
    exit 1
fi
echo "PASS: 6 frames staged into ONE aggregated CMD53 ($BATCH batch / $SLOTS slots),"
echo "      split by the card and all 6 returned in order — W16 TX multiport aggregation"
