#!/bin/sh
# run_qemu.sh -- the CARD-ABSENT gate for m2_hci_probe (BT-1).
#
# WHAT THIS PROVES
#   With no second -serial, qemu2's LPUART2 has no chardev: the firmware can
#   transmit and will never receive a byte -- exactly the "nothing answered"
#   case on silicon.  The gate asserts that the probe
#     * reaches its Reset step and times out BY NAME (hci_reset=timeout
#       reason=no_response) after all 10 attempts, with timeouts=10;
#     * prints NOTHING it could only know from a reply (no hci_version,
#       no bd_addr, no inq:) -- the fallback must not invent identity;
#     * reaches hci_probe_done and keeps heartbeating afterwards.
#   The reason code plus the LATER heartbeat are the positive tokens: "no
#   identity printed" is also what a dead image produces.
#
# WHAT THIS DOES NOT PROVE
#   Anything about the IW416.  Every claim about the card lives in
#   transcript_hw_evkb.txt.  The bidirectional transport is gated by
#   run_qemu_hci.sh against a fake controller.
#
# WHAT THIS GATE CANNOT SEE (2026-08-23, measured)
#   Breaking the driver's reply matching -- M2Radio hci/Hci.cpp, onPacket's
#   Command Complete branch, `opcode == m_inflightOpcode` ->
#   `opcode == (m_inflightOpcode ^ 1)`, so no reply is ever matched -- left this
#   gate GREEN on the same ELF:
#     PASS: HCI probe reached the no_response fallback cleanly and kept running
#   It has nothing to match here, so a total failure of reply matching is
#   INDISTINGUISHABLE from the card-absent case it asserts.  run_qemu_hci.sh
#   caught it in the same breath (`late=10`, Reset timing out with replies on
#   the wire), and that asymmetry is why the [hci] variant exists.  Do not read
#   a green here as evidence about the transport.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || {
    echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board)) -- the M.2 socket is on the MIMXRT1170-EVKB"; exit 1; }
ELF="$DIR/$(gate_build_dir)/m2_hci_probe.elf"
OUT=$(gate_capture_path "$DIR" serial.uart)
DBG=$(gate_capture_path "$DIR" serial.dbg)
rm -f "$OUT" "$DBG"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# The probe spends ~1.1 s in the preamble, the SDIO timeouts, 0.4 s settle and
# 10 x 0.5 s Reset attempts before the first heartbeat.  Wait for the SECOND
# heartbeat: it is the last line this gate parses.
for _ in $(seq 1 120); do
    [ -f "$OUT" ] && grep -q "^hb card=0 hci=no_response n=1 " "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 M.2 HCI probe up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "^sdio_begin=cmd5-no-response" "$OUT" || {
    echo "FAIL: expected the cmd5-no-response SDIO fallback (a plain SD card ignores CMD5)"; exit 1; }
grep -q "^card=0[[:space:]]*$" "$OUT" || { echo "FAIL: card= line missing or not 0"; exit 1; }
grep -q "^serial2=up_115200[[:space:]]*$" "$OUT" || { echo "FAIL: Serial2 never came up"; exit 1; }
grep -q "^hci_reset=timeout reason=no_response attempts=10 timeouts=10 framing=0 starved=0 qfull=0 late=0[[:space:]]*$" "$OUT" || {
    echo "FAIL: expected the Reset timeout BY NAME with all ten attempts counted"; exit 1; }
# The fallback must not claim what it cannot have read.
for T in "^hci_version" "^bd_addr=" "^hci_buffer" "^inquiry=started" "^inq:" "^inq_name:"; do
    if grep -q "$T" "$OUT"; then echo "FAIL: reported '$T' with nothing on LPUART2"; exit 1; fi
done
grep -q "^hci_probe_done[[:space:]]*$" "$OUT" || { echo "FAIL: probe never completed"; exit 1; }
grep -q "^hb card=0 hci=no_response n=1 " "$OUT" || { echo "FAIL: no heartbeat after the probe"; exit 1; }
echo "PASS: HCI probe reached the no_response fallback cleanly and kept running"
