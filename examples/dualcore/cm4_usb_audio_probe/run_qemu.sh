#!/bin/sh
# QEMU gate for cm4_usb_audio_probe (Phase 7.3): can the CM4 claim a USB audio
# device, negotiate a format, complete the control sequence and ARM an
# isochronous OUT stream?
#
# ============================================================================
# ★★ READ THIS BEFORE ADDING A PACKET ASSERTION. YOU WILL WANT TO. DO NOT.
# ============================================================================
#
# ISOCHRONOUS DATA DOES NOT FLOW IN QEMU. Measured 2026-08-06 and recorded in
# docs/KNOWN-BROKEN-GATES.md: against the emulated `usb-audio` device an OUT
# sketch enumerates, claims, selects alt 1, completes the whole control sequence
# with ctrl=0/0/0, reports "streaming started" -- and then sits at pkts/s = 0
# forever. The cause is not established; the leading suspicion is a split-
# transaction/TT mismatch, since an siTD is the descriptor for a full-speed
# device behind a high-speed hub and the model has this one on the root port.
#
# So this gate covers the CONTROL PLANE only, and `pkts` is a WORLD-SPLIT token:
# the firmware PRINTS STREAM_PACKETS=PASS/FAIL, this gate asserts only that the
# token is PRESENT, and SILICON asserts that it is PASS. Same idiom, and the
# same reasoning, as PHY_PLL_CM4 in cm4_usb_irq_probe (7.1) -- where asserting
# the value in QEMU would have made the gate unpassable for a reason unrelated
# to what it was testing. That mistake has now cost this phase twice; the third
# time is not free either.
#
# What the gate DOES prove, and what earns it a place in the sweep:
#
#   - the CM4 image links the UAC class driver on top of 7.2's transport core
#     and still boots, with ~190 KB of DMA-reachable rings in OCRAM2;
#   - a real device is CLAIMED by that driver on the M4 (bound + vid/pid);
#   - a FORMAT is negotiated -- uac1_find_alt() refuses a rate/channel/width the
#     device never advertised, so a valid alt is a real negotiation;
#   - the post-claim CONTROL SEQUENCE completes: nothing outstanding, no
#     watchdog timeout, no Transfer_t pool exhaustion;
#   - 32 siTDs are allocated from the isochronous pool, filled and linked into
#     all 32 periodic frame slots without error.
#
# THE FORMAT IS 48000/2/16 AND THAT IS NOT A PREFERENCE. QEMU's usb-audio bakes
# USBAUDIO_SAMPLE_RATE=48000 into its descriptor tables as a compile-time
# #define (hw/usb/dev-audio.c:118) with no property to override it. Every other
# streaming example in this tree asks for 44100 to match the Audio library,
# which is exactly why none of them can be gated against that model.
#
# THE DEVICE IS HOTPLUGGED, NOT PRESENT AT RESET, inherited from 7.1 where it
# was learned the expensive way. ChipIdea defers the attach report to the
# guest's PP write (chipidea.c:230 -> hcd-ehci.c:1066), so a device present at
# reset raises the port-change condition while USBHost::begin() is still
# running. This gate waits for the CM4's stage-3 marker -- "armed, port powered,
# nothing connected" -- and only then device_adds.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/cm4_usb_audio_probe.elf"
OUT="$DIR/cm4_usb_audio.uart"
MON="$DIR/mon.sock"
rm -f "$OUT" "$MON"
gate_tmp "$MON"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_usb_audio.dbg" \
    -audiodev none,id=snd0 \
    -monitor unix:"$MON",server,nowait &
P=$!; gate_pid $P

# Wait for the CM4 to finish USBHost::begin() and settle (stage 3), THEN plug.
# Polling for the token rather than sleeping a fixed interval: the emulator does
# not run at wall-clock rate, so a fixed offset would be fragile.
for _ in $(seq 1 160); do
    [ -f "$OUT" ] && grep -q "s3=A5B00003" "$OUT" 2>/dev/null && break
    sleep 0.25
