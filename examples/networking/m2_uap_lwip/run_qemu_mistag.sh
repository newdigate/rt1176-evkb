#!/bin/sh
# MIS-TAGGED frames must NOT be delivered -- the W17 handoff's named hazard.
#
# The handoff put it plainly: "With a second BSS live, RX frames from AP
# clients would be silently mis-delivered to the STA netif."  This driver did
# exactly that before W17, because it read straight past the RxPD's bss_type.
#
# The model is told to tag every injected frame bss_type=0 (the STA interface)
# while the guest has ONLY a uAP netif.  A driver that ROUTES on the tag must
# refuse all of them and say how many; a driver that ignores it delivers
# AP-client traffic into the station's stack and NOTHING ANYWHERE REPORTS A
# PROBLEM -- which is what makes this worth a gate rather than a comment.
#
# ★ This is the arm that can actually fail.  run_qemu_data.sh passes on a
#   broken demux too, because there every frame belongs where it landed.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/m2_uap_lwip.elf"
OUT=$(gate_capture_path "$DIR" mistag.uart)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -machine m2-wifi=on \
    -global iw416-sdio.fw-preboot=on -global iw416-sdio.uap=on \
    -global iw416-sdio.inject-bss=0 -global iw416-sdio.inject-count=20 \
    -global iw416-sdio.inject-period-ms=100 -global iw416-sdio.inject-size=64 \
    -kernel "$ELF" -display none $(gate_console "$OUT") \
    -d guest_errors -D "$(gate_capture_path "$DIR" mistag.dbg)" &
P=$!; gate_pid $P
for _ in $(seq 1 200); do
    [ -f "$OUT" ] && grep -q "^hb card=1 bss=1 .* rx_bss[01]=20 " "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "^uap_hosting " "$OUT" || { echo "FAIL: the AP never came up"; exit 1; }

# Every frame claims the STA interface; there is no STA netif; all must be
# refused, COUNTED, and none may reach the uAP side.
grep -q "^hb card=1 bss=1 .* rx_bss0=20 rx_bss1=0 unrouted=20 " "$OUT" || {
    echo "FAIL: STA-tagged frames were not refused."
    echo "      Expected rx_bss0=20 rx_bss1=0 unrouted=20."
    echo "      unrouted=0 with the frames counted anyway means the driver"
    echo "      DELIVERED them despite the tag -- that is the silent"
    echo "      mis-delivery the W17 handoff warned about, and on a real AP it"
    echo "      puts a client's traffic into the station's stack."
    exit 1; }
# And nothing may have reached the socket: a mis-delivered frame that lwip
# happened to drop later is still a mis-delivery.
grep -q "^hb card=1 bss=1 udp_rx=0 " "$OUT" || {
    echo "FAIL: a mis-tagged frame reached the UDP socket"; exit 1; }
echo "PASS: 20 STA-tagged frames refused and counted; none reached the uAP stack"
