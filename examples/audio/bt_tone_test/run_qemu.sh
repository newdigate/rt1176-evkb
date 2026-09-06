#!/bin/sh
# run_qemu.sh -- the CARD-ABSENT gate for bt_tone_test (BT-3 phase 4).
#
# WHAT THIS PROVES
#   With no second -serial, qemu2's LPUART2 has no chardev -- exactly the
#   "nothing answered" case on silicon (m2_hci_probe's own card-absent gate).
#   NEW-34 piece 2: setup() only calls BtSession::begin() when the Reset step
#   came up OK (s_hciSt == Hci::OK); with no card it skips the session
#   entirely rather than starting it and letting it fail, so the gate proves:
#     * hci_reset=timeout BY NAME after 10 attempts;
#     * a2dp=deferred (no HCI: card absent) -- the session is never begun, so
#       no inquiry/page is ever issued (there is no card to answer one);
#     * "streaming by=" is NEVER printed (onStreamCb(true) only fires from
#       BtSession reaching STREAMING, which needs a begun session);
#     * every heartbeat reads streaming=0 blocks=0 packets=0 drops=0 hw=0, and
#       every bt_link health line reads links=0 lost=0 (both keep printing).
#   The reason codes plus the LATER heartbeat/health lines are the positive
#   tokens: "no identity printed" is also what a dead image produces.
#
# WHAT THIS DOES NOT PROVE
#   Anything about a real A2DP sink or the SBC encoder's output -- that needs
#   silicon (or the [hci] fake-controller style gate this example does not yet
#   have). This gate is the same class as m2_hci_probe/run_qemu.sh: vacuity
#   guards on the no-card path, not a positive streaming proof.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || {
    echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board)) -- the M.2 socket is on the MIMXRT1170-EVKB"; exit 1; }
ELF="$DIR/$(gate_build_dir)/bt_tone_test.elf"
OUT=$(gate_capture_path "$DIR" serial.uart)
DBG=$(gate_capture_path "$DIR" serial.dbg)
rm -f "$OUT" "$DBG"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# Preamble + 10 x 0.5 s Reset attempts land before setup() ever reaches the session
# (skipped entirely with no card -- see bt_tone_test.cpp).  Wait for the bt_link health
# line to appear TWICE: it is the last line this gate parses, and it proves the image
# keeps heartbeating rather than stalling after the first line.
for _ in $(seq 1 120); do
    [ -f "$OUT" ] && [ "$(grep -c '^bt_link links=0 ' "$OUT" 2>/dev/null)" -ge 2 ] && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 BT tone test up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "^serial2=up_115200[[:space:]]*$" "$OUT" || { echo "FAIL: Serial2 never came up"; exit 1; }
grep -q "^hci_reset=timeout reason=no_response attempts=10 timeouts=10 framing=0 starved=0 qfull=0 late=0[[:space:]]*$" "$OUT" || {
    echo "FAIL: expected the Reset timeout BY NAME with all ten attempts counted"; exit 1; }
# The fallback must not claim what it cannot have read.
for T in "^hci_version" "^bd_addr=" "^hci_buffer" "^streaming by="; do
    if grep -q "$T" "$OUT"; then echo "FAIL: reported '$T' with nothing on LPUART2"; exit 1; fi
done
grep -q "^a2dp=deferred (no HCI: card absent)[[:space:]]*$" "$OUT" || { echo "FAIL: expected a2dp=deferred with no card"; exit 1; }
grep -q "^hb streaming=0 blocks=0 packets=0 drops=0 hw=0[[:space:]]*$" "$OUT" || { echo "FAIL: no vacuous heartbeat"; exit 1; }
grep -q "^bt_link links=0 lost=0 " "$OUT" || { echo "FAIL: no bt_link health line"; exit 1; }
echo "PASS: A2DP session stays vacuous with no card (no stream, no link, health line present)"
