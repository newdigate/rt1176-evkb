#!/bin/sh
# run_qemu_stranded.sh — REGRESSION GATE for W12, the stranded upload.
#
# WHAT THIS PROVES
#   The card model is run with `suppress-updl=on`: it queues uploads on the
#   ring and publishes their lengths, but NEVER raises HOST_INT_UP_LD for them.
#   That is a measured firmware behaviour (W13: `stranded` survived with every
#   host interrupt reader verified accumulating), and combined with
#   HOST_INT_STATUS being clear-on-read it is the whole of the W12 fault: the
#   frame is on the ring, nothing ever tells the host, and RX is dead until
#   reflash. All 8 frames must still be delivered.
#
#   AND the recovery counter must be non-zero. That assertion is the stronger
#   half and the reason this gate is not just "the frames arrived": a run where
#   they arrive by luck (some other path happening to drain the ring) is
#   indistinguishable from a working safety net by frame count alone.
#   rxStrandedRecovered() only counts a drain the NET initiated that actually
#   pulled a packet off the ring, so a non-zero value is proof the net did the
#   work. It is asserted as "> 0", not as an exact count: how many frames one
#   net check happens to batch is a scheduling detail, and pinning it would
#   make the gate fragile without making it stronger.
#
#   The bug this exists for: `serviceLink()` used to drain only on its OWN
#   sighting of the upload bit while four other paths — readHostResp's 1 ms
#   command-wait poll and three diagnostics — read the same clear-on-read
#   register and threw the bit away. Fixed in two layers (M2Radio 84c2520 +
#   W13): a sticky `m_intPending` accumulator at every read site, and a bitmap
#   safety net that asks RD_BITMAP directly every 64 quiet passes and probes
#   the candidate slot's own RD_LEN before entering the drain.
#
#   DEMONSTRATED, not asserted on faith (2026-08-19), and the demonstration
#   corrected an assumption — read this before deciding what a red here means:
#     * Driver reverted to the FULL pre-W12 shape (serviceLink branching on its
#       own fresh HOST_INT_STATUS read, safety net removed): RED at the first
#       frame, summary
#         demo_done frames=0 rx_data=0 rd_bitmap=0xFF ... stranded=0/0
#       — all eight uploads sitting on ring slots 0-7 unread, RX dead. That is
#       the silicon freeze signature W12 root-caused, reproduced in QEMU.
#     * Layer 2 (the net) removed, layer 1 (the sticky accumulator) LEFT IN:
#       still RED, identically. The net is what this gate covers.
#     * Layer 1 reverted (`st = fresh`), layer 2 left in: **GREEN**. It has to
#       be: `suppress-updl` means the card never raises the bit at all, so
#       there is nothing for an accumulator to accumulate. Which is why the
#       "does NOT prove" note below is worded the way it is.
#   The driver was then restored (M2Radio 7aec26b, clean tree) and this gate
#   went green again.
#
# WHAT THIS DOES *NOT* PROVE
#   * NOT firmware download, NOT Wi-Fi, NOT silicon — same three caveats as
#     run_qemu_ring.sh, for the same reasons. `fw-preboot=on` is the model's
#     own admitted fiction (its NOTE 7) and the only way a blob-free tree can
#     reach the rings at all.
#   * NOT the OTHER half of the W12 fault. `suppress-updl` models the card
#     never raising the bit; the historic bug ALSO had the host losing a bit
#     the card did raise, to a non-draining reader. This model cannot express
#     that — nothing here can make one of the driver's five readers run at the
#     right instant — so the sticky accumulator (layer 1) is NOT what this gate
#     covers, as the third bullet above MEASURES rather than assumes. It covers
#     layer 2, the net, which is what makes either variant survivable. Do not
#     read a green here as "the accumulator is fine".
#   * NOT the 32-slot ring. Injection deliberately uses slots 0..7, the LOWER
#     half, so that a 16-bit-bitmap regression cannot fail this gate too and
#     blur which bug is which. run_qemu_ring.sh owns slots 16..31. Measured the
#     same day: with readRdBitmap32() reverted to 16 bits, run_qemu_ring.sh
#     went red and this gate stayed GREEN. The separation is real, not hoped for.
#   * NOT the stale-bit hazard. The model's `stale-rd-bits` (a set bitmap bit
#     with RD_LEN 0 — W12's ~6100 fruitless resyncs) is left OFF here; that is
#     a third behaviour and mixing it in would make a red ambiguous.
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
OUT=$(gate_capture_path "$DIR" stranded.uart)
DBG=$(gate_capture_path "$DIR" stranded.dbg)
rm -f "$OUT" "$DBG"
"$QEMU" $(gate_qemu_machine) -machine m2-wifi=on \
    -global iw416-sdio.fw-preboot=on \
    -global iw416-sdio.suppress-updl=on \
    -global iw416-sdio.inject-count=8 \
    -global iw416-sdio.inject-slot-first=0 \
    -global iw416-sdio.inject-slot-last=7 \
    -global iw416-sdio.inject-period-ms=300 \
    -kernel "$ELF" -display none $(gate_console "$OUT") \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
