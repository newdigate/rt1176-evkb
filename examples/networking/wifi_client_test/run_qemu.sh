#!/bin/sh
# run_qemu.sh — the CARD-ABSENT gate for wifi_client_test.
#
# WHAT THIS PROVES: with QEMU's default SD *memory* card (which ignores CMD5),
# WiFi.begin() must fail cleanly with WL_NO_SHIELD (255), never claim an IP,
# and leave the sketch's heartbeat running.  This is what a default sweep sees.
# WHAT THIS DOES NOT PROVE: anything about Wi-Fi.  Enumeration + scan live in
# run_qemu_wifi.sh; association/DHCP/TCP live on silicon only
# (transcript_hw_evkb.txt) — the QEMU model deliberately returns zero scan
# results, so no gate anywhere may assert association.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/wifi_client_test.elf"
OUT=$(gate_capture_path "$DIR" serial.uart)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -d guest_errors -D "$(gate_capture_path "$DIR" serial.dbg)" &
P=$!; gate_pid $P
for _ in $(seq 1 60); do
    [ -f "$OUT" ] && grep -q "alive=2" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 WiFi client test up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "^wifi_status=255" "$OUT" || { echo "FAIL: expected WL_NO_SHIELD (255) with no card"; exit 1; }
grep -q "^alive=2" "$OUT" || { echo "FAIL: no heartbeat after begin()"; exit 1; }
if grep -q "^wifi_ip=" "$OUT"; then
    echo "FAIL: claimed an IP with no card present"; exit 1
fi
echo "PASS: WL_NO_SHIELD fallback, no IP claimed, heartbeat alive"
