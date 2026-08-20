#!/bin/sh
# run_qemu_irq.sh — GATE for W15, interrupt-driven SDIO service (DAT1).
#
# WHAT THIS PROVES
#   The card model injects frames at a steady 250 ms across BOTH of the demo's
#   service windows, which differ in exactly one thing — the second one runs
#   with Iw416::setInterruptMode(true). Two facts then have to hold together:
#
#     1. THE FRAMES STILL ARRIVE, in order, paired with the slot the CARD chose
#        (`seq=K from_slot=S`), same oracle run_qemu_ring.sh uses. Interrupt
#        mode that delivers fewer frames, or the same frames later or jumbled,
#        is not a win.
#     2. THE HOST STOPPED POLLING FOR THEM. cmd52PollsSvc() per received frame
#        must be at least 5x lower in the interrupt window than in the polled
#        one. That counter (W11) is the driver's own service-side CMD52 count,
#        and dividing it by the frames the SAME window delivered is what makes
#        the comparison an argument rather than an anecdote: both windows are
#        6 s of the same image against the same card at the same injection
#        cadence, so a driver that merely polls FASTER moves both numbers and
#        the ratio does not budge.
#
#   MEASURED (2026-08-20, first passing run, and stable to ±2% over four runs):
#     phase=polled frames=34 c52svc=8961   -> 263 CMD52 per frame
#     phase=irq    frames=34 c52svc=937    ->  27 CMD52 per frame   = 9.6x
#   Idle rate, which is the design doc's own success criterion, falls the same
#   way: ~1500 CMD52/s polled to ~110/s, i.e. an order of magnitude. The
#   threshold below is set at 5x — a little over half the measured margin — so
#   the gate reports a REGRESSION rather than scheduling jitter.
#
#   WHY THE FLOOR IS NOT ZERO, and why that is deliberate: the W12/W13 RD-bitmap
#   safety net still runs every 64 quiet serviceLink passes (~45 ms here) at
#   4 CMD52 a check, plus one HOST_INT_STATUS read folded into the same tick.
#   That is the whole of the remaining ~110/s. It is NOT overhead to be
#   optimised away — W13 measured this firmware stranding uploads with no
#   interrupt ~3 times per 80 blasts on silicon, so a build that trusted DAT1
#   completely would reintroduce the W12 fault class. See run_qemu_stranded.sh.
#
# THE NEGATIVE THAT STOPS A SILENT NO-OP
#   `cardints=` is Iw416::cardInts(), incremented only when serviceLink takes
#   delivery of a DAT1 assertion the USDHC1 ISR flagged. It is asserted > 0 in
#   the interrupt window and == 0 in the polled one. A build where interrupt
#   mode silently did not engage — the mode flag never set, INT_SIGNAL_EN[CINT]
#   never written, the vector never attached, or a QEMU without the card-
#   interrupt plumbing — delivers every frame perfectly and reads 0 here, which
#   is exactly the outcome that must not pass. The ratio alone could not catch
#   it: a driver could satisfy the ratio by polling more slowly and be worse in
#   every way.
#
# WHAT THIS DOES *NOT* PROVE
#   * NOT firmware download, NOT Wi-Fi, NOT silicon — the same three caveats as
#     the other two gates here, for the same reasons (`fw-preboot=on` is the
#     model's own admitted fiction, its NOTE 7).
#   * ★ NOT that the CARD behaves like this. DAT1 is the ONE behaviour in
#     hw/sd/iw416-sdio.c that is not anchored to a capture — its MODELLING
#     NOTE 10 says so outright. The RT1176 uSDHC's card interrupt has never
#     been exercised on a MIMXRT1170-EVKB, so a green here proves the driver
#     matches the SDIO specification as this tree reads it, not that it matches
#     the MAYA-W161. That is why Iw416's interrupt mode defaults OFF and why
#     this example opts in explicitly. If silicon disagrees, silicon wins and
#     the model changes — never the reverse.
#   * NOT the 32-slot ring. Injection is confined to slots 0..15, the LOWER
#     half, exactly as run_qemu_stranded.sh does and for the same reason: a
#     16-bit-bitmap regression must fail run_qemu_ring.sh and nothing else, so
#     a red here always means the interrupt path. (The `ring=…/…/4` resyncs in
#     the summary are the expected artefact of walking a 16-slot injection
#     window inside a 32-slot host ring, not a fault; nothing asserts them.)
#   * NOT the safety net. It is asserted IDLE here (`stranded=0/0`), which is
#     the orthogonality claim: these frames came up the interrupt path, not out
#     of the net. run_qemu_stranded.sh is where the net is under test.
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
OUT=$(gate_capture_path "$DIR" irq.uart)
DBG=$(gate_capture_path "$DIR" irq.dbg)
rm -f "$OUT" "$DBG"
# 70 injections at 250 ms is ~17 s of virtual time, which straddles both of the
# demo's 6 s service windows and lands ~34 frames in each. Equal frame counts
# are what make the per-frame division a like-for-like comparison; the last two
# injections fall past the end of the run and nothing asserts them.
"$QEMU" $(gate_qemu_machine) -machine m2-wifi=on \
    -global iw416-sdio.fw-preboot=on \
    -global iw416-sdio.inject-count=70 \
    -global iw416-sdio.inject-period-ms=250 \
    -global iw416-sdio.inject-slot-first=0 \
    -global iw416-sdio.inject-slot-last=15 \
    -kernel "$ELF" -display none $(gate_console "$OUT") \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# Two 6 s windows plus boot: ~19 s wall on the reference machine. 240 x 0.25 s
