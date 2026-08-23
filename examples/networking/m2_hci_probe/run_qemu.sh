#!/bin/sh
# run_qemu.sh -- the CARD-ABSENT gate for m2_hci_probe (BT-1).
#
# WHAT THIS PROVES
#   With no second -serial, qemu2's LPUART2 has no chardev: the firmware can
#   transmit and will never receive a byte -- exactly the "nothing answered"
#   case on silicon.  The gate asserts that the probe
#     * reaches its Reset step and fails BY NAME after all 10 attempts
#       (hci_reset=fail reason=ncmd_starved attempts=10 timeouts=1
#       starved=9): Hci::begin() grants exactly one command credit up
#       front (Vol 4 Part E 4.4), attempt 1 spends it and genuinely times
#       out, and attempts 2-10 never get it back -- nothing ever replies --
#       so they fail as NCMD_STARVED instead;
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
# DEMONSTRATED RED (2026-08-XX): <quote the Task 11 Step 6 result here>
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
#
# hci=ncmd_starved, not no_response: Hci::begin() grants exactly ONE command
# credit up front (Vol 4 Part E 4.4) and only a reply ever restores it.
# Attempt 1 spends that credit and genuinely times out (a real TIMEOUT,
# counted in timeouts=1); attempts 2-10 never get a credit back -- nothing
# ever replies -- so dispatch() fails them as NCMD_STARVED as soon as each
# one's own queued-time deadline elapses (starved=9).  This is deterministic
# given no chardev on LPUART2, not a race: measured on the first-ever run,
# see the report for the plan-vs-firmware note.
for _ in $(seq 1 120); do
    [ -f "$OUT" ] && grep -q "^hb card=0 hci=ncmd_starved n=1 " "$OUT" 2>/dev/null && break
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
grep -q "^hci_reset=fail reason=ncmd_starved attempts=10 timeouts=1 framing=0 starved=9 qfull=0 late=0[[:space:]]*$" "$OUT" || {
    echo "FAIL: expected the Reset failure BY NAME (one real timeout spending the initial credit, then nine starved attempts once it is never returned)"; exit 1; }
# The fallback must not claim what it cannot have read.
for T in "^hci_version" "^bd_addr=" "^hci_buffer" "^inquiry=started" "^inq:" "^inq_name:"; do
    if grep -q "$T" "$OUT"; then echo "FAIL: reported '$T' with nothing on LPUART2"; exit 1; fi
done
grep -q "^hci_probe_done[[:space:]]*$" "$OUT" || { echo "FAIL: probe never completed"; exit 1; }
grep -q "^hb card=0 hci=ncmd_starved n=1 " "$OUT" || { echo "FAIL: no heartbeat after the probe"; exit 1; }
echo "PASS: HCI probe reached the ncmd_starved fallback cleanly and kept running"
