#!/bin/sh
# QEMU gate for cm4_usb_enum_probe (Phase 7.2c): does the REAL USBHost_t36
# stack run on the CM4 and enumerate a device?
#
# 7.1 (cm4_usb_irq_probe) proved the CM4 can bring the port up by hand and take
# IRQ 135 on its own NVIC. This asserts the rest: the library itself compiled
# into a freestanding CM4 image, fetching a device's descriptors and reporting
# its VID/PID.
#
# ★ THE VID/PID IS THE UN-FAKEABLE PART. Every other token in this gate is
# something the firmware could in principle produce without a device on the
# bus -- a register it wrote and read back, a counter it incremented. 46F4:0002
# is QEMU's emulated usb-audio device (hw/usb/dev-audio.c:43-44,
# "CRC16() of QEMU"), and nothing in the CM4 image knows those numbers. They can
# only have arrived in a GET_DESCRIPTOR data stage, over the wire, through DMA
# the EHCI controller performed into OCRAM2, completed by an interrupt
# dispatched on the CM4's own NVIC. Assert them exactly.
#
# THE DEVICE IS HOTPLUGGED, NOT PRESENT AT RESET, and that is load-bearing --
# inherited from 7.1, where it was learned the expensive way. ChipIdea defers
# the attach report to the guest's PP write (chipidea.c:230 -> hcd-ehci.c:1066),
# so a device present from reset raises the port-change condition while
# USBHost::begin() is still running. This gate waits for the CM4's stage-3
# marker -- "armed, port powered, nothing connected" -- and only then
# device_adds, so the connect edge happens with the stack listening. That is
# also the same physical event the silicon probe asserts (operator plugs J47),
# and PLUG_TRANSITION machine-checks the 0->1 edge in both worlds.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/cm4_usb_enum_probe.elf"
OUT="$DIR/cm4_usb_enum.uart"
MON="$DIR/mon.sock"
rm -f "$OUT" "$MON"
gate_tmp "$MON"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_usb_enum.dbg" \
    -audiodev none,id=snd0 \
    -monitor unix:"$MON",server,nowait &
P=$!; gate_pid $P

# Wait for the CM4 to finish USBHost::begin() and settle (stage 3), THEN plug.
# Polling for the token rather than sleeping a fixed interval: the emulator does
# not run at wall-clock rate, so a fixed offset would be fragile.
for _ in $(seq 1 160); do
    [ -f "$OUT" ] && grep -q "s3=E5B00003" "$OUT" 2>/dev/null && break
    sleep 0.25
done
# python3, not nc: -U (unix socket) support varies across macOS versions, and
# python3 is already a dependency of this repo's tools.
printf 'device_add usb-audio,bus=usbhost.0,port=1,audiodev=snd0\n' \
    | python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(sys.stdin.buffer.read()); s.close()' "$MON"

for _ in $(seq 1 160); do
    [ -f "$OUT" ] && grep -q "CM4USBENUM-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"

echo "==== captured ===="; cat "$OUT"

grep -q "CM4USBENUM-DONE" "$OUT" || { echo "FAIL: no done marker"; exit 1; }

# Assertions ordered so a red run LOCALISES itself. These are five different
# faults and must not collapse into one FAIL:
#
#   1. a frozen cycle counter (every USBHost_t36 timeout silently never fires)
#   2. an interrupt mask too narrow to complete a control transfer
#   3. a device that never arrived on the port
#   4. an interrupt that arrived but never dispatched through vector slot 151
#   5. a descriptor that never came back
grep -q "STAGES=PASS" "$OUT" \
    || { echo "FAIL: the CM4 did not reach all four stage markers"; exit 1; }

# 1. DWT. Deliberately first, and deliberately a MEASURED cycle delta rather
# than a register readback: if DEMCR.TRCENA/DWT_CTRL.CYCCNTENA do not take,
# ARM_DWT_CYCCNT reads 0 forever, so millis()/micros() stand still and every
# timeout in the stack waits forever. Enumeration would not fail -- it would
# never finish and never give up. That is the single most expensive silent
# failure in this port, so it gets its own token before anything else.
grep -q "DWT_ALIVE=PASS" "$OUT" \
    || { echo "FAIL: the CM4 cycle counter is dead -- every USBHost_t36 timeout is frozen"; exit 1; }
grep -q "dwt=00000000" "$OUT" \
    && { echo "FAIL: dwt is zero yet DWT_ALIVE passed -- verdict logic is wrong"; exit 1; }

# 2. USBINTR. 7.1 armed PCE alone, which is enough to SEE a plug and not enough
# to enumerate: every control transfer completes through UAI ->
# followup_Transfer (ehci.cpp:380). USBHost::begin() sets the full mask itself
# (ehci.cpp:326-328); this asserts the image did not narrow it afterwards.
grep -q "USBINTR_UAIE=PASS" "$OUT" \
    || { echo "FAIL: UAIE not armed -- control transfers cannot complete"; exit 1; }

# 3. A 0->1 CONNECT TRANSITION during the window, not merely a device being
# present at the end of it. A device already attached at reset, and one that
# never arrives, both fail here -- the same un-fakeability the silicon probe has.
grep -q "PLUG_TRANSITION=PASS" "$OUT" \
    || { echo "FAIL: no 0->1 connect transition during the observation window"; exit 1; }

# 4. claim() was called. ★ This is the interrupt assertion. claim() is reachable
# only through claim_drivers <- enumeration_receive <- followup_Transfer, which
# runs inside USBHost::isr(); there is no polled path to it. A non-zero count
# therefore proves IRQ 135 reached the CM4's own NVIC AND that vector slot 151
# dispatched to the library's ISR rather than to Default_Handler.
grep -q "CLAIM_CALLED=PASS" "$OUT" \
    || { echo "FAIL: claim() never called -- the ISR path did not close (check stsraw: bits set = the condition arose but was never serviced)"; exit 1; }

# 5. Descriptors came back.
grep -q "DESCRIPTOR_FETCHED=PASS" "$OUT" \
    || { echo "FAIL: no device descriptor was fetched (vid=0)"; exit 1; }

grep -q "ENUM_CM4=PASS" "$OUT" \
    || { echo "FAIL: the CM4 did not enumerate a device"; exit 1; }

# ★ THE ORACLE. Everything above could in principle be produced by a code path
# that ran without a device answering; these two numbers could not. They are
# QEMU's usb-audio identity (hw/usb/dev-audio.c:43-44) and the CM4 image has no
# knowledge of them whatsoever.
#
# The firmware deliberately does NOT assert them -- it asserts vid != 0 -- so
# that the identical image serves the silicon probe with a real device on J47.
# The device-specific oracle belongs to whoever knows which device is attached,
# which here is the gate.
grep -q "vid=000046F4" "$OUT" \
    || { echo "FAIL: idVendor is not QEMU's usb-audio 46F4 -- descriptors did not come off the wire"; exit 1; }
grep -q "pid=00000002" "$OUT" \
    || { echo "FAIL: idProduct is not QEMU's usb-audio 0002"; exit 1; }

echo "PASS: ENUM_CM4 (46F4:0002 enumerated by the real USBHost_t36 stack on the CM4)"