for _ in $(seq 1 160); do
    [ -f "$OUT" ] && grep -q "^demo_done " "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 M.2 RX demo up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
# The same negative run_qemu_ring.sh carries: a plain SD memory card ignores
# CMD5, so without the model attached everything below would be vacuous.
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
# SECOND NEGATIVE, and this one is specific to this gate: bit 0 of int_seen is
# HOST_INT_UP_LD, and int_seen is the union of every status bit the host ever
# saw. 0xC2 = CMD_PORT_DNLD|CMD_PORT_UPLD|DN_LD with UP_LD ABSENT. If the model
# silently ignored `suppress-updl` the bit would be there, the frames would
# arrive down the ordinary path, and a green would mean nothing at all.
grep -q "^demo_done .* int_seen=0xC2 " "$OUT" || {
    echo "FAIL: HOST_INT_UP_LD was seen — suppress-updl did not take effect, so this"
    echo "      run exercised the ordinary interrupt path and proves nothing about the"
    echo "      safety net (expected int_seen=0xC2, i.e. the union WITHOUT bit 0)"
    exit 1; }
# Eight frames, host counter N == card sequence K, slot S == K-1, over the ring's
# lower half. Every one of them arrived with no interrupt ever raised for it.
i=1
while [ $i -le 8 ]; do
    slot=$((i - 1))
    grep -q "^rx_frame $i: len=64 .* seq=$i from_slot=$slot " "$OUT" || {
        echo "FAIL: frame $i (card seq $i, ring slot $slot) was STRANDED — queued by the"
        echo "      card with no HOST_INT_UP_LD and never recovered. This is the W12"
        echo "      signature: RX dead with the frame sitting on the ring."
        exit 1; }
    i=$((i + 1))
done
grep -q "^demo_done frames=8 rx_data=8 rd_bitmap=0x0 " "$OUT" || {
    echo "FAIL: the run did not end with 8 frames delivered and a drained ring"; exit 1; }
grep -q "^demo_done .* drainerr=0 notready=0 " "$OUT" || {
    echo "FAIL: the ring drain hit errors or unready slots"; exit 1; }
# THE ASSERTION THAT MAKES THIS A W12 GATE RATHER THAN A DELIVERY TEST.
# `stranded=A/B` is rxStrandedRecovered()/rxDesyncRecovered(): A counts a drain
# the net started, at the host's own ring slot, that pulled a real packet.
# Non-zero proves the net did the work; zero with 8 frames delivered would mean
# something else moved them and the net is untested.
STR=$(sed -n 's/^demo_done .* stranded=\([0-9]*\)\/[0-9]*.*/\1/p' "$OUT" | head -1)
[ -n "$STR" ] || { echo "FAIL: no stranded= counter in the summary line"; exit 1; }
[ "$STR" -gt 0 ] || {
    echo "FAIL: rxStrandedRecovered()=0 — the 8 frames arrived, but NOT through the"
    echo "      bitmap safety net, so this run does not exercise the W12 fix at all"
    exit 1; }
echo "PASS: 8 uploads recovered with HOST_INT_UP_LD never raised, rxStrandedRecovered()=$STR (W12 regression gate)"
