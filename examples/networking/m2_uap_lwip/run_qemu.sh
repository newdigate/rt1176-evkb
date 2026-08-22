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
# shell loop that used a RELATIVE `cd` per gate.  It then passed ~43 consecutive
# runs -- 19 more in relative-path loops, 24 in absolute-path loops -- and has
# never once failed when run on its own.  The most likely explanation is the
# LOOP, not the gate: the harness running these commands resets the working
# directory between invocations, so a relative `cd` can fail and produce a
# non-zero status with the gate never running at all.  That is a hypothesis, not
# a finding: it was not reproduced deliberately, so it is not proven.
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
