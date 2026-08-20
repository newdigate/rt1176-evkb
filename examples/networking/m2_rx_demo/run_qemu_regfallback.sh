#!/bin/sh
# run_qemu_regfallback.sh — GATE for W16's one unverified assumption, and for
# the driver's answer to it being wrong.
#
# THE ASSUMPTION
#   W16 stopped polling the card's status and bitmap registers with CMD52 and
#   started reading 196 bytes of the register file in ONE byte-mode CMD53 at
#   function 1 address 0. That is exactly what NXP's own driver does
#   (wlan_interrupt(), mcuxsdk wifi_nxp) — including passing flags = 0, i.e.
#   OP Code 0, which holds the CMD53 address FIXED. On an ordinary SDIO
#   function that means "read this one register 196 times". We believe this
#   card streams the register file instead, because NXP's driver plainly
#   depends on it. NOTHING IN THIS TREE HAS MEASURED IT.
#
# WHY IT NEEDED A GATE RATHER THAN A COMMENT
#   Because getting it wrong does not look like an error. Register 0 is
#   HOST_POWER_UP and reads back 0, so a card that obeys OP Code 0 literally
#   hands the host 196 zero bytes: no interrupt bits, both ring bitmaps empty,
#   all 32 slot lengths zero. That is BYTE-IDENTICAL to a healthy idle card.
#   RX would go permanently silent — the W12/W13 safety net sees an empty
#   bitmap and never fires — TX would time out on an empty wr-bitmap, and the
#   read would still be consuming the card's real clear-on-read interrupt
#   status underneath the whole time. A board would present as "associated,
#   no traffic", with every health counter reading zero.
#
# WHAT THIS GATE DOES
#   Runs the SAME image against the card model with `reg-port-literal=on`, the
#   model's deliberate implementation of the other reading (its NOTE 16), and
#   asserts the driver notices and survives:
#
#     mpregs=0    — Iw416::mpRegsUsable() went false: the first snapshot of the
#                   firmware life failed its check against CARD_STATUS (0x5C),
#                   a register begin() has already read by CMD52, so the two
#                   TRANSPORTS were compared against each other rather than
#                   against a constant this driver invented.
#     c53regs=1/0 — exactly one register-port read ever happened. The port is
#                   abandoned for the rest of the firmware life, not retried
#                   per pass.
#     8 frames    — and the link still WORKS, over the pre-W16 CMD52 path.
#
#   MEASURED (2026-08-20):
#     demo_done frames=8 rx_data=8 rd_bitmap=0x0 ... c52svc=56503 c53regs=1/0
#               c53rx=1 rxaggr=1/8 mpregs=0 rej=0x0 mperr=0 split=0
#   Note `rej=0x0`: the CARD_STATUS byte that came back was 0x00 — register 0,
#   read 196 times — which is the predicted signature exactly. And note
#   `rxaggr=1/8`: RX aggregation still worked, because the fallback fills the
#   same snapshot struct by CMD52, so only the transport degrades, not the
#   feature. c52svc jumping to ~56k against c53regs=1 is the cost of that
#   degradation, and is what makes it visible in a soak rather than silent.
#
# DEMONSTRATED TO FAIL, not asserted on faith (2026-08-20). The sanity check in
# readMpRegs() was disabled (`if (false && !m_mpRegsChecked)`) — leaving the
# fallback code itself intact, so this is a test of the DETECTION, not of the
# fallback — and this gate went red exactly as the argument above predicts:
#
#     tx=cmd-timeout wr_bitmap=0x0 tx_port_now=0
#     demo_done frames=0 rx_data=0 rd_bitmap=0xFF wr_bitmap=0x0 ring=0/0/0
#               stranded=0/0 drainerr=0 notready=0 int_seen=0xC2 tx=0 c53=0
#               c53regs=7676/204 c53rx=0 rxaggr=0/0 mpregs=1
#     FAIL: mpRegsUsable() still reads 1 — the driver did NOT notice ...
#
#   Read that demo_done line, because it is the whole argument for this gate
#   existing. RX dead (frames=0), TX dead (wr_bitmap=0x0, tx=cmd-timeout), and
#   EVERY HEALTH COUNTER CLEAN: stranded=0/0, drainerr=0, notready=0, ring
#   still at 0/0/0. Nothing anywhere says "broken". The one contradiction in
#   the line is `rd_bitmap=0xFF` — eight uploads waiting — and that value comes
#   from probeRdBitmap(), which reads the bitmap by CMD52 and therefore sees
#   the truth the register port was hiding. A bench session would have had that
#   single byte to go on. The check was restored and the gate went green again.
#
# WHAT THIS DOES *NOT* PROVE
#   * NOT which reading is RIGHT. Both are modelled; only silicon can say. What
#     this proves is that the driver survives EITHER, and says which one it got.
#   * NOT firmware download (`fw-preboot=on`, the model's NOTE 7 fiction), NOT
#     Wi-Fi, NOT silicon.
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
OUT=$(gate_capture_path "$DIR" regfallback.uart)
DBG=$(gate_capture_path "$DIR" regfallback.dbg)
rm -f "$OUT" "$DBG"
"$QEMU" $(gate_qemu_machine) -machine m2-wifi=on \
    -global iw416-sdio.fw-preboot=on \
    -global iw416-sdio.reg-port-literal=on \
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
if grep -q "^sdio_begin=cmd5-no-response" "$OUT"; then
    echo "FAIL: the card-absent fallback ran — the iw416-sdio model did not attach"
    echo "      (a QEMU without reg-port-literal REJECTS the property outright, which"
    echo "       shows up as an empty capture, not as this line)"
    exit 1