done
# python3, not nc: -U (unix socket) support varies across macOS versions, and
# python3 is already a dependency of this repo's tools.
printf 'device_add usb-audio,bus=usbhost.0,port=1,audiodev=snd0\n' \
    | python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(sys.stdin.buffer.read()); s.close()' "$MON"

# Longer than 7.2's wait: this firmware adds a fixed 4 s streaming window after
# the claim window, and the CM4's millis() tracks wall-clock in the model (the
# machine clocks its CM4 at 400 MHz, fsl-imxrt1170.c:365, and QEMU derives
# CYCCNT from the virtual clock scaled by it).
for _ in $(seq 1 200); do
    [ -f "$OUT" ] && grep -q "CM4USBAUDIO-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"

echo "==== captured ===="; cat "$OUT"

grep -q "CM4USBAUDIO-DONE" "$OUT" || { echo "FAIL: no done marker"; exit 1; }

# Assertions ordered so a red run LOCALISES itself. These are distinct faults
# and must not collapse into one FAIL.
grep -q "STAGES=PASS" "$OUT" \
    || { echo "FAIL: the CM4 did not reach all seven stage markers"; exit 1; }

# 1. DWT. First, and a MEASURED cycle delta rather than a register readback: if
# DEMCR.TRCENA/DWT_CTRL.CYCCNTENA do not take, ARM_DWT_CYCCNT reads 0 forever,
# millis() stands still, and every timeout in the stack waits forever -- INCLUDING
# the audio driver's own control watchdog, which is the only way back from a
# configuration request the device never answers.
grep -q "DWT_ALIVE=PASS" "$OUT" \
    || { echo "FAIL: the CM4 cycle counter is dead -- every USBHost_t36 timeout, and the audio control watchdog, are frozen"; exit 1; }
grep -q "dwt=00000000" "$OUT" \
    && { echo "FAIL: dwt is zero yet DWT_ALIVE passed -- verdict logic is wrong"; exit 1; }

# 2. USBINTR. Control transfers -- enumeration's and the driver's
# SET_INTERFACE/SET_CUR alike -- complete through UAI -> followup_Transfer
# (ehci.cpp:380). USBHost::begin() sets the full mask; this asserts the image
# did not narrow it afterwards.
grep -q "USBINTR_UAIE=PASS" "$OUT" \
    || { echo "FAIL: UAIE not armed -- control transfers cannot complete"; exit 1; }

# 3. A 0->1 CONNECT TRANSITION during the window, not merely a device present at
# the end of it. A device already attached at reset, and one that never arrives,
# both fail here -- the same un-fakeability the silicon probe has.
grep -q "PLUG_TRANSITION=PASS" "$OUT" \
    || { echo "FAIL: no 0->1 connect transition during the observation window"; exit 1; }

# 4. The audio driver was BOUND to the device. Unlike 7.2's probe -- which
# claimed nothing on purpose -- this driver takes the device at type 0, so
# operator bool() going true means claim() returned TRUE, which in turn means
# the descriptors parsed as UAC and an alt matching the requested format was
# found. It is also, as in 7.2, independent interrupt evidence: claim() is
# reachable only via claim_drivers <- enumeration_receive <- followup_Transfer,
# which runs inside USBHost::isr(). There is no polled path to it.
grep -q "DEVICE_CLAIMED=PASS" "$OUT" \
    || { echo "FAIL: the audio driver never bound to a device"; exit 1; }

# 5. A format was negotiated. uac1_find_alt() refuses any rate, channel count or
# bit width the device did not advertise, so a valid alternate setting cannot be
# produced by a code path that merely ran.
grep -q "FORMAT_NEGOTIATED=PASS" "$OUT" \
    || { echo "FAIL: no alternate setting selected -- the device does not offer the requested format, or SET_INTERFACE never completed"; exit 1; }

