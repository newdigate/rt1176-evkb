#!/bin/sh
# QEMU gate for cm4_graph_usb_capstone (Phase 7.4, the CAPSTONE): does an
# AudioStream GRAPH run on the Cortex-M4 and feed the isochronous USB stream the
# same core drives?
#
# ============================================================================
# ★★ READ THIS BEFORE ADDING A PACKET ASSERTION, OR A GRAPH-CLOCKING ONE.
#    YOU WILL WANT TO ADD BOTH. DO NOT.
# ============================================================================
#
# ISOCHRONOUS DATA DOES NOT FLOW IN QEMU. Measured 2026-08-06 and recorded in
# docs/KNOWN-BROKEN-GATES.md: against the emulated `usb-audio` device an OUT
# sketch enumerates, claims, selects alt 1, completes the whole control sequence
# with ctrl=0/0/0, reports "streaming started" -- and then sits at pkts/s = 0
# forever. The cause is not established; the leading suspicion is a split-
# transaction/TT mismatch, since an siTD is the descriptor for a full-speed
# device behind a high-speed hub and the model has this one on the root port.
# 7.3's PASSING gate carries pkts=0 for exactly this reason.
#
# ★ 7.4 INHERITS A SECOND CONSEQUENCE OF THAT, and it is the trap specific to
# this gate. The graph's clock IS packet flow: AudioOutputUSBHost registers a
# callback with USBAudioOut::onFrameConsumed(), and the driver calls it each
# time service() arms a ring slot. No packets => no callbacks => the graph is
# never clocked. So `gpasses` is 0 here, and that is not a broken graph.
#
# The graph is therefore proved SEPARATELY, in a way that is world-independent:
# at stage 8 the CM4 image pends IRQ_SOFTWARE itself until the FIFO reaches the
# adapter's own setpoint. That is pure CPU -- the real AudioStream engine, the
# real sine, the real adapter, the real FIFO -- with only the tick supplied
# locally, and it is what this gate asserts.
#
# FOUR world-split tokens, all PRESENCE-checked here and value-asserted only on
# silicon:
#   STREAM_PACKETS   nothing moves against the emulated device
#   GRAPH_CLOCKED    consequence of the above
#   TRANSPORT_CLEAN  vacuously PASS: a stream that sent nothing cannot have had
#                    a transmission error
#   STREAM_FED       vacuously PASS: nothing was consumed, so the producer
#                    cannot have failed to keep up
# Value-asserting the first two would make the gate unpassable; value-asserting
# the last two would be a check that cannot fail, which this tree treats as
# worse than no check at all. Same idiom, and the same reasoning, as
# PHY_PLL_CM4 in 7.1 -- a mistake that has now cost this phase twice.
#
# What the gate DOES prove, and what earns it a place in the sweep:
#
#   - the CM4 image links the AudioStream engine, the Audio library's USB-host
#     sink, a sine source and a peak analyser ON TOP of 7.3's whole USB stack,
#     and still boots with ~168 KB of DMA-reachable rings in OCRAM2 and the
#     audio block pool in DTCM;
#   - a real device is CLAIMED by the UAC driver on the M4 (bound + vid/pid);
#   - the rate on the wire was set BY THE ADAPTER, not by the sketch: main_cm4
#     never calls format(), so `rate` matching the build's PROBE_RATE_HZ proves
#     AudioOutputUSBHost's constructor ran and did its one job;
#   - the post-claim CONTROL SEQUENCE completes and 32 siTDs arm;
#   - THE GRAPH RUNS ON THE M4 and its blocks reach the USB FIFO: passes
#     happened, every adapter write was accepted (drop=0), and the FIFO reached
#     the setpoint, which it can only do by way of
#     AudioOutputUSBHost::update();
#   - THE BLOCKS CARRY AUDIO, not silence: the peak tap read non-zero. Every
#     other number here would look identical if the sine produced zeros;
#   - the block pool is bounded (no leak) and the measured interrupt priorities
#     put USB above the graph.
#
# THE FORMAT IS 48000/2/16 AND THAT IS NOT A PREFERENCE. QEMU's usb-audio bakes
# USBAUDIO_SAMPLE_RATE=48000 into its descriptor tables as a compile-time
# #define (hw/usb/dev-audio.c:118) with no property to override it. Here that
# rate also drives the GRAPH: AudioOutputUSBHost forces the USB rate to
# AUDIO_SAMPLE_RATE_EXACT, so CMake's PROBE_RATE_HZ sets both together (a
# silicon run against the 44.1 kHz dongle uses -DPROBE_RATE_HZ=44100).
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
ELF="$DIR/$(gate_build_dir)/cm4_graph_usb_capstone.elf"
OUT="$DIR/cm4_graph_usb.uart"
MON="$DIR/mon.sock"
rm -f "$OUT" "$MON"
gate_tmp "$MON"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_graph_usb.dbg" \
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

