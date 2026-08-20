#!/bin/sh
# run_qemu_rxaggr.sh — GATE for W16, RX multiport aggregation (MPA).
#
# WHAT THIS PROVES
#   The card model is told to place EIGHT uploads on ring slots 0..7 in a
#   single burst (`inject-period-ms=0`, so they are all resident before the
#   host next looks at the ring). Two things then have to hold together:
#
#     1. EVERY FRAME ARRIVES, IN ORDER, paired with the slot the CARD chose
#        (`seq=K from_slot=S`) — the same oracle run_qemu_ring.sh uses. An
#        aggregated read that loses, duplicates or reorders frames is not a
#        win, it is a regression with a good counter.
#     2. THEY COST FEWER COMMANDS THAN THEY ARE. `c53rx` is the driver's
#        DATA-port CMD53 count for the RX direction, and `rxaggr=B/S` is
#        <CMD53s that carried more than one slot>/<slots those carried>.
#        Eight frames delivered with c53rx <= 4 is arithmetically impossible
#        for a driver that reads one slot per command, whatever else it does;
#        rxaggr's S >= 6 says the same thing from the other side and cannot be
#        reached at all without a multi-slot transfer. That pairing is what
#        makes this a measurement rather than a hopeful assertion — neither
#        number can be improved by polling faster, buffering more, or getting
#        lucky with scheduling.
#
#   MEASURED (2026-08-20, three consecutive runs, identical every time):
#     demo_done frames=8 rx_data=8 rd_bitmap=0x0 ... c53rx=1 rxaggr=1/8
#   i.e. all eight uploads came up in ONE CMD53. The thresholds below are set
#   at c53rx <= 4 and S >= 6 so that a batch split by scheduling jitter still
#   passes while one-command-per-frame cannot.
#
# DEMONSTRATED TO FAIL, not asserted on faith (2026-08-20). Iw416's
# m_rxAggr default was flipped to false — the one-line change that turns the
# batch bound back down to a single slot, i.e. exactly the pre-W16 driver —
# and this gate went RED on the counters while still delivering all 8 frames:
#
#     demo_done frames=8 rx_data=8 rd_bitmap=0x0 ... c53rx=8 rxaggr=0/0
#     RX aggregation: 0 slots in 0 multi-slot CMD53(s); 8 RX data CMD53s for 8 frames
#     FAIL: 8 frames cost 8 RX data CMD53s (want <= 4)
#
#   Note WHICH half went red: the frames-in-order assertions all passed. That
#   is the point of having both — a gate that only checked delivery would have
#   called the non-aggregating driver correct, because it IS correct, just not
#   faster. The driver was then restored and the gate went green again.
#
# WHAT THIS DOES *NOT* PROVE
#   * NOT firmware download. The model runs with `fw-preboot=on`, an admitted
#     FICTION (its NOTE 7) — the real IW416 blob is NXP-licensed and no gate
#     may depend on it.
#   * NOT Wi-Fi. No scan, no association, no radio. The frames are injected.
#   * NOT the aggregated ADDRESS ENCODING against silicon. The model decodes
#     (ioport | MPA_ADDR_BASE | ((ports-1) << 8)) + start_port because that is
#     what NXP's driver emits, not because this tree has seen it on a wire.
#     Both ends of that encoding are ours until a hardware run says otherwise;
#     see the design doc's "Where this could be lying".
#   * NOT the TX direction — that is run_qemu_txaggr.sh, kept separate so each
#     gate names one property.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
# The M.2 socket (J54 on USDHC1) is a MIMXRT1170-EVKB feature and the model is
# only attached by the mimxrt1170-evk machine. Fail by name rather than handing
# `m2-wifi=on` to a machine that has no such property and blaming the firmware.
[ "$(gate_board)" = rt1176 ] || {
    echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board)) — the M.2 socket and the"
    echo "      iw416-sdio model both live on the MIMXRT1170-EVKB machine"; exit 1; }
ELF="$DIR/$(gate_build_dir)/m2_rx_demo.elf"
OUT=$(gate_capture_path "$DIR" rxaggr.uart)
DBG=$(gate_capture_path "$DIR" rxaggr.dbg)
rm -f "$OUT" "$DBG"
# `inject-period-ms=0` is what makes this a BURST rather than a trickle: the
# model re-arms its injection timer at the same virtual time, so all eight
# uploads are queued before the guest runs again and the ring genuinely holds a
# run of occupied slots. Without it the host drains each frame as it lands and
# there is nothing to aggregate — which is a real property of the workload, not
# a defect: aggregation can only help when frames arrive faster than the host
# services them.
"$QEMU" $(gate_qemu_machine) -machine m2-wifi=on \
    -global iw416-sdio.fw-preboot=on \
    -global iw416-sdio.inject-count=8 \
    -global iw416-sdio.inject-period-ms=0 \
    -global iw416-sdio.inject-slot-first=0 \
    -global iw416-sdio.inject-slot-last=7 \
    -kernel "$ELF" -display none $(gate_console "$OUT") \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# Wait for the line AFTER the one this gate asserts on -- see run_qemu_irq.sh's
