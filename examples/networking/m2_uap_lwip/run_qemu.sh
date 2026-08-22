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
for _ in $(seq 1 40); do
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
grep -q "^hb card=0 bss=0 .* dhcp_ack=0 dhcp_full=0" "$OUT" || {
    echo "FAIL: no heartbeat, or it claimed a card/BSS with neither present"; exit 1; }

# --- vacuity: with no card, NOTHING about an AP may be invented -------------
for T in "^uap_configure=" "^uap_bss_start=" "^uap_hosting " "^uap_netif_up " \
         "^uap_udp_bound" "^uap_udp_first " "^uap_dhcp_up" "^uap_dhcp_ack "; do
    if grep -q "$T" "$OUT"; then
        echo "FAIL: emitted '$T' with no card present -- this example TRANSMITS,"
        echo "      and a build that reaches those lines has started an AP"
        exit 1
    fi
done
echo "PASS: card-absent fallback; no AP was configured, started, or claimed"
