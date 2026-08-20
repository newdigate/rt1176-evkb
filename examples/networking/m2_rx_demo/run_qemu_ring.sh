#!/bin/sh
# run_qemu_ring.sh — REGRESSION GATE for W8, the 32-slot ring bug.
#
# WHAT THIS PROVES
#   The card model is told to place 16 uploads on ring slots 16..31 — the
#   entire UPPER half of the 32-slot upload ring, one frame per slot — and the
#   host must deliver every one of them, in order, to the application sink.
#   Each `rx_frame N: ... seq=K from_slot=S` line pairs a host-side counter
#   with two values the CARD chose (its own sequence number and the slot it
#   used), so the gate asserts the pairing rather than a count: 16 frames that
#   arrived out of order, duplicated, or from the wrong slots do not pass.
#
#   The bug this exists for: for days the driver read RD_BITMAP as a 16-BIT
#   value against a 32-slot ring, so every upload landing on slots 16-31 was
#   invisible and RX simply stopped. It was invisible on hardware because a
#   quiet link rarely walks that far around the ring. Reintroduce it — read
#   only RD_BITMAP_L/U, or mis-derive the per-slot RD_LEN address
#   (RD_LEN_P0_L + (port << 1)) — and this gate reads ZERO frames and fails.
#   DEMONSTRATED, not asserted on faith (2026-08-19): readRdBitmap32() was
#   temporarily reverted to reading only RD_BITMAP_L/U and this gate went red
#   at the first frame, with the summary reading
#     demo_done frames=0 rx_data=0 rd_bitmap=0x0 ... int_seen=0xC3
#   — int_seen bit 0 set, i.e. the card DID raise the upload interrupt and the
#   host still saw an empty ring. That is the W8 fault exactly. The driver was
#   then restored (M2Radio 7aec26b, clean tree) and the gate went green again.
#
# WHAT THIS DOES *NOT* PROVE
#   * NOT firmware download. The model is run with `fw-preboot=on`, an admitted
#     FICTION (its NOTE 7) — a real IW416 has no flash and always needs a blob,
#     and the blob is NXP-licensed so no gate may depend on it. Everything
#     downstream of the ROM phase is the real post-download code path, but this
#     gate asserts nothing at all about downloading.
#   * NOT Wi-Fi. No scan, no association, no supplicant, no radio. The model
#     has none of those and says so; the frames are injected, not received.
#   * NOT silicon. Nothing the model's header flags as inferred or fictional is
#     asserted here — in particular RxPD's rx_pkt_offset (its NOTE 12, inferred
#     not measured) is never checked, only used, and the model's ring-slot
#     ORDER is its own choice, which is why the gate asserts the card's stated
#     from_slot rather than an order it decided in advance.
#   * NOT the interrupt-loss safety net. That is run_qemu_stranded.sh's job,
#     and the two are kept orthogonal: this gate asserts the upload interrupt
#     WAS raised and seen (int_seen bit 0), so the frames came up the ordinary
#     path and the ring is what is under test.
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
OUT=$(gate_capture_path "$DIR" ring.uart)
DBG=$(gate_capture_path "$DIR" ring.dbg)
rm -f "$OUT" "$DBG"
# `-M ...` from gate-lib (no gate names a machine) plus one more `-machine`
# fragment for the opt-in property; QEMU merges the two into one machine opts
# group. Injection walks 16..31 inclusive, one frame per slot.
"$QEMU" $(gate_qemu_machine) -machine m2-wifi=on \
    -global iw416-sdio.fw-preboot=on \
    -global iw416-sdio.inject-count=16 \
    -global iw416-sdio.inject-slot-first=16 \
    -global iw416-sdio.inject-slot-last=31 \
    -kernel "$ELF" -display none $(gate_console "$OUT") \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# Wait for the line AFTER the one this gate asserts on: `demo_done` is a long
# line and a reap that lands mid-line would fail the field matches below
# against a healthy run.  `irq_mode=` is printed immediately after it.
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
# run_qemu.sh asserts. Every assertion below would then be vacuous, so check it
# first and say why.
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
# `[[:space:]]*$` rather than a bare `$` on end-anchored patterns: Print's
# println() emits CRLF, so a capture line ends "...\r" and a plain `$` never
# matches.
#
# THE RING ASSERTION. Sixteen frames, host counter N == card sequence K, slot
# S == 15+K, walking the whole upper half of the ring. A 16-bit bitmap read
# sees none of these; a mis-derived per-slot RD_LEN address sees the bit but
# reads length 0 and drains nothing.
i=1
while [ $i -le 16 ]; do
    slot=$((15 + i))
    grep -q "^rx_frame $i: len=64 .* seq=$i from_slot=$slot " "$OUT" || {
        echo "FAIL: frame $i (card seq $i, ring slot $slot) never reached the sink"
        echo "      — this is the W8 signature: uploads on slots 16-31 invisible to a"
        echo "        host that reads a 16-bit bitmap against a 32-slot ring"
        exit 1; }
    i=$((i + 1))
done
# The summary, from the driver's own counters. frames= is the sink's count and
# rx_data= is the ring layer's, so they agree only if every packet the ring
# pulled was also delivered. rd_bitmap=0x0 says the ring is fully drained;
# drainerr/notready are the two "gave up mid-drain" counters and must be clean.
grep -q "^demo_done frames=16 rx_data=16 rd_bitmap=0x0 " "$OUT" || {
    echo "FAIL: the run did not end with 16 frames delivered and a drained ring"; exit 1; }
grep -q "^demo_done .* drainerr=0 notready=0 " "$OUT" || {
    echo "FAIL: the ring drain hit errors or unready slots"; exit 1; }
# Orthogonality with run_qemu_stranded.sh, asserted rather than assumed: bit 0
# of int_seen is HOST_INT_UP_LD. 0xC3 = CMD_PORT_DNLD|CMD_PORT_UPLD|DN_LD|UP_LD,
# i.e. the card raised the upload interrupt and the host saw it, so these frames
# came up the ORDINARY path. (The stranded gate asserts 0xC2 — the same union
# with UP_LD absent.) It is a union of everything ever seen, so it is monotonic
# and stable, not a sample.
grep -q "^demo_done .* int_seen=0xC3 " "$OUT" || {
    echo "FAIL: the upload interrupt was never seen — this gate is meant to exercise"
    echo "      the ordinary interrupt-driven path, not the bitmap safety net"; exit 1; }
echo "PASS: all 16 uploads on ring slots 16-31 delivered in order (W8 regression gate)"
