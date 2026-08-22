#!/bin/sh
# The FAULT 1 REGRESSION GATE: a populated SYS_CONFIGURE is accepted, a MINIMAL
# one kills the command port, and nothing answers afterwards.
#
# This is the gate the uAP work most needed and could not have until the model
# grew an AP surface (qemu2 `uap=on`).  What it protects is a rule that is easy
# to break and expensive to discover:
#
#   THERE IS NO HARMLESS PROBE OF SYS_CONFIGURE.  A config GET is exactly the
#   shape that wedges, so any code that "just queries" the uAP configuration
#   kills the card's command port for the rest of its firmware life, recoverable
#   only by a card reset.  Silicon reproduced that 5/5 across three minimal
#   shapes; a driver that reintroduces one fails HERE instead of on a bench.
#
# ★ Not a substitute for the silicon captures.  The model wedges on "no SSID
#   TLV", which is a rule that FITS every capture rather than the firmware's
#   actual predicate — see MODELLING NOTE 20's where-this-could-be-lying list.
#   A green run here means the driver still sends a populated configuration; it
#   does not mean an arbitrary configuration would be accepted on silicon.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/m2_uap_probe.elf"
OUT=$(gate_capture_path "$DIR" uap.uart)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -machine m2-wifi=on \
    -global iw416-sdio.fw-preboot=on -global iw416-sdio.uap=on \
    -kernel "$ELF" -display none $(gate_console "$OUT") \
    -d guest_errors -D "$(gate_capture_path "$DIR" uap.dbg)" &
P=$!; gate_pid $P
# ★ Wait for the VERDICT, not for the heartbeat.  Once the port is wedged the
# probe spends ~90 s retrying before it reports uap_recover=never and reaches
# loop(), and qrun caps a gate's QEMU at 60 s (QRUN_TIMEOUT) -- so waiting for
# `hb` here would mean waiting out the full 225 s of the poll loop for a run
# QEMU had already been killed under.  Measured, not guessed: that is exactly
# what the first version of this gate did.
#
# Everything this gate asserts is printed BEFORE that wait: the wedge shows up
# in the matrix rows and the trailing controls, which is what a regression gate
# needs.  `uap_recover=never` is deliberately NOT asserted here -- it costs 90 s
# of sweep time to re-prove something the silicon transcript already establishes
# (30 retries, ~90 s, never recovered), and the rows above already say the port
# stayed dead for the rest of the matrix.
for _ in $(seq 1 200); do
    [ -f "$OUT" ] && grep -q "^uap_verdict=" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

grep -q "RT1176 M.2 uAP probe up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }

# --- a POPULATED configuration is ACCEPTED, on both interfaces --------------
for B in 0 1; do
    grep -q "^uap_probe cmd=0x00B0 name=SYSCFG.fullopen bss=$B st=ok .* result=0x0000 " "$OUT" || {
        echo "FAIL: the full open-AP configuration was not accepted on bss=$B."
        echo "      Either uapConfigure() stopped sending an SSID TLV, or the"
        echo "      model's uap surface is absent (needs -global iw416-sdio.uap=on)."
        exit 1; }
done
# ...and the port SURVIVES it, which is what makes the row evidence.
for B in 0 1; do
    grep -q "^uap_probe cmd=0x0003 name=HWSPEC.ctl+.e5 bss=$B st=ok " "$OUT" || {
        echo "FAIL: the command port did not survive a POPULATED SYS_CONFIGURE"; exit 1; }
done

# --- a MINIMAL configuration WEDGES the port -------------------------------
# Both minimal rows must time out.  The first is the one that does the damage;
# the second times out because the port is already dead, and asserting both is
# what says the wedge PERSISTS rather than being a single dropped reply.
for N in SYSCFG.chantlv SYSCFG.bare; do
    for B in 0 1; do
        grep -q "^uap_probe cmd=0x00B0 name=$N bss=$B st=cmd-timeout " "$OUT" || {
            echo "FAIL: $N (a MINIMAL SYS_CONFIGURE) was answered on bss=$B."
            echo "      Silicon wedges the command port on this request and the"
            echo "      model must too -- if it answers, a driver that sends a"
            echo "      config GET looks fine here and dies on the bench."
            exit 1; }
    done
done
# Everything after the wedge is dead, including the trailing controls.
for N in HWSPEC.ctl+.f HWSPEC.ctl+.g; do
    grep -q "^uap_probe cmd=0x0003 name=$N bss=0 st=cmd-timeout " "$OUT" || {
        echo "FAIL: $N answered after the wedge -- the port recovered on its own,"
        echo "      which silicon never does (30 retries over ~90 s)"; exit 1; }
done

# --- and the probe's own verdict -------------------------------------------
# bracketed=6 distinct=6 unbracketed=4 is the SILICON tally, byte for byte:
# SYS_INFO, STA_LIST and SYSCFG.fullopen bracketed on both interfaces; the four
# minimal-SYSCFG cells unbracketed because the port died under them.
grep -q "^uap_tally bracketed=6 distinct_from_neg=6 unbracketed=4[[:space:]]*$" "$OUT" || {
    echo "FAIL: wrong tally -- this should match the silicon run exactly"; exit 1; }
grep -q "^uap_verdict=SUPPORTED[[:space:]]*$" "$OUT" || {
    echo "FAIL: the AP command family was not found against a model that has it"
    exit 1; }
echo "PASS: populated SYS_CONFIGURE accepted, minimal one wedged the port and it stayed wedged"
