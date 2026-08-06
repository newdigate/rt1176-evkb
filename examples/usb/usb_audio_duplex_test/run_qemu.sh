#!/bin/sh
# QEMU gate for usb_audio_duplex_test.
#
# WHAT THIS CAN AND CANNOT PROVE. The mimxrt1170-evk QEMU machine has no USB
# audio device model, so nothing enumerates, nothing is played and nothing is
# captured. This gate therefore says NOTHING about whether duplex works -- that
# is silicon's job, and the design spec (docs/superpowers/specs/
# 2026-08-05-uac-host-input-and-duplex-design.md section 6) says so plainly.
#
# What it does prove, and what earns a gate:
#
#   - the image boots with the GROWN descriptor pool. Stage C took
#     ITD_POOL_SIZE 64 -> 96 for the third HS ring; those descriptors live in
#     DMA-reachable OCRAM alongside two payload rings, and an image that
#     overran OCRAM or mislaid its zero-init range should fail here rather
#     than on someone's bench.
#   - the loop keeps running while servicing BOTH directions.
#   - the control watchdog stays quiet with NO device attached. ctrl=0/0/0 is
#     the assertion that matters most: duplex runs the longest control chain
#     in this driver (clock, interface, rate, then the same three for input),
#     and a state machine that armed itself without a device would show up
#     here as timeouts climbing against nothing.
#   - neither direction claims to be running. out=0 in=0.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/build/usb_audio_duplex_test.elf"; OUT="$DIR/duplex.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/duplex.dbg" &
P=$!; gate_pid $P
# Heartbeat is every 2 s, so seq=2 lands around 4 s; 60 quarter-seconds is
# ample margin without letting a hung image burn the full harness timeout.
for _ in $(seq 1 60); do [ -f "$OUT" ] && grep -q "seq=2 " "$OUT" 2>/dev/null && break; sleep 0.25; done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"

grep -q "DUPLEX-GATE v1" "$OUT" || { echo "FAIL: banner"; exit 1; }
grep -q "DUPLEX-TEST: requesting OUT 44100 Hz, IN 44100 Hz 8 ch 24 bit, taking 2 ch" "$OUT" \
    || { echo "FAIL: requested format not announced"; exit 1; }
grep -q "seq=1 " "$OUT" || { echo "FAIL: no first heartbeat"; exit 1; }
grep -q "seq=2 " "$OUT" || { echo "FAIL: loop did not survive to a second heartbeat"; exit 1; }

# No device in QEMU: neither direction streaming, and -- the real assertion --
# the control sequence idle with nothing timing out. A missing token here must
# fail by name rather than read as proof of the good outcome.
grep -q "out=0 in=0 ctrl=0/0/0" "$OUT" \
    || { echo "FAIL: expected 'out=0 in=0 ctrl=0/0/0' with no device attached"; exit 1; }

# And no per-direction statistics at all, because neither direction ran. This
# is the vacuity guard: the heartbeat prints the OUT[...] and IN[...] blocks
# only when streaming()/recording() are true, so their ABSENCE is what proves
# the driver is not reporting streams it does not have. Checked separately so
# a failure names WHICH direction lied.
grep -q "OUT\[" "$OUT" && { echo "FAIL: OUT stats printed with no device"; exit 1; }
grep -q "IN\[" "$OUT"  && { echo "FAIL: IN stats printed with no device"; exit 1; }

echo "PASS: DUPLEX_BOOT_LIVENESS_NODEVICE"
