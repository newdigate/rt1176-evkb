#!/bin/sh
# The AP BRING-UP gate: configure -> BSS_START -> netif -> UDP socket -> DHCP
# server, against a model that has an AP surface (qemu2 `uap=on`).
#
# Until this existed, every uAP gate asserted the card-ABSENT fallback and the
# entire bring-up sequence was silicon-only -- verified once, by hand, and
# therefore free to rot.  This is the standing check that the sequence still
# runs and still runs IN THE RIGHT ORDER.
#
# ★ WHAT IT DOES NOT PROVE, and the distinction matters:
#   * nothing goes on the air.  The model has no radio.  "The AP is hosting"
#     here means the command sequence was accepted, not that a station could
#     see it -- that is what the foreign-radio scans in transcript_hw_evkb.txt
#     are for.
#   * no client joins.  The model has no station, so sta_count is 0 and the
#     DHCP server is exercised only to the point of binding.  Join/leave/data
#     gates need a modelled station and there is not one yet.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/m2_uap_lwip.elf"
OUT=$(gate_capture_path "$DIR" uap.uart)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -machine m2-wifi=on \
    -global iw416-sdio.fw-preboot=on -global iw416-sdio.uap=on \
    -kernel "$ELF" -display none $(gate_console "$OUT") \
    -d guest_errors -D "$(gate_capture_path "$DIR" uap.dbg)" &
P=$!; gate_pid $P
# Wait for a heartbeat that reports a live BSS -- the LAST thing setup() leads
# to, not the first interesting line, so the capture cannot be torn mid-line.
for _ in $(seq 1 240); do
    [ -f "$OUT" ] && grep -q "^hb card=1 bss=1 " "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

grep -q "RT1176 M.2 uAP + lwip up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "^sdio_begin=ok" "$OUT" || { echo "FAIL: SDIO enumeration failed"; exit 1; }

# --- the sequence, asserted IN ORDER ---------------------------------------
# Order is the substance: configuring AFTER starting would be a different (and
# broken) driver that these same greps would otherwise accept.
grep -q "^uap_configure=ok result=0x0[[:space:]]*$" "$OUT" || {
    echo "FAIL: the AP configuration was rejected"
    echo "      (a populated SYS_CONFIGURE must be accepted; if this times out"
    echo "       instead, the request lost its SSID TLV and wedged the port)"
    exit 1; }
grep -q "^uap_bss_start=ok result=0x0[[:space:]]*$" "$OUT" || {
    echo "FAIL: BSS_START was rejected -- on this model that means it ran"
    echo "      BEFORE a configuration was accepted"; exit 1; }
CFG=$(grep -n "^uap_configure=ok" "$OUT" | head -1 | cut -d: -f1)
BSS=$(grep -n "^uap_bss_start=ok" "$OUT" | head -1 | cut -d: -f1)
[ "$CFG" -lt "$BSS" ] || {
    echo "FAIL: BSS_START (line $BSS) came before the configuration (line $CFG)"
    exit 1; }

grep -q "^uap_hosting ssid=.* chan=" "$OUT" || { echo "FAIL: never reported hosting"; exit 1; }
grep -q "^uap_netif_up addr=192.168.44.1[[:space:]]*$" "$OUT" || {
    echo "FAIL: the uAP netif did not come up at the expected address"; exit 1; }
grep -q "^uap_udp_bound port=5001[[:space:]]*$" "$OUT" || {
    echo "FAIL: the UDP socket was not bound"; exit 1; }
grep -q "^uap_dhcp_up pool=" "$OUT" || { echo "FAIL: the DHCP server did not start"; exit 1; }
grep -q "^uap_lwip_done" "$OUT" || { echo "FAIL: setup never completed"; exit 1; }

# --- and the health line, which must be spotless on a model that drops nothing
grep -q "^health stranded=0 desync=0 split=0 dropped=0 seqmm=0 pswake=0 rx_bss0=0 unrouted=0 " "$OUT" || {
    echo "FAIL: a health counter is non-zero on a model that has no radio and"
    echo "      therefore nothing to lose -- suspect the driver, not the air"
    exit 1; }
# No station exists here, so any join is the MODEL inventing one.
grep -q "^hb card=1 bss=1 .* sta=0 joins=0 leaves=0 " "$OUT" || {
    echo "FAIL: reported a client on a model that has no station"; exit 1; }
echo "PASS: configure -> BSS_START -> netif -> socket -> DHCP, in order, health clean"
