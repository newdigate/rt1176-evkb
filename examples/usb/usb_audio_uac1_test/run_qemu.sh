#!/bin/sh
# QEMU gate for usb_audio_uac1_test -- the UAC1 OUT control plane, on both boards.
#
# WHAT THIS PROVES: that the host stack enumerates QEMU's emulated usb-audio
# device, CLAIMS it, parses its audio topology, selects the streaming alternate
# setting, and starts its streaming state machine. On rt1062 that is the first
# time usb_audio*.cpp has run at all -- the point of Phase 4.
#
# ★ WHAT IT CANNOT PROVE: that audio moves. Isochronous data does NOT flow
# against QEMU's model. Measured on both boards, identically:
#
#   UAC1-TEST: siTD posted, 180 bytes
#   UAC1-TEST: siTD active=1 xact_err=0 babble=0 buf_err=0 bytes_left=180
#   UAC1-TEST: SITD FAIL - see flags above
#
# The controller never runs the descriptor -- active=1, bytes_left unchanged --
# with NO error flags set. So "SITD FAIL" is the EXPECTED output here and is not
# a failure signal, "SITD PASS" is unreachable, and pkts/s is always 0.
# DO NOT add assertions on SITD PASS or pkts/s. Silicon is the sole proof the
# tone plays; see transcript_hw_evkb.txt.
#
# What IS asserted about the siTD is that its error flags are clear. Be precise
# about what that buys, because the obvious justification is wrong: since this
# model never RUNS the descriptor, no path in it can set xact_err/babble/buf_err,
# so the check does not catch a malformed descriptor here. What it actually
# proves today is that the status readback happened at all -- testPacketStatus()
# returned true and the driver read the controller's writeback -- and it goes red
# if that line is missing or any flag differs. It becomes the stronger check it
# looks like only if QEMU ever gains working isochronous transfer, since SITD
# PASS requires those same flags clear. Keep it; just do not overclaim it.
#
# The example requests 48000 (UAC1_RATE_HZ) because QEMU's model offers only
# that rate. The J47 bench device supports 44100 and 48000, so the same binary
# serves both worlds.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/usb_audio_uac1_test.elf"; OUT=$(gate_capture_path "$DIR" uac1.uart)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") -d guest_errors -D "$(gate_capture_path "$DIR" uac1.dbg)" \
    -audiodev none,id=snd0 \
    -device usb-audio,bus=usbhost.0,port=1,audiodev=snd0 &
P=$!; gate_pid $P
# Poll for the last token asserted rather than guessing a duration -- a fixed
# sleep makes the gate load-sensitive, which has cost this tree real time.
#
# seq=2 is a GUEST-time marker (millis()), not a causally-final one like the
# sibling's "SURVEY: end", so host load cannot reorder it past the streaming
# tokens. The failure it could mask: if enumeration ever slipped beyond 2 s of
# guest time, this reaps early and the gate reports "streaming did not start"
# for what is really a timeout. False RED, so it fails safe -- but read that
# message with this in mind before blaming the streaming path.
for _ in $(seq 1 80); do
    [ -f "$OUT" ] && grep -q "HEARTBEAT seq=2 " "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; head -20 "$OUT"

grep -q "UAC1-TEST: start" "$OUT" || { echo "FAIL: banner"; exit 1; }
grep -q "UAC1-TEST: DEVICE READY" "$OUT" \
    || { echo "FAIL: AudioOut never claimed the emulated device"; exit 1; }

# External oracle: this line is QEMU's DECLARED descriptor, which the firmware
# has no knowledge of, so agreeing with it is not the firmware agreeing with
# itself. It is also what proves the UAC1_RATE_HZ=48000 claim path specifically.
grep -q "alt 1 ep=0x01 attr=0x0D mps=192 ch=2 bits=16 rates=48000" "$OUT" \
    || { echo "FAIL: audio topology not parsed as declared"; exit 1; }

grep -q "UAC1-TEST: selected alt=1" "$OUT" \
    || { echo "FAIL: streaming alternate setting not selected"; exit 1; }
grep -q "UAC1-TEST: PASS" "$OUT" || { echo "FAIL: topology report"; exit 1; }
grep -q "UAC1-TEST: siTD posted, 180 bytes" "$OUT" \
    || { echo "FAIL: isochronous descriptor not posted"; exit 1; }
grep -q "xact_err=0 babble=0 buf_err=0" "$OUT" \
    || { echo "FAIL: siTD reported transaction/babble/buffer errors"; exit 1; }
grep -q "UAC1-TEST: streaming started, 1 kHz tone" "$OUT" \
    || { echo "FAIL: streaming did not start"; exit 1; }
grep -q "HEARTBEAT seq=2 " "$OUT" \
    || { echo "FAIL: loop did not survive to a second heartbeat"; exit 1; }

# Vacuity guards. Each is a state the firmware prints INSTEAD of a success token
# above, so a loose grep could otherwise pass while the path failed.
grep -q "UAC1-TEST: siTD POST FAILED" "$OUT" && { echo "FAIL: siTD post failed"; exit 1; }
grep -q "UAC1-TEST: STREAM START FAILED" "$OUT" && { echo "FAIL: stream start failed"; exit 1; }

echo "PASS: UAC1_OUT_CONTROL_PLANE"