# Same budget as 7.3: a fixed 4 s streaming window after the claim window, on a
# CM4 the machine clocks at 400 MHz (fsl-imxrt1170.c:365) with CYCCNT derived
# from the virtual clock. The graph priming between them is bounded to 64
# passes and costs nothing measurable.
for _ in $(seq 1 200); do
    [ -f "$OUT" ] && grep -q "CM4GRAPHUSB-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"

echo "==== captured ===="; cat "$OUT"

grep -q "CM4GRAPHUSB-DONE" "$OUT" || { echo "FAIL: no done marker"; exit 1; }

# Assertions ordered so a red run LOCALISES itself. These are distinct faults
# and must not collapse into one FAIL.
grep -q "STAGES=PASS" "$OUT" \
    || { echo "FAIL: the CM4 did not reach all nine stage markers"; exit 1; }

# 1. DWT. First, and a MEASURED cycle delta rather than a register readback. In
# THIS image a dead cycle counter breaks three things at once: every
# USBHost_t36 timeout, the audio driver's control watchdog, and software_isr's
# own cost accounting -- so it would report a FREE graph, which is the most
# misleading failure available to this measurement.
grep -q "DWT_ALIVE=PASS" "$OUT" \
    || { echo "FAIL: the CM4 cycle counter is dead -- USBHost_t36 timeouts, the audio control watchdog and the graph's cost accounting are all frozen"; exit 1; }
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

# 4. The audio driver was BOUND to the device. claim() is reachable only via
# claim_drivers <- enumeration_receive <- followup_Transfer, which runs inside
# USBHost::isr(); there is no polled path to it, so this is also independent
# evidence that IRQ 135 dispatched on the CM4's OWN NVIC.
grep -q "DEVICE_CLAIMED=PASS" "$OUT" \
    || { echo "FAIL: the audio driver never bound to a device"; exit 1; }

# 5. A format was negotiated. uac1_find_alt() refuses any rate, channel count or
# bit width the device did not advertise, so a valid alternate setting cannot be
# produced by a code path that merely ran.
grep -q "FORMAT_NEGOTIATED=PASS" "$OUT" \
    || { echo "FAIL: no alternate setting selected -- the device does not offer the requested format, or SET_INTERFACE never completed"; exit 1; }

# 6. The control sequence completed cleanly. Separate from 5 because they fail
# differently -- a format the device never offered gives alt=-1 with timeouts=0,
# a device that stalled the request gives alt=-1 with timeouts climbing.
grep -q "CONTROL_SEQUENCE=PASS" "$OUT" \
    || { echo "FAIL: the post-claim control sequence did not complete cleanly (see ctrl_state/ctrl_timeouts/ctrl_qfails in the decode line)"; exit 1; }

# 7. The isochronous ring armed: 32 siTDs out of the pool, filled and linked
# into all 32 periodic frame slots.
grep -q "STREAM_ARMED=PASS" "$OUT" \
    || { echo "FAIL: beginStreaming() refused -- the iso pool, the frame sizing or the periodic link failed"; exit 1; }

# ---- 7.4's own assertions start here ------------------------------------

# 8. THE ADAPTER SET THE RATE. main_cm4.cpp deliberately never calls format();
# AudioOutputUSBHost's constructor does, from the graph's AUDIO_SAMPLE_RATE_EXACT.
# A mismatch means the adapter was not constructed, or the build handed the
# graph and the probe different rates -- the defect that puts a 44100 graph into
# a 48000 stream and drains the FIFO for good.
grep -q "GRAPH_RATE=PASS" "$OUT" \
    || { echo "FAIL: the USB rate is not the graph's rate -- AudioOutputUSBHost's constructor did not run, or the build's PROBE_RATE_HZ and AUDIO_SAMPLE_RATE_EXACT disagree"; exit 1; }

# 9. THE GRAPH RAN, ON THE M4, AND ITS BLOCKS REACHED THE USB FIFO. Passes
# happened (so vector index 60 dispatches and software_isr executes), every
# adapter write was accepted, and the FIFO reached the adapter's setpoint --
# which it can only do through AudioOutputUSBHost::update() copying blocks in.
grep -q "GRAPH_ALIVE=PASS" "$OUT" \
    || { echo "FAIL: the graph did not run, or its blocks did not reach the USB FIFO (see the graph: line -- blocks/fifo/dropped)"; exit 1; }
