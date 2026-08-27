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
#
# DEMONSTRATED RED (2026-08-26), both health assertions, before being trusted:
#   - pswake printed as psWakes() + ((millis()/2000)&1)  ->
#       "FAIL: expected exactly ONE distinct iw416 health signature, got 2"
#   - pswake printed as psWakes() + 1  ->
#       "FAIL: iw416 health signature is not the all-zero card-absent form"
# Each fires by NAME against a bug the other cannot see: a varying counter
# always passes the all-zero grep at some sample, and a constantly-dirty one
# is a single distinct signature.
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
# Wait for the THIRD `pcb ` line (t=6s), not for alive=2: the health block is
# the LAST thing a loop() pass prints, so reaping on its third sample means
# every line asserted below -- alive=2 included -- is already complete in the
# capture.  Reaping on the first interesting line tears the capture mid-line
# under sweep load (the m2_rx_demo [irq] lesson), and three samples make the
# distinct-signature assertion below measure repetition, not a single print.
for _ in $(seq 1 60); do
    [ -f "$OUT" ] && [ "$(grep -c '^pcb ' "$OUT" 2>/dev/null)" -ge 3 ] && break
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
# --- NEW-8 soak instrument, card-absent form --------------------------------
# The 2 s health line exists so a silicon soak can be judged by `sort -u`
# collapsing every sample to ONE distinct all-zero signature (the technique
# that made the uAP soak conclusive).  This gate asserts the card-absent form
# of exactly that: the line is present, it repeats, the run only ever printed
# ONE distinct signature, and that signature is the all-zero one.  Asserting
# mere presence would let a counter that is dirty from boot pass unnoticed.
# wc -l, not `grep -c .`: this script runs under set -e, and an assignment
# from a $() whose pipeline ends in a failing grep EXITS THE SCRIPT SILENTLY
# -- an unnamed red, which is exactly what a gate must never produce.  A
# pipeline ending in wc always exits 0.  (Measured here before the fix: the
# stale-ELF run died at this line with exit 1 and NO FAIL message.)
IW_DISTINCT=$(grep '^iw416 ' "$OUT" | sort -u | wc -l | tr -d ' ')
[ "$IW_DISTINCT" = "1" ] || {
    echo "FAIL: expected exactly ONE distinct iw416 health signature, got $IW_DISTINCT"
    grep '^iw416 ' "$OUT" | sort -u || true; exit 1; }
# tr -d '\r' before the $-anchored matches: the UART capture is CRLF, so a
# bare `...=0$` never matches (the CR sits between the 0 and the newline).
# Every other grep in this gate is prefix-only and does not care.
tr -d '\r' < "$OUT" | grep -q '^iw416 stranded=0 desync=0 split=0 drop=0 seqmis=0 pswake=0$' || {
    echo "FAIL: iw416 health signature is not the all-zero card-absent form"; exit 1; }
# lwip pressure, card-absent: no link, so both pcb lists must be EMPTY at
# every sample -- a nonzero here with no card would be lwip inventing state.
PCB_DISTINCT=$(grep '^pcb ' "$OUT" | sort -u | wc -l | tr -d ' ')
[ "$PCB_DISTINCT" = "1" ] || {
    echo "FAIL: expected exactly ONE distinct pcb pressure line, got $PCB_DISTINCT"
    grep '^pcb ' "$OUT" | sort -u || true; exit 1; }
tr -d '\r' < "$OUT" | grep -q '^pcb act=0 tw=0$' || {
    echo "FAIL: pcb pressure line is not the empty card-absent form (act=0 tw=0)"; exit 1; }
echo "PASS: WL_NO_SHIELD fallback; server.begin() no-op'd cleanly (NO_LINK); alive; health signature clean"
