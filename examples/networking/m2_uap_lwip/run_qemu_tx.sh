#!/bin/sh
# TX over the uAP BSS: a frame sent on the uAP netif must go out ADDRESSED to
# the uAP interface, not to the station's.
#
# This is the last piece of the uAP driver that was silicon-only.  On the bench
# it was proven by a real client answering an ARP request; here the model's
# tx-loopback echoes each frame back ON THE INTERFACE IT WAS SENT ON, so the
# tag that returns is the tag that went out.
#
# ★ WHY A ROUND TRIP AND NOT A STATUS CODE.  sendDataFrameBss() returning OK
#   proves the card accepted a buffer and nothing more -- not that the frame
#   left, and certainly not which interface it claimed.  Nothing else a host can
#   observe distinguishes "addressed the uAP" from "addressed the station and
#   got lucky", because with one client on one BSS both look identical until
#   something reads the tag.
#
# The discrimination is total, and was measured before this gate was written:
#     sendDataFrameBss(.., BSS_TYPE_UAP)  ->  rx_bss0=0  rx_bss1=71 unrouted=0
#     sendDataFrame(..)  [the regression] ->  rx_bss0=71 rx_bss1=0  unrouted=71
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/m2_uap_lwip.elf"
OUT=$(gate_capture_path "$DIR" tx.uart)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -machine m2-wifi=on \
    -global iw416-sdio.fw-preboot=on -global iw416-sdio.uap=on \
    -global iw416-sdio.tx-loopback=on \
    -kernel "$ELF" -display none $(gate_console "$OUT") \
    -d guest_errors -D "$(gate_capture_path "$DIR" tx.dbg)" &
P=$!; gate_pid $P
# Wait until enough probes have been sent that a zero cannot be "not yet".
for _ in $(seq 1 200); do
    [ -f "$OUT" ] && grep -qE "tx_probes=([1-9][0-9]+)" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

grep -q "^uap_hosting " "$OUT" || { echo "FAIL: the AP never came up"; exit 1; }
grep -qE "tx_probes=([1-9][0-9]+)" "$OUT" || {
    echo "FAIL: the TX probe never sent anything, so nothing was tested."
    echo "      (UAP_TX_PROBE is ON by default; a build without it makes this"
    echo "       gate vacuous rather than failing, which is why the count is"
    echo "       asserted before anything else)"; exit 1; }

# Echoes must come back on the uAP interface, and none may be refused.
grep -q "^hb card=1 bss=1 .* rx_bss0=0 rx_bss1=[1-9][0-9]* unrouted=0 " "$OUT" || {
    echo "FAIL: frames sent on the uAP netif did not come back on it."
    echo "      rx_bss0 climbing with unrouted climbing too means the uAP netif"
    echo "      TRANSMITTED ON THE STATION INTERFACE -- i.e. low_level_output_uap"
    echo "      called sendDataFrame() instead of sendDataFrameBss(), so the"
    echo "      TxPD carried bss_type=0 and a real client would never see it."
    exit 1; }
# Nothing may be tagged for an interface this build never registered.
grep -q "^hb card=1 bss=1 .* rx_bss0=0 " "$OUT" || {
    echo "FAIL: a frame came back tagged for the station interface"; exit 1; }
echo "PASS: uAP-netif frames went out addressed to the uAP interface and returned on it"