# 6. The control sequence completed cleanly: nothing outstanding, no watchdog
# timeout, no Transfer_t pool exhaustion. Separate from 5 because they fail
# differently -- a format the device never offered gives alt=-1 with timeouts=0,
# a device that stalled the request gives alt=-1 with timeouts climbing.
grep -q "CONTROL_SEQUENCE=PASS" "$OUT" \
    || { echo "FAIL: the post-claim control sequence did not complete cleanly (see ctrl_state/ctrl_timeouts/ctrl_qfails in the decode line)"; exit 1; }

# 7. The isochronous ring armed: 32 siTDs out of the pool, filled from the tone
# FIFO, linked into all 32 periodic frame slots.
grep -q "STREAM_ARMED=PASS" "$OUT" \
    || { echo "FAIL: beginStreaming() refused -- the iso pool, the frame sizing or the periodic link failed"; exit 1; }

grep -q "AUDIO_CM4=PASS" "$OUT" \
    || { echo "FAIL: the CM4 did not reach an armed isochronous stream"; exit 1; }

# ★ THE WORLD-SPLIT TOKENS: PRESENCE ONLY. See the banner at the top of this
# file. STREAM_PACKETS is FAIL here because nothing moves against QEMU's
# usb-audio model; TRANSPORT_CLEAN is PASS here only because a stream that
# transmitted nothing cannot have had a transmission error. Value-asserting
# either one in this world would be a gate that cannot fail, or one that cannot
# pass. Silicon asserts both.
grep -q "STREAM_PACKETS=" "$OUT" \
    || { echo "FAIL: STREAM_PACKETS token missing entirely"; exit 1; }
grep -q "TRANSPORT_CLEAN=" "$OUT" \
    || { echo "FAIL: TRANSPORT_CLEAN token missing entirely"; exit 1; }

# Vacuity guard, and it works in BOTH worlds: a transcript carrying pkts=0
# alongside STREAM_PACKETS=PASS would mean the verdict logic is broken, not that
# packets flowed. Deliberately NOT the converse check -- pkts>0 with a FAIL
# verdict -- because that is the same statement, and because if QEMU ever does
# start moving isochronous data this gate must go green, not red.
grep -q "pkts=00000000" "$OUT" && grep -q "STREAM_PACKETS=PASS" "$OUT" \
    && { echo "FAIL: pkts is zero yet STREAM_PACKETS passed -- verdict logic is wrong"; exit 1; }

# ★ THE ORACLE. Everything above could in principle be produced by a code path
# that ran without a device answering; these two numbers could not. They are
# QEMU's usb-audio identity (hw/usb/dev-audio.c:43-44) and the CM4 image has no
# knowledge of them whatsoever. The firmware deliberately asserts only vid != 0
# so the identical image serves a silicon run with whatever is in J47 -- the
# device-specific oracle belongs to whoever knows which device is attached.
grep -q "vid=000046F4" "$OUT" \
    || { echo "FAIL: idVendor is not QEMU's usb-audio 46F4 -- descriptors did not come off the wire"; exit 1; }
grep -q "pid=00000002" "$OUT" \
    || { echo "FAIL: idProduct is not QEMU's usb-audio 0002"; exit 1; }
# ALTSET_STEREO is alternate setting 1 in that model (hw/usb/dev-audio.c, the
# ALTSET_OFF/ALTSET_STEREO enum), and it is the only one the single-channel
# variant offers. Asserting the number, not just "some alt", so an off-by-one in
# the parser's alt bookkeeping cannot pass.
grep -q "alt=00000001" "$OUT" \
    || { echo "FAIL: the selected alternate setting is not the model's ALTSET_STEREO (1)"; exit 1; }

echo "PASS: AUDIO_CM4 (control plane; 46F4:0002 claimed, alt 1 negotiated and an iso ring armed by the CM4 -- packet flow is silicon's to prove)"
