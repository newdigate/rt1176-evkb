#!/bin/sh
# QEMU gate for usb_audio_capture_test.
#
# WHAT THIS CAN AND CANNOT PROVE. The mimxrt1170-evk QEMU machine has no USB
# audio device model, so nothing here ever enumerates and no packet is ever
# captured. This gate therefore says NOTHING about whether recording works --
# that is silicon's job, and the design spec (docs/superpowers/specs/
# 2026-08-05-uac-host-input-and-duplex-design.md section 6) says so in as many
# words.
#
# What it does prove, and what is worth a gate:
#
#   - the image boots. The input path added 72 KB of DMAMEM (.bss.dma 113472
#     -> 187200); an image that overran OCRAM or mislaid its zero-init range
#     would fail here rather than on someone's bench.
#   - the loop keeps running. Two heartbeats two seconds apart is a liveness
#     check on a service() that now harvests a capture ring every pass.
#   - the control watchdog stays quiet with NO device attached. ctrl=0/0/0
#     is the assertion that matters most: the input half added three states
#     to that sequence, and a state machine that armed itself without a
#     device would show up here as timeouts climbing against nothing.
#   - the driver reports no device rather than inventing one. ready=0/0.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/build/usb_audio_capture_test.elf"; OUT="$DIR/capture.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/capture.dbg" &
P=$!; gate_pid $P
# Heartbeat is every 2 s, so seq=2 lands around 4 s; 60 quarter-seconds is
# ample margin without letting a hung image burn the full harness timeout.
for _ in $(seq 1 60); do [ -f "$OUT" ] && grep -q "seq=2 " "$OUT" 2>/dev/null && break; sleep 0.25; done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"

grep -q "CAPTURE-GATE v1" "$OUT" || { echo "FAIL: banner"; exit 1; }
grep -q "CAPTURE-TEST: requesting IN 44100 Hz 8 ch 24 bit, taking 2 ch" "$OUT" \
    || { echo "FAIL: requested format not announced"; exit 1; }
grep -q "seq=1 " "$OUT" || { echo "FAIL: no first heartbeat"; exit 1; }
grep -q "seq=2 " "$OUT" || { echo "FAIL: loop did not survive to a second heartbeat"; exit 1; }

# No device in QEMU: recording off, neither direction ready, and -- the real
# assertion -- the control sequence idle with nothing timing out. A missing
# token here must fail by name and not read as proof of the good outcome.
grep -q "rec=0 ready=0/0 ctrl=0/0/0" "$OUT" \
    || { echo "FAIL: expected 'rec=0 ready=0/0 ctrl=0/0/0' with no device attached"; exit 1; }

# And no capture statistics at all, because nothing was captured. This is the
# vacuity guard: the heartbeat only prints pkts/s when recording() is true, so
# its ABSENCE is what proves the driver is not reporting a stream it does not
# have.
grep -q "pkts/s=" "$OUT" && { echo "FAIL: capture stats printed with no device"; exit 1; }

echo "PASS: CAPTURE_BOOT_LIVENESS_NODEVICE"
