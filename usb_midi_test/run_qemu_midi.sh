#!/bin/sh
# RT1176 USB MIDI host gate (USB_OTG2 / usbhost.0): bidirectional MIDI.
#
# The firmware brings up the ChipIdea EHCI in host mode, enumerates the virtual
# usb-midi device, and exercises MIDI *both* directions over its bulk pipes,
# printing markers over Serial1 (LPUART1 = serial_hd(0) = first -serial):
#   USB_MIDI_BEGIN                 after myusb.begin()
#   MIDI_CONNECT=<vid>:<pid>       when the usb-midi device enumerates
#                                  (MIDIDeviceBase::claim() parsed the MS descs)
#   NOTE_ON=1,60,100               device->host: the fixed note-on the usb-midi
#   NOTE_OFF=1,60,0                bulk IN emits, decoded by midi1.read()
#   MIDI_SENT                      after midi1.sendNoteOn/Off (host->device TX)
# and on the device side (guest_errors log, -D below):
#   VMIDI: RX ..                   each USB-MIDI event the bulk OUT decoded
#   VMIDI=PASS                     the device saw the host's note-on 90 3C 64
#
# Two independent properties, one per direction:
#   IN  (device->host): NOTE_ON/NOTE_OFF prove the bulk/interrupt IN pipe carries
#                       the device's emitted events into midi1.read().
#   OUT (host->device): VMIDI=PASS proves midi1.sendNoteOn(60,100,1) reached the
#                       device.  The driver buffers the two events and flushes
#                       them from its USBDriverTimer, which is the ChipIdea USB
#                       GP timer 1 (GPTIMER1 -> USBSTS.TI1) -- so this run also
#                       exercises that timer end to end.
#
# One run: the single-port OTG2 root hub hosts the one usb-midi device, pinned to
# the root port with port=1.  Without it QEMU's convenience auto-hub (nfree==1)
# inserts a full-speed usb-hub ahead of the device -- not how the single physical
# root port behaves; the HID gate pins it for the same reason.  -icount shift=auto
# keeps the EHCI frame timer and the GP timer deterministic against the firmware.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/usb_midi_test.elf"
OUT="$DIR/midi.uart"; DBG="$DIR/midi.dbg"
rm -f "$OUT" "$DBG"

"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto -display none \
    -serial file:"$OUT" -d guest_errors -D "$DBG" \
    -device usb-midi,bus=usbhost.0,port=1 &
P=$!; gate_pid $P

# Wait (bounded) until both directions are proven -- MIDI_SENT on the UART (host
# TX issued) and VMIDI=PASS in the dbg (the device decoded it) -- or a wall-clock
# deadline.  Break early on success so a green run is fast; the deadline bounds a
# failing run.  Python's sleep is used (not a shell poll) for a tight, portable loop.
python3 - "$OUT" "$DBG" 25 <<'PYWAIT'
import sys, time
out, dbg, secs = sys.argv[1], sys.argv[2], int(sys.argv[3])
deadline = time.time() + secs
def has(path, needle):
    try:
        with open(path, errors="replace") as f:
            return needle in f.read()
    except OSError:
        return False
while time.time() < deadline:
    if has(out, "MIDI_SENT") and has(dbg, "VMIDI=PASS"):
        break
    time.sleep(0.5)
PYWAIT

kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== usb-midi UART (LPUART1) ===="; cat "$OUT"
echo "==== usb-midi guest_errors (VMIDI) ===="; grep "VMIDI" "$DBG" 2>/dev/null || echo "(no VMIDI lines)"
python3 "$DIR/check_midi.py" "$OUT" "$DBG"