# leaves generous headroom without approaching the runner's 120 s per-gate cap.
for _ in $(seq 1 240); do
    [ -f "$OUT" ] && grep -q "^irq_done " "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 M.2 RX demo up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
# The same negative the other two gates carry: without the model attached the
# board gets a plain SD memory card, which ignores CMD5, and everything below
# would be vacuous.
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
grep -q "^demo_done " "$OUT" || {
    echo "FAIL: the polled window never finished — no baseline to compare against"; exit 1; }
# Both halves of the switch, reported by the two objects that own them:
# interruptMode() is the driver's and cardIntEnabled() is the uSDHC's. Asserting
# only one could not tell "the mode was requested" from "the mode engaged".
grep -q "^irq_mode=1 host_cardint=1[[:space:]]*$" "$OUT" || {
    echo "FAIL: the interrupt window did not start with BOTH the driver's mode flag and"
    echo "      the uSDHC's CINT signalling enabled (expected 'irq_mode=1 host_cardint=1')"
    exit 1; }

# --- the A/B ---------------------------------------------------------------
# `phase=polled ms=N frames=A c52svc=PA cardints=0`
# `phase=irq    ms=N frames=B c52svc=PB cardints=CI`
PA_LINE=$(grep "^phase=polled " "$OUT" | head -1)
IRQ_LINE=$(grep "^phase=irq " "$OUT" | head -1)
[ -n "$PA_LINE" ] && [ -n "$IRQ_LINE" ] || {
    echo "FAIL: the run did not print both phase= summary lines"; exit 1; }
A=$( echo "$PA_LINE"  | sed -n 's/.* frames=\([0-9]*\) .*/\1/p')
PA=$(echo "$PA_LINE"  | sed -n 's/.* c52svc=\([0-9]*\) .*/\1/p')
B=$( echo "$IRQ_LINE" | sed -n 's/.* frames=\([0-9]*\) .*/\1/p')
PB=$(echo "$IRQ_LINE" | sed -n 's/.* c52svc=\([0-9]*\) .*/\1/p')
CI=$(echo "$IRQ_LINE" | sed -n 's/.* cardints=\([0-9]*\).*/\1/p')
for v in "$A" "$PA" "$B" "$PB" "$CI"; do
    [ -n "$v" ] || { echo "FAIL: could not parse the phase= counters"; exit 1; }
done
# The polled window must really have been polled: cardInts() is reset per
# firmware life and nothing enables signalling until the switch, so a non-zero
# here means the windows are not the controlled comparison this gate claims.
echo "$PA_LINE" | grep -q " cardints=0$\| cardints=0[[:space:]]" || {
    echo "FAIL: the POLLED window reports card interrupts — the two windows are not a"
    echo "      controlled A/B and the ratio below means nothing"; exit 1; }
[ "$A" -gt 0 ] && [ "$B" -gt 0 ] || {
    echo "FAIL: one of the windows received no frames at all (polled=$A irq=$B) — with"
    echo "      nothing to divide by there is no measurement here"; exit 1; }
[ "$B" -ge 20 ] || {
    echo "FAIL: only $B frames arrived in the interrupt window (want >= 20). Either the"
    echo "      injection schedule no longer straddles both windows, or interrupt-driven"
    echo "      RX is dropping frames the polled path delivered"; exit 1; }