# note: a reap landing mid-`demo_done` fails the field matches on a healthy run.
for _ in $(seq 1 160); do
    [ -f "$OUT" ] && grep -q "^irq_mode=" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 M.2 RX demo up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
# THE NEGATIVE THAT MAKES THE REST MEAN SOMETHING. Without the model attached
# the board gets a plain SD memory card, which ignores CMD5 — the outcome
# run_qemu.sh asserts. Every assertion below would then be vacuous.
if grep -q "^sdio_begin=cmd5-no-response" "$OUT"; then
    echo "FAIL: the card-absent fallback ran — the iw416-sdio model did not attach"
    echo "      (a QEMU without the model REJECTS -machine m2-wifi=on outright, which"
    echo "       shows up as an empty capture; reaching this line instead means the"
    echo "       property was accepted and a plain SD card was attached anyway)"
    exit 1
fi
grep -q "^sdio_begin=ok " "$OUT" || { echo "FAIL: SDIO enumeration did not succeed"; exit 1; }
grep -q "^demo_ready[[:space:]]*$" "$OUT" || {
    echo "FAIL: never reached demo_ready — the command port did not complete"; exit 1; }

# HALF ONE: every frame, in order, on the slot the CARD chose. `[[:space:]]*$`
# rather than a bare `$` on end-anchored patterns: Print's println() emits CRLF.
i=1
while [ $i -le 8 ]; do
    slot=$((i - 1))
    grep -q "^rx_frame $i: len=64 .* seq=$i from_slot=$slot " "$OUT" || {
        echo "FAIL: frame $i (card seq $i, ring slot $slot) never reached the sink —"
        echo "      an aggregated read that drops or reorders frames is a regression,"
        echo "      however few commands it used"
        exit 1; }
    i=$((i + 1))
done
grep -q "^demo_done frames=8 rx_data=8 rd_bitmap=0x0 " "$OUT" || {
    echo "FAIL: the run did not end with 8 frames delivered and a drained ring"; exit 1; }
grep -q "^demo_done .* drainerr=0 notready=0 " "$OUT" || {
    echo "FAIL: the ring drain hit errors or unready slots"; exit 1; }
# The W12/W13 health set must be untouched by aggregation. `stranded=0/0` is
# both variants (aligned / desynced); a batch read that walked the ring wrong
# would show up here long before it showed up as a missing frame.
grep -q "^demo_done .* stranded=0/0 " "$OUT" || {
    echo "FAIL: the safety net had to recover an upload — aggregation is not allowed to"
    echo "      make the W12/W13 lost-interrupt picture worse"; exit 1; }

# HALF TWO: the win, from the driver's own counters.
LINE=$(grep "^demo_done " "$OUT" | head -1)
C53RX=$(echo "$LINE" | sed -n 's/.* c53rx=\([0-9]*\) .*/\1/p')
BATCH=$(echo "$LINE" | sed -n 's/.* rxaggr=\([0-9]*\)\/[0-9]*.*/\1/p')
SLOTS=$(echo "$LINE" | sed -n 's/.* rxaggr=[0-9]*\/\([0-9]*\).*/\1/p')
for v in "$C53RX" "$BATCH" "$SLOTS"; do
    [ -n "$v" ] || { echo "FAIL: could not parse the aggregation counters from demo_done"; exit 1; }
done
echo "RX aggregation: $SLOTS slots in $BATCH multi-slot CMD53(s); $C53RX RX data CMD53s for 8 frames"
[ "$C53RX" -le 4 ] || {
    echo "FAIL: 8 frames cost $C53RX RX data CMD53s (want <= 4)"
    echo "      One command per frame is the pre-W16 behaviour and the whole thing W16"
    echo "      exists to remove. Either readRingBatch() is not batching, or the model"
    echo "      is refusing the aggregated address and the driver is falling back."
    exit 1; }
[ "$SLOTS" -ge 6 ] || {
    echo "FAIL: only $SLOTS slots were carried by multi-slot CMD53s (want >= 6)"
    echo "      This counter is incremented ONLY by a CMD53 that covered more than one"
    echo "      ring slot, so a low value means the frames arrived one command at a"
    echo "      time — whatever c53rx happens to read."
    exit 1; }
[ "$BATCH" -ge 1 ] || { echo "FAIL: no aggregated read happened at all"; exit 1; }
# The model logs a guest error for every illegal thing a host can do to the
# aggregated path: a start slot out of range, a run covering a slot the card is
# not offering, a run overrunning the staging buffer. None may fire.
if [ -f "$DBG" ] && grep -q "iw416-sdio" "$DBG"; then
    echo "FAIL: the card model reported guest errors — the host's aggregated access is"
    echo "      malformed even though the frames arrived:"
    grep "iw416-sdio" "$DBG" | head -5
    exit 1
fi
echo "PASS: 8 uploads on ring slots 0-7 delivered in order in $C53RX RX data CMD53"
echo "      ($SLOTS slots aggregated) — W16 RX multiport aggregation"
