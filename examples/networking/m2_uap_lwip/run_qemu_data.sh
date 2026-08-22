#!/bin/sh
# DATA over the uAP BSS: frames tagged bss_type=1 must reach the uAP netif.
#
# Until the model could tag a frame, the entire uAP RX path was silicon-only.
# This is the standing check that a frame claiming the AP interface is actually
# delivered to the AP interface's stack.
#
# Its sibling run_qemu_mistag.sh is the other half and the more important one:
# this gate alone would still pass on a driver that delivered EVERY frame to
# whatever netif it had, because here every frame happens to belong there.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/m2_uap_lwip.elf"
OUT=$(gate_capture_path "$DIR" data.uart)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -machine m2-wifi=on \
    -global iw416-sdio.fw-preboot=on -global iw416-sdio.uap=on \
    -global iw416-sdio.inject-bss=1 -global iw416-sdio.inject-count=20 \
    -global iw416-sdio.inject-period-ms=100 -global iw416-sdio.inject-size=64 \
    -kernel "$ELF" -display none $(gate_console "$OUT") \
    -d guest_errors -D "$(gate_capture_path "$DIR" data.dbg)" &
P=$!; gate_pid $P
for _ in $(seq 1 200); do
    [ -f "$OUT" ] && grep -q "^hb card=1 bss=1 .* rx_bss[01]=20 " "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "^uap_hosting " "$OUT" || { echo "FAIL: the AP never came up"; exit 1; }

# All 20 injected frames claim the uAP interface and must arrive there.
grep -q "^hb card=1 bss=1 .* rx_bss0=0 rx_bss1=20 unrouted=0 " "$OUT" || {
    echo "FAIL: uAP-tagged frames did not reach the uAP netif."
    echo "      Expected rx_bss0=0 rx_bss1=20 unrouted=0."
    echo "      rx_bss1=0 with rx_bss0=20 would mean the driver reads the tag"
    echo "      from the wrong RxPD offset; unrouted=20 would mean it read the"
    echo "      tag correctly and then found no netif registered for it."
    exit 1; }
echo "PASS: 20 uAP-tagged frames delivered to the uAP netif, none unrouted"
