#!/bin/sh
# run_qemu.sh — the CLEAN-FAILURE gate for wifi_client_test (default: no card).
#
# WHAT THIS PROVES: with QEMU's default SD *memory* card (which ignores CMD5),
# WiFi.begin() must fail cleanly with WL_NO_SHIELD (255), never claim an IP,
# and leave the sketch's heartbeat running.  This is what a default sweep sees.
# WHAT THIS DOES NOT PROVE: anything about Wi-Fi -- and not even that the card
# was absent.  WL_NO_SHIELD is the shared exit of all three bring-up failures
# (no card / no function 1 / no firmware and none supplied), and this gate
# cannot tell them apart: add `-machine m2-wifi=on` to this gate's own QEMU
# line below, KEEPING gate_qemu_machine's `-global fsl-imxrt1170.boot-xip=on`
# (boot-xip is a -global on the SoC object, NOT a machine property --
# `-machine mimxrt1170-evk,m2-wifi=on` alone aborts with an EMPTY capture,
# which looks exactly like firmware that never started).  The SAME elf then
# still prints 255, from the "no firmware, none supplied" branch -- measured,
# not assumed.  What IS asserted is the clean-failure contract on whichever
# branch fires.
# Enumeration + scan live in run_qemu_wifi.sh (fw-preboot=on, where the
# card-present path is actually exercised); association/DHCP/TCP live on
# silicon only (transcript_hw_evkb.txt) -- the QEMU model deliberately returns
# zero scan results, so no gate anywhere may assert association.
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
echo "PASS: clean WL_NO_SHIELD failure, no IP claimed, heartbeat alive"
