#!/bin/sh
# Card-ABSENT gate for the uAP + lwip example.
#
# QEMU attaches an SD *memory* card, which ignores CMD5, so the correct outcome
# here is the clean fallback -- NOT an AP.  The AP itself is silicon-only: the
# model has no uAP command surface, and even the IW416 model
# (-machine m2-wifi=on) answers SYS_CONFIGURE with its unknown-command error.
# The on-air proof lives in transcript_hw_evkb.txt.
#
# ★ THE VACUITY GUARDS ARE THE POINT OF THIS GATE.  An example whose whole
# purpose is to TRANSMIT must be provably silent when there is no card, and
# "it printed the fallback" does not say that on its own -- the AP lines must
# be ABSENT.  A gate that only checked the happy path would pass just as
# cheerfully on a build that had started beaconing.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/m2_uap_lwip.elf"
OUT=$(gate_capture_path "$DIR" serial.uart)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -d guest_errors -D "$(gate_capture_path "$DIR" serial.dbg)" &
P=$!; gate_pid $P
# Wait for the LAST line of a heartbeat, not the first interesting one: reaping
# mid-line is how a healthy run gets blamed on the firmware (see CLAUDE.md's
# note on the m2_rx_demo [irq] race).
# 30 s, not the 10 s the older gates in this tree use.  Cheap insurance rather
# than a diagnosed fix, and the distinction is worth recording accurately.
#
# What happened: this gate reported failure TWICE early on, both times inside a
# shell loop running several gates.  It then passed ~43 consecutive runs and has
# never once failed when run on its own.
#
# ★ A BETTER EXPLANATION TURNED UP LATER, and it is worth recording because it
# is not about this gate at all.  m2_rx_demo[txaggr] later "failed" the same way
# in a 19-gate loop, passed 3/3 standalone at 17-21 s, and passed when the same
# 19 gates were run in three smaller batches.  The cause was the OUTER command's
# own two-minute limit killing whichever gate was in flight -- the loop, not the
# gate, and not the run.  That fits both of this gate's failures at least as
# well as the relative-`cd` idea previously recorded here, and it is testable
# where that was not.
# Neither is proven for THIS gate specifically, so nothing here is stated as
# fact.  What is certain: run long gate sets in batches, and never diagnose a
# gate from a loop that was itself cut short.
#
# The timeout was raised anyway, because it costs nothing -- the loop exits the
# moment the token appears, so the budget only ever matters when the gate would
# otherwise fail -- and because CLAUDE.md's standing warning is that a
# load-sensitive gate produces FALSE regressions, which in a 111-gate sweep are
# expensive precisely because they look like real breakage in new code.
# If this ever fails again, capture its OUTPUT: it prints a named FAIL for every
# assertion, and both original failures were lost to `>/dev/null`.
for _ in $(seq 1 120); do
    [ -f "$OUT" ] && grep -q "^hb card=0 " "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 M.2 uAP + lwip up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "^sdio_begin=cmd5-no-response" "$OUT" || {
    echo "FAIL: expected the cmd5-no-response fallback"; exit 1; }
grep -q "^uap_lwip_done" "$OUT" || { echo "FAIL: setup never completed"; exit 1; }
# The health line must be emitted even with no card: a soak counter that only
# appears once things are working is one nobody checks when they are not.
grep -q "^health stranded=0 desync=0 split=0 dropped=0 seqmm=0 pswake=0 rx_bss0=0 unrouted=0 " "$OUT" || {
    echo "FAIL: health line missing, or a counter was non-zero with no card"; exit 1; }
grep -q "^hb card=0 bss=0 .* dhcp_ack=0 dhcp_full=0 dhcp_bcast=0 sta=? joins=0 leaves=0 " "$OUT" || {
    echo "FAIL: no heartbeat, or it claimed a card/BSS with neither present"; exit 1; }

# --- vacuity: with no card, NOTHING about an AP may be invented -------------
for T in "^uap_configure=" "^uap_bss_start=" "^uap_hosting " "^uap_netif_up " \
         "^uap_udp_bound" "^uap_udp_first " "^uap_dhcp_up" "^uap_dhcp_ack " \
         "^uap_membership "; do
    if grep -q "$T" "$OUT"; then
        echo "FAIL: emitted '$T' with no card present -- this example TRANSMITS,"
        echo "      and a build that reaches those lines has started an AP"
        exit 1
    fi
done
echo "PASS: card-absent fallback; no AP was configured, started, or claimed"
