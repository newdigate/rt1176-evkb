#!/bin/sh
# run_qemu.sh — the CARD-ABSENT gate for wifi_server_test.
#
# WHAT THIS PROVES: with QEMU's default SD *memory* card (which ignores CMD5),
# WiFi.begin() fails cleanly with WL_NO_SHIELD (255), a WiFiServer.begin() on
# that dead stack is a clean no-op -- server_begin=ok_nolink with
# server_err=... (NO_LINK), naming WHICH of ListenError's five FAILURE values
# fired (the enum has six; LISTEN_OK is the sixth) -- and the sketch's
# heartbeat keeps running, so begin() did not wedge.
#
# WHAT THIS DOES NOT PROVE -- and the list is longer than what it does:
#   - NOTHING about accept, data flow, echo, broadcast, or the connection pool.
#     The QEMU IW416 model returns ZERO scan results by design, so the board
#     never associates, `server` is never truthy under any gate in this tree,
#     and not one line of acceptCb / available() / read() / write() executes
#     here.  The evict=/stall=/refuse= counters this gate prints are therefore
#     structurally 0 and asserting on them would be asserting on dead code.
#   - Not even that the card was absent.  WL_NO_SHIELD is the shared exit of
#     all three bring-up failures (no card / no function 1 / no firmware and
#     none supplied), exactly as documented in wifi_client_test/run_qemu.sh:
#     add `-machine m2-wifi=on` to the QEMU line below (KEEPING
#     gate_qemu_machine's `-global fsl-imxrt1170.boot-xip=on`) and the same elf
#     still prints 255, from the "no firmware, none supplied" branch.
#   - Nothing about the SERVER side of a real link: bind, listen, or a second
#     WiFiServer on the same port.  Those need lwip up, which needs a link.
#
# The server data path is SILICON-ONLY: transcript_hw_evkb.txt plus
# wifi_peer.py, which is the authoritative side (it counts its own bytes; the
# board's alive= line is the cross-check, not the measurement).
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/wifi_server_test.elf"
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
grep -q "RT1176 WiFi server test up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "^wifi_status=255" "$OUT" || { echo "FAIL: expected WL_NO_SHIELD (255) with no card"; exit 1; }
grep -q "^server_begin=ok_nolink" "$OUT" || {
    echo "FAIL: server.begin() on a dead stack must be a clean no-op"; exit 1; }
# WHICH exit, not just "it did not listen".  A server_begin=ok_nolink with
# BIND_FAILED would mean something quite different and must not pass.
# Keyed on the NAME, not the ordinal: renumbering ListenError is a
# non-semantic change and must not red a gate.  The sketch prints both
# ("server_err=2 (NO_LINK)") so a bench still gets the number.
grep -qE "^server_err=[0-9]+ \(NO_LINK\)" "$OUT" || {
    echo "FAIL: expected WiFiServer::NO_LINK with no lwip"; exit 1; }
grep -q "^alive=2" "$OUT" || { echo "FAIL: no heartbeat -- server.begin() wedged?"; exit 1; }
echo "PASS: WL_NO_SHIELD fallback; server.begin() no-op'd cleanly (NO_LINK); alive"
