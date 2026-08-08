#!/bin/sh
# QEMU gate for usb_descriptor_survey -- the FIRST gate in this tree that runs
# the USB audio stack against an actual emulated audio device.
#
# Every other usb_audio_* gate can only prove its image boots and its loop
# survives, because until now nothing on the emulated bus was an audio device.
# QEMU ships one (hw/usb/dev-audio.c) and the machine's OTG2 host bus takes it,
# so this gate asserts things that previously waited for the bench:
#
#   - a device ENUMERATES on the emulated host and its configuration
#     descriptors are retrieved intact (104 bytes, byte-for-byte what the
#     model declares);
#   - the descriptor walk finds the AudioControl and AudioStreaming
#     interfaces and reads bcdADC;
#   - FORMAT_TYPE_I is decoded: channels, bit resolution, and the discrete
#     sample-rate list;
#   - the endpoint's SYNCHRONISATION TYPE is decoded -- the field that decides
#     how much work supporting a device is, and the one this survey exists to
#     answer;
#   - the verdict logic reaches the right conclusion about a synchronous
#     device.
#
# WHY THIS EXAMPLE AND NOT A STREAMING ONE. usb_descriptor_survey deliberately
# never binds: its claim() copies the descriptor and returns false. So it is
# format-agnostic, and this gate does not depend on the emulated device's
# format matching what any streaming sketch happens to request. QEMU's model
# offers 48000 Hz ONLY (USBAUDIO_SAMPLE_RATE is a compile-time #define baked
# into its descriptor tables, with no property to change it), while the
# streaming examples ship asking for 44100 to match the Audio library -- so a
# streaming gate here would have to either change an example's shipping format
# or patch QEMU. Neither is worth it for coverage this gate already provides.
#
# WHAT IT STILL CANNOT PROVE: that isochronous data moves. Measured
# 2026-08-06, an OUT sketch built for 48000 enumerates, claims, selects alt 1,
# completes the whole control sequence with ctrl=0/0/0 and reports "streaming
# started" -- and then sits at pkts/s=0. The transport does not run against
# this model and the cause is not established (a split-transaction/TT
# mismatch is the leading suspicion: siTD is for a full-speed device behind a
# high-speed hub, and here the device is on the root port). Silicon remains
# the only proof that audio flows.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/usb_descriptor_survey.elf"; OUT="$DIR/survey.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_serial1 "$OUT") -d guest_errors -D "$DIR/survey.dbg" \
    -audiodev none,id=snd0 \
    -device usb-audio,bus=usbhost.0,port=1,audiodev=snd0 &
P=$!; gate_pid $P
# The survey prints its report as soon as the device enumerates, then a
# "waiting" heartbeat every 2 s. Wait for the end-of-report marker.
for _ in $(seq 1 60); do [ -f "$OUT" ] && grep -q "SURVEY: end" "$OUT" 2>/dev/null && break; sleep 0.25; done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; grep -v "SURVEY-HEX" "$OUT" | head -20

grep -q "SURVEY: start" "$OUT" || { echo "FAIL: banner"; exit 1; }

# Enumeration reached the driver with the descriptors intact. 104 is what
# QEMU's model declares; a short or truncated capture would change it.
grep -q "SURVEY: device 46F4:0002, 104 descriptor bytes" "$OUT" \
    || { echo "FAIL: device did not enumerate with the expected 104-byte config"; exit 1; }

grep -q "class=0x01(AUDIO) sub=0x01" "$OUT" || { echo "FAIL: AudioControl interface not found"; exit 1; }
grep -q "class=0x01(AUDIO) sub=0x02" "$OUT" || { echo "FAIL: AudioStreaming interface not found"; exit 1; }
grep -q "UAC 1.00" "$OUT" || { echo "FAIL: bcdADC not decoded as UAC 1.00"; exit 1; }

# FORMAT_TYPE_I decoded: channels, resolution, and the discrete rate list.
grep -q "format: 2 ch, 16-bit, 1 rate(s): 48000" "$OUT" \
    || { echo "FAIL: FORMAT_TYPE_I not decoded (2 ch, 16-bit, 48000)"; exit 1; }

# The endpoint, including the synchronisation type this survey exists to
# report. Whitespace-tolerant: the alignment is cosmetic, the fields are not.
grep -qE "ep 0x01 +OUT +iso +sync=synchronous +usage=data +mps=192" "$OUT" \
    || { echo "FAIL: iso OUT endpoint / sync type not decoded"; exit 1; }

grep -q "VERDICT adaptive/synchronous only" "$OUT" \
    || { echo "FAIL: wrong verdict for a synchronous device"; exit 1; }

# Vacuity guards. A run where nothing enumerated would still print a banner
# and could still 'pass' a loose grep, so assert the outcomes that must NOT
# appear: the not-an-audio-device verdict, and the async verdicts which would
# mean the sync type was misread in the direction that matters most.
grep -q "VERDICT not an audio device" "$OUT" && { echo "FAIL: enumerated device not recognised as audio"; exit 1; }
grep -q "VERDICT ASYNCHRONOUS" "$OUT" && { echo "FAIL: synchronous device reported as asynchronous"; exit 1; }

echo "PASS: SURVEY_ENUMERATES_EMULATED_UAC1_DEVICE"
