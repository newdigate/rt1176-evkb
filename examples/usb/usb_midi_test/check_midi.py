#!/usr/bin/env python3
# Marker checker for the RT1176 USB MIDI host gate (bidirectional MIDI).
#
# The gate runs the firmware once against the virtual usb-midi device on the
# OTG2 root port and proves MIDI travels *both* directions over the bulk pipes:
#
#   device -> host (the usb-midi bulk IN emits a fixed note sequence, which the
#   guest's midi1.read() decodes):
#       USB_MIDI_BEGIN            host controller came up (myusb.begin())
#       MIDI_CONNECT=<vid>:<pid>  the usb-midi device enumerated (claim() parsed
#                                 the MIDIStreaming descriptors); a non-zero VID
#                                 is required so a 0:0 phantom can't pass
#       NOTE_ON=1,60,100          note-on  (09 90 3C 64) reached the interrupt/
#                                 bulk IN pipe and decoded
#       NOTE_OFF=1,60,0           note-off (08 80 3C 00) decoded
#
#   host -> device (midi1.sendNoteOn/Off, flushed by the driver's USBDriverTimer
#   over the ChipIdea GP timer, then decoded by the usb-midi bulk OUT handler):
#       MIDI_SENT                 the host issued its sendNoteOn/Off (UART side)
#       VMIDI=PASS                the device decoded the host's note-on 90 3C 64
#                                 (guest_errors / dbg side) -- proves host TX
#
# Usage: check_midi.py <uart_marker_file> <dbg_file>
# Prints USB_MIDI=PASS iff every marker is present, else USB_MIDI=FAIL and the
# list of missing markers.  Exit 0 on PASS, 1 otherwise.

import re
import sys


def read(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except OSError as e:
        print(f"  cannot read {path}: {e}")
        return ""


def main():
    if len(sys.argv) != 3:
        print("usage: check_midi.py <uart_marker_file> <dbg_file>")
        return 2

    uart = read(sys.argv[1])
    dbg = read(sys.argv[2])

    missing = []

    # device -> host: enumeration + the decoded note sequence (UART side).
    if "USB_MIDI_BEGIN" in uart:
        print("  USB_MIDI_BEGIN seen (host controller up)")
    else:
        missing.append("USB_MIDI_BEGIN")

    m = re.search(r"MIDI_CONNECT=([0-9a-fA-F]+):([0-9a-fA-F]+)", uart)
    if m:
        vid, pid = int(m.group(1), 16), int(m.group(2), 16)
        print(f"  MIDI_CONNECT={vid:04x}:{pid:04x} (usb-midi enumerated)")
        if vid == 0:
            print("  FAIL: MIDI_CONNECT VID is zero (phantom, not enumerated)")
            missing.append("MIDI_CONNECT(non-zero-vid)")
    else:
        missing.append("MIDI_CONNECT=<vid>:<pid>")

    for marker in ("NOTE_ON=1,60,100", "NOTE_OFF=1,60,0"):
        if marker in uart:
            print(f"  {marker} seen (device->host bulk IN decoded)")
        else:
            missing.append(marker)

    if "MIDI_SENT" in uart:
        print("  MIDI_SENT seen (host issued sendNoteOn/Off)")
    else:
        missing.append("MIDI_SENT")

    # host -> device: the device decoded the host's note-on (dbg side).
    if "VMIDI=PASS" in dbg:
        print("  VMIDI=PASS seen (host->device bulk OUT decoded on the device)")
    else:
        missing.append("VMIDI=PASS")

    if missing:
        print("USB_MIDI=FAIL missing: " + ", ".join(missing))
        return 1
    print("USB_MIDI=PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
