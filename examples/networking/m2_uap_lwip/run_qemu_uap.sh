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
#   * the modelled station joins INSTANTLY when the BSS starts.  A real client
#     scans, authenticates, associates and on WPA2 completes a 4-way handshake
#     -- seconds, and variable.  So this gate can assert that a join is SEEN
#     and reported, never anything about how long it takes.
#   * no DHCP exchange happens: the modelled station never sends anything.  The
#     server is exercised to the point of binding and no further.
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
# --- the modelled station, and the event that announces it ------------------
# The model joins a station as soon as the BSS is up, so STA_LIST must report
# it.  Asserting on the COUNT rather than on a join TRANSITION is deliberate:
# the station is already associated by the first poll, so no 0->1 edge is ever
# seen, and a gate written against `joins=1` would fail on a working model.
# The same reasoning applies on silicon for a different reason -- an event can
# be missed, and a count cannot drift.
grep -q "^hb card=1 bss=1 .* sta=1 " "$OUT" || {
    echo "FAIL: STA_LIST did not report the modelled station"; exit 1; }
# ★ The join event id DEPENDS ON THE SECURITY MODE: open raises
# EVENT_MICRO_AP_STA_ASSOC (0x2D), WPA2 raises EVENT_MICRO_AP_RSN_CONNECT
# (0x51) after the handshake.  This build is OPEN (gates carry no credential),
# so 0x2D is the correct one here -- and the 0x01 in the top byte is
# EVENT_GET_BSS_TYPE saying uAP, which is what makes it OUR event and not
# something from the station side.
grep -q "^hb card=1 bss=1 .* lastevent=0x100002D" "$OUT" || {
    echo "FAIL: wrong or missing join event.  Expected 0x100002D --"
    echo "      bss_type=1 (bits 31:24) + EVENT_MICRO_AP_STA_ASSOC (0x2D)."
    echo "      0x1000051 would mean the model thinks this AP is WPA2; a bare"
    echo "      0x2D would mean it dropped the bss_type the host demuxes on."
    exit 1; }
echo "PASS: configure -> BSS_START -> netif -> socket -> DHCP, in order, health clean"
