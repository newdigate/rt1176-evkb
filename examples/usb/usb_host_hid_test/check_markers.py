#!/usr/bin/env python3
# Marker checker for the RT1176 USB Host gate (enumeration + HID input).
#
# The gate runs the firmware once per device (single-port OTG2 root hub can't
# host several at once, see run_qemu_usbhost.sh); each run also injects one HID
# event over QMP after the device enumerates (see inject_qmp.py):
#   run A  usb-kbd (high speed)           -> KBD_CONNECT   + KEY=<n>
#   run B  usb-mouse (high speed)         -> MOUSE_CONNECT + MOUSE=<dx>,<dy>,<btn>
#   run C  usb-kbd,usb_version=1 (full)   -> KBD_CONNECT   + KEY=<n>
# Run C exercises the companion-less FS/LS root-port path a real keyboard uses.
#
# Two independent properties are proven:
#   USB_HOST_ENUMERATE  the *_CONNECT line (control transfers / async schedule).
#                       The firmware prints it only after reading the device's
#                       descriptor (idVendor/idProduct); a non-zero VID is
#                       required so a 0:0 phantom can't pass.
#   USB_HOST_INPUT      the KEY=/MOUSE= line (interrupt-IN / periodic schedule).
#                       The firmware prints it only when an injected HID report
#                       is delivered through the interrupt pipe -- a path
#                       enumeration alone never touches.
#
# Usage: check_markers.py <kbd_hs_uart> <mouse_hs_uart> <kbd_fs_uart>
# Prints USB_HOST_ENUMERATE=PASS and USB_HOST_INPUT=PASS iff every run proved
# both; exit 0 only when both PASS.

import re
import sys

BEGIN = "USB_HOST_BEGIN"

# label -> (connect keyword, input regex). The keyboard runs prove input with
# KEY=<int> (onPress fired); the mouse run with MOUSE=<dx>,<dy>,<btn>.
KEY_RE = re.compile(r"KEY=(-?\d+)")
MOUSE_RE = re.compile(r"MOUSE=(-?\d+),(-?\d+),([0-9a-fA-F]+)")


def check(path, label, connect_kw, input_kind):
    """Return (enumerate_ok, input_ok) for one run's marker file."""
    try:
        with open(path, "r", errors="replace") as f:
            text = f.read()
    except OSError as e:
        print(f"FAIL[{label}]: cannot read {path}: {e}")
        return False, False

    enum_ok = True
    if BEGIN not in text:
        print(f"FAIL[{label}]: missing {BEGIN} (host controller never came up)")
        enum_ok = False
    else:
        print(f"  [{label}] {BEGIN} seen")

    m = re.search(connect_kw + r"=([0-9a-fA-F]+):([0-9a-fA-F]+)", text)
    if not m:
        print(f"FAIL[{label}]: missing {connect_kw}=<vid>:<pid> (never enumerated)")
        enum_ok = False
    else:
        vid, pid = int(m.group(1), 16), int(m.group(2), 16)
        print(f"  [{label}] {connect_kw}={vid:04x}:{pid:04x}")
        if vid == 0:
            print(f"FAIL[{label}]: {connect_kw} VID is zero (phantom, not enumerated)")
            enum_ok = False

    input_ok = True
    if input_kind == "kbd":
        k = KEY_RE.search(text)
        if not k:
            print(f"FAIL[{label}]: missing KEY=<n> (injected keypress not delivered)")
            input_ok = False
        else:
            print(f"  [{label}] KEY={k.group(1)}")
    else:  # mouse
        k = MOUSE_RE.search(text)
        if not k:
            print(f"FAIL[{label}]: missing MOUSE=<dx>,<dy>,<btn> (injected report not delivered)")
            input_ok = False
        else:
            print(f"  [{label}] MOUSE={k.group(1)},{k.group(2)},{k.group(3)}")
    return enum_ok, input_ok


def main():
    if len(sys.argv) != 4:
        print("usage: check_markers.py <kbd_hs_uart> <mouse_hs_uart> <kbd_fs_uart>")
        return 2
    a_enum, a_in = check(sys.argv[1], "kbd/HS", "KBD_CONNECT", "kbd")
    b_enum, b_in = check(sys.argv[2], "mouse/HS", "MOUSE_CONNECT", "mouse")
    c_enum, c_in = check(sys.argv[3], "kbd/FS", "KBD_CONNECT", "kbd")

    enum_pass = a_enum and b_enum and c_enum
    input_pass = a_in and b_in and c_in
    print("USB_HOST_ENUMERATE=" + ("PASS" if enum_pass else "FAIL"))
    print("USB_HOST_INPUT=" + ("PASS" if input_pass else "FAIL"))
    return 0 if (enum_pass and input_pass) else 1


if __name__ == "__main__":
    sys.exit(main())