# Vacuity guard: a transcript with zero graph passes and a passing verdict would
# mean the verdict logic is broken, not that the graph ran.
grep -q "gblocks=00000000" "$OUT" && grep -q "GRAPH_ALIVE=PASS" "$OUT" \
    && { echo "FAIL: gblocks is zero yet GRAPH_ALIVE passed -- verdict logic is wrong"; exit 1; }

# 10. AND THE BLOCKS CARRIED AUDIO. This is the one assertion nothing else here
# can substitute for: blocks flowing, a FIFO filling and packets leaving all
# look identical whether the samples are a sine or zeros. peak is q15 of the
# largest |sample| the tap saw, and the graph asks for amplitude 0.5.
grep -q "GRAPH_AUDIO=PASS" "$OUT" \
    || { echo "FAIL: no audio reached the sink -- peak=FFFFFFFF means the tap analysed NO block (a broken graph), peak=0 means blocks arrived carrying SILENCE (a broken source). See the graph: line."; exit 1; }
# Two vacuity guards, one per silent shape, because they are different faults.
grep -q "peak=00000000" "$OUT" && grep -q "GRAPH_AUDIO=PASS" "$OUT" \
    && { echo "FAIL: peak is zero yet GRAPH_AUDIO passed -- verdict logic is wrong"; exit 1; }
grep -q "peak=FFFFFFFF" "$OUT" && grep -q "GRAPH_AUDIO=PASS" "$OUT" \
    && { echo "FAIL: peak is the no-data sentinel yet GRAPH_AUDIO passed -- verdict logic is wrong"; exit 1; }

# 11. The block pool is BOUNDED, not drained: steady state is exactly one live
# block, because update order is construction order and the sink was constructed
# before the source. memmax=0 would mean nothing was ever allocated; a climbing
# memuse would mean a node stopped releasing.
grep -q "GRAPH_NOLEAK=PASS" "$OUT" \
    || { echo "FAIL: the audio block pool is not bounded -- a node is not releasing, or nothing ever allocated (see mem=used/max)"; exit 1; }

# 12. The measured interrupt-priority polarity: USB above the graph. Neither
# number is set by this firmware -- AudioStream::update_setup() chose 208 for
# IRQ_SOFTWARE and USBHost::begin() left IRQ 135 at its reset value -- so this
# reads back what the libraries actually did on this core.
grep -q "PRIORITY_SPLIT=PASS" "$OUT" \
    || { echo "FAIL: the graph interrupt outranks USB -- enumeration and control transfers would be preempted by graph work"; exit 1; }

grep -q "CAPSTONE_CM4=PASS" "$OUT" \
    || { echo "FAIL: the CM4 did not reach an armed isochronous stream fed by a live audio graph"; exit 1; }

# ★ THE FOUR WORLD-SPLIT TOKENS: PRESENCE ONLY. See the banner at the top of
# this file before touching any of these four lines.
for t in STREAM_PACKETS GRAPH_CLOCKED TRANSPORT_CLEAN STREAM_FED; do
    grep -q "$t=" "$OUT" || { echo "FAIL: $t token missing entirely"; exit 1; }
done

# Vacuity guards for the two that read FAIL here, and they work in BOTH worlds:
# a transcript carrying the zero alongside a PASS verdict would mean the verdict
# logic is broken, not that anything flowed. Deliberately NOT the converse check
# -- that is the same statement, and if QEMU ever does start moving isochronous
# data this gate must go GREEN, not red.
grep -q "pkts=00000000" "$OUT" && grep -q "STREAM_PACKETS=PASS" "$OUT" \
    && { echo "FAIL: pkts is zero yet STREAM_PACKETS passed -- verdict logic is wrong"; exit 1; }
grep -q "gpasses=00000000" "$OUT" && grep -q "GRAPH_CLOCKED=PASS" "$OUT" \
    && { echo "FAIL: gpasses is zero yet GRAPH_CLOCKED passed -- verdict logic is wrong"; exit 1; }

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

echo "PASS: CAPSTONE_CM4 (46F4:0002 claimed at the graph's own rate, an iso ring armed, and an AudioStream graph running on the M4 feeding real audio into its FIFO -- packet flow and USB-clocked graph passes are silicon's to prove)"