fi
grep -q "^sdio_begin=ok " "$OUT" || { echo "FAIL: SDIO enumeration did not succeed"; exit 1; }
grep -q "^demo_ready[[:space:]]*$" "$OUT" || {
    echo "FAIL: never reached demo_ready — the command port did not complete"; exit 1; }

LINE=$(grep "^demo_done " "$OUT" | head -1)
[ -n "$LINE" ] || { echo "FAIL: the run never reached demo_done"; exit 1; }
OK=$(   echo "$LINE" | sed -n 's/.* mpregs=\([0-9]*\) .*/\1/p')
REGS=$( echo "$LINE" | sed -n 's/.* c53regs=\([0-9]*\)\/[0-9]*.*/\1/p')
FRAMES=$(echo "$LINE" | sed -n 's/^demo_done frames=\([0-9]*\) .*/\1/p')
for v in "$OK" "$REGS" "$FRAMES"; do
    [ -n "$v" ] || { echo "FAIL: could not parse the register-port health counters"; exit 1; }
done

# THE DETECTION.
[ "$OK" = "0" ] || {
    echo "FAIL: mpRegsUsable() still reads 1 — the driver did NOT notice that the card"
    echo "      took OP Code 0 literally. Against a card that really behaves this way it"
    echo "      is now reading an all-zero snapshot and calling it a healthy idle link:"
    echo "      RX silent forever, every health counter clean, nothing to point at."
    exit 1; }
[ "$REGS" -le 2 ] || {
    echo "FAIL: $REGS register-port reads happened (want <= 2) — the port was rejected"
    echo "      but is still being retried, so the driver is paying for both transports"
    echo "      AND eating the clear-on-read interrupt status on every retry"
    exit 1; }
# THE SURVIVAL. Detection that ends the link is not a fallback.
[ "$FRAMES" -eq 8 ] || {
    echo "FAIL: only $FRAMES of 8 injected frames reached the sink — the driver spotted"
    echo "      the bad register port and then failed to keep the link running on the"
    echo "      CMD52 path it fell back to"
    exit 1; }
i=1
while [ $i -le 8 ]; do
    slot=$((i - 1))
    grep -q "^rx_frame $i: len=64 .* seq=$i from_slot=$slot " "$OUT" || {
        echo "FAIL: frame $i (card seq $i, ring slot $slot) never reached the sink over"
        echo "      the CMD52 fallback"
        exit 1; }
    i=$((i + 1))
done
grep -q "^demo_done .* stranded=0/0 " "$OUT" || {
    echo "FAIL: the safety net had to recover an upload — the fallback path is losing"
    echo "      interrupts the register-port path did not"; exit 1; }
grep -q "^demo_done .* drainerr=0 notready=0 " "$OUT" || {
    echo "FAIL: the fallback path hit drain errors or unready slots"; exit 1; }
echo "PASS: the card took OP Code 0 literally, the driver caught it (mpregs=0 after"
echo "      $REGS register-port read) and delivered all 8 frames over the CMD52 fallback"
