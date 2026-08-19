#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/m2_throughput_test.elf"
OUT=$(gate_capture_path "$DIR" serial.uart)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -d guest_errors -D "$(gate_capture_path "$DIR" serial.dbg)" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "alive=2" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 M.2 throughput test up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
# This gate asserts the DEVICE-ABSENT path: QEMU attaches an SD *memory*
# card, which ignores CMD5, so the correct outcome is the clean fallback --
# NOT a Wi-Fi bring-up.  The throughput services + lwip proof lives on
# silicon in transcript_hw_evkb.txt.
grep -q "^sdio_begin=cmd5-no-response" "$OUT" || {
    echo "FAIL: expected the cmd5-no-response fallback"; exit 1; }
grep -q "^int_status=0x" "$OUT" || { echo "FAIL: no raw evidence line"; exit 1; }
grep -q "^lwip_probe_done" "$OUT" || { echo "FAIL: probe never completed"; exit 1; }
grep -q "^alive=2" "$OUT" || { echo "FAIL: no heartbeat after the probe"; exit 1; }
# The fallback must not claim a Wi-Fi link it cannot have.
if grep -q "^lwip_netif_up" "$OUT"; then
    echo "FAIL: claimed a netif with no card present"; exit 1
fi
if grep -q "^tput: ip=" "$OUT"; then
    echo "FAIL: claimed a link (tput: ip=) with no card present"; exit 1
fi
echo "PASS: reached the cmd5-no-response fallback; services never armed"
