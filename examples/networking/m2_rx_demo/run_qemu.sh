#!/bin/sh
# run_qemu.sh — the CARD-ABSENT gate for m2_rx_demo.
#
# WHAT THIS PROVES
#   Without `-machine m2-wifi=on` the board attaches a plain SD *memory* card,
#   which by spec ignores CMD5. The right outcome is a clean cmd5-no-response
#   verdict, a `demo_result=no-card` reason and an image that keeps running --
#   NOT success, and above all not a single rx_frame. It is what a default
#   sweep of this example is supposed to see, and it is what stops the two
#   real gates (run_qemu_ring.sh, run_qemu_stranded.sh) from being the only
#   thing standing between this example and an ungated build.
#
# WHAT THIS DOES *NOT* PROVE
#   Nothing whatsoever about the IW416, the SDIO data path, or the two bugs
#   this example exists for. A green run here is the ABSENCE of a card. The
#   sibling gates are where the data path is asserted.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/m2_rx_demo.elf"
# Distinct basenames per gate in this directory: all three run from here and
# each starts with `rm -f`, so a shared name means one gate deleting another's
# LIVE capture under `-j` (see gate_capture_path's comment in gate-lib.sh).
OUT=$(gate_capture_path "$DIR" absent.uart)
DBG=$(gate_capture_path "$DIR" absent.dbg)
rm -f "$OUT" "$DBG"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
for _ in $(seq 1 60); do
    [ -f "$OUT" ] && grep -q "alive=2" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 M.2 RX demo up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "^sdio_begin=cmd5-no-response" "$OUT" || {
    echo "FAIL: expected the cmd5-no-response fallback"
    echo "      (if this now says ok=, the machine attached an SDIO model without"
    echo "       being asked to -- update the gate deliberately, do not delete it)"
    exit 1; }
grep -q "^demo_result=no-card" "$OUT" || { echo "FAIL: no reason code for the absent card"; exit 1; }
# A reason code alone is not enough: "nothing found" is also what a wedged
# image looks like. alive= proves it is still running after the verdict.
grep -q "^alive=2" "$OUT" || { echo "FAIL: no heartbeat after the verdict"; exit 1; }
# The negatives. Reaching the data path with no card would mean the reason code
# is lying, and a stray rx_frame here would make the other two gates worthless.
if grep -q "^demo_ready" "$OUT"; then
    echo "FAIL: the demo declared itself ready with no card present"; exit 1
fi
if grep -q "^rx_frame " "$OUT"; then
    echo "FAIL: received a frame from a card that is not there"; exit 1
fi
echo "PASS: card-absent fallback clean — cmd5-no-response, no ready, no frames"
