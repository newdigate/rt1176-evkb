#!/bin/sh
# run_qemu.sh -- the CARD-ABSENT gate for bt_tone_test (BT-3 phase 4).
#
# WHAT THIS PROVES
#   With no second -serial, qemu2's LPUART2 has no chardev -- exactly the
#   "nothing answered" case on silicon (m2_hci_probe's own card-absent gate).
#   This image goes one step further than the probe: it always calls
#   A2dpSource::connect(), so the gate also proves the whole A2DP bring-up
#   chain (Hci Reset -> BtLink inquiry -> ... -> Avdtp) fails CLEANLY with no
#   peer, and that AudioOutputBluetooth never claims to be streaming:
#     * hci_reset=timeout BY NAME after 10 attempts;
#     * a2dp=connect_failed (BtLink's own inquiry -- the first HCI command
#       A2dpSource::connect() issues -- times out with no card to answer it);
#     * "streaming" is NEVER printed (btout.begin() is gated on a2dp==OK);
#     * every heartbeat reads streaming=0 blocks=0 packets=0 drops=0.
#   The reason codes plus the LATER heartbeat are the positive tokens: "no
#   identity printed" is also what a dead image produces.
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
# Preamble + 10 x 0.5 s Reset attempts + the A2dpSource connect attempt (its
# own inquiry times out in ~1 s) all land before the first heartbeat. Wait for
# the SECOND heartbeat: it is the last line this gate parses.
for _ in $(seq 1 120); do
    [ -f "$OUT" ] && grep -q "^hb streaming=0 blocks=0 packets=0 drops=0 hw=0 n=1[[:space:]]*$" "$OUT" 2>/dev/null && break
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
for T in "^hci_version" "^bd_addr=" "^hci_buffer" "^streaming[[:space:]]*\$"; do
    if grep -q "$T" "$OUT"; then echo "FAIL: reported '$T' with nothing on LPUART2"; exit 1; fi
done
grep -q "^a2dp=connect_failed[[:space:]]*$" "$OUT" || { echo "FAIL: expected a2dp=connect_failed (no card to answer BtLink's inquiry)"; exit 1; }
grep -q "^hb streaming=0 blocks=0 packets=0 drops=0 hw=0 n=0[[:space:]]*$" "$OUT" || { echo "FAIL: no heartbeat after bring-up"; exit 1; }
grep -q "^hb streaming=0 blocks=0 packets=0 drops=0 hw=0 n=1[[:space:]]*$" "$OUT" || { echo "FAIL: image did not keep heartbeating"; exit 1; }
echo "PASS: A2DP bring-up reached connect_failed cleanly with no card, and AudioOutputBluetooth never claimed to stream"