# THE NEGATIVE. See the header: a build that never actually engaged the card
# interrupt delivers every frame and reads 0 here.
[ "$CI" -gt 0 ] || {
    echo "FAIL: cardInts()=0 — not one DAT1 assertion was serviced, so the frames in the"
    echo "      'interrupt' window arrived by polling or via the bitmap safety net and"
    echo "      this run proves nothing about interrupt-driven service at all"
    exit 1; }
# ...and the interrupt must be carrying the traffic, not incidental to it. One
# assertion per frame is what the model produces (measured CI == B exactly); a
# quarter of that is the floor, which still cannot be reached by a net-driven or
# polled run.
[ $((CI * 4)) -ge "$B" ] || {
    echo "FAIL: only $CI DAT1 assertions for $B frames — the card interrupt is not what"
    echo "      is delivering RX; something else (the safety net?) is doing the work"
    exit 1; }

# THE WIN. Cross-multiplied so no division rounds anything away:
#   PA/A >= 5 * (PB/B)   <=>   PA*B >= 5*PB*A
PPF_A=$((PA * 100 / A))
PPF_B=$((PB * 100 / B))
echo "service CMD52 per frame: polled=$((PPF_A / 100)).$((PPF_A % 100 / 10))  irq=$((PPF_B / 100)).$((PPF_B % 100 / 10))"
[ $((PA * B)) -ge $((5 * PB * A)) ] || {
    echo "FAIL: the interrupt window did not cut service polling by 5x"
    echo "      polled: $PA CMD52 for $A frames; irq: $PB CMD52 for $B frames"
    echo "      This is the whole point of W15 — interrupt mode engaged (cardints=$CI)"
    echo "      but serviceLink is still polling HOST_INT_STATUS on quiet passes."
    exit 1; }

# The interrupt window's own summary. rd_bitmap=0x0 says the ring is drained;
# drainerr/notready are the two "gave up mid-drain" counters.
grep -q "^irq_done .* rd_bitmap=0x0 " "$OUT" || {
    echo "FAIL: the interrupt window ended with the ring not drained"; exit 1; }
grep -q "^irq_done .* drainerr=0 notready=0 " "$OUT" || {
    echo "FAIL: the ring drain hit errors or unready slots"; exit 1; }
# ORTHOGONALITY WITH run_qemu_stranded.sh, asserted rather than assumed. The
# W12/W13 safety net is still armed and still ticking (that is what the residual
# ~110 CMD52/s above IS) — but it initiated nothing, so these frames came up the
# interrupt path. A red here with everything else green would mean interrupt
# mode is announcing uploads late and the net is quietly covering for it.
grep -q "^irq_done .* stranded=0/0 " "$OUT" || {
    echo "FAIL: the safety net recovered an upload during the interrupt window — the"
    echo "      card interrupt is not announcing uploads promptly and the net is"
    echo "      covering for it (expected stranded=0/0)"; exit 1; }
# Bit 0 of int_seen is HOST_INT_UP_LD: the ordinary upload path was live, as in
# run_qemu_ring.sh. (run_qemu_stranded.sh asserts 0xC2, the same union without
# it.) Union of everything ever seen, so monotonic and stable, not a sample.
grep -q "^irq_done .* int_seen=0xC3 " "$OUT" || {
    echo "FAIL: HOST_INT_UP_LD was never seen — this gate exercises the ordinary
      interrupt-driven upload path"; exit 1; }

# EVERY frame, in order, paired with the slot the CARD chose. Injection walks
# slots 0..15 inclusive and wraps, so frame N came from slot (N-1) mod 16 — a
# value this image cannot know in advance. The count is taken from the run
# rather than pinned, so timing shifts change how many are checked, never
# whether the pairing held.
TOT=$(sed -n 's/^irq_done frames=\([0-9]*\) .*/\1/p' "$OUT" | head -1)
[ -n "$TOT" ] && [ "$TOT" -ge 40 ] || {
    echo "FAIL: irq_done reported ${TOT:-no} frames over the whole run (want >= 40)"; exit 1; }
i=1
while [ $i -le "$TOT" ]; do
    slot=$(( (i - 1) % 16 ))
    grep -q "^rx_frame $i: len=64 .* seq=$i from_slot=$slot " "$OUT" || {
        echo "FAIL: frame $i (card seq $i, ring slot $slot) never reached the sink, or"
        echo "      arrived out of order — interrupt-driven RX must deliver exactly what"
        echo "      the polled path did"
        exit 1; }
    i=$((i + 1))
done
echo "PASS: $TOT frames delivered in order; interrupt window cut service CMD52 per frame"
echo "      from $((PPF_A / 100)) to $((PPF_B / 100)) ($CI DAT1 assertions serviced) — W15 interrupt-driven SDIO"
