#!/usr/bin/env python3
# Marker checker for the RT1176 USB Host enumeration gate.
#
# The gate runs the firmware once per device (single-port OTG2 root hub can't
# host several at once, see run_qemu_usbhost.sh):
#   run A  usb-kbd (high speed)           -> USB_HOST_BEGIN + KBD_CONNECT
#   run B  usb-mouse (high speed)         -> USB_HOST_BEGIN + MOUSE_CONNECT
#   run C  usb-kbd,usb_version=1 (full)   -> USB_HOST_BEGIN + KBD_CONNECT
# Run C exercises the companion-less FS/LS root-port path a real keyboard uses.
#
# Enumeration is proven by the *_CONNECT line, which the firmware only prints
# after the host stack has read the device's descriptor (idVendor/idProduct).
# A non-zero VID is required so a 0:0 phantom can't pass.
#
# Usage: check_markers.py <kbd_hs_uart> <mouse_hs_uart> <kbd_fs_uart>
# Prints USB_HOST_ENUMERATE=PASS iff all runs enumerated; exit 0 on pass.

import re
import sys

BEGIN = "USB_HOST_BEGIN"


def check(path, label, connect_kw):
    try:
        with open(path, "r", errors="replace") as f:
            text = f.read()
    except OSError as e:
        print(f"FAIL[{label}]: cannot read {path}: {e}")
        return False

    ok = True
    if BEGIN not in text:
        print(f"FAIL[{label}]: missing {BEGIN} (host controller never came up)")
        ok = False
    else:
        print(f"  [{label}] {BEGIN} seen")

    m = re.search(connect_kw + r"=([0-9a-fA-F]+):([0-9a-fA-F]+)", text)
    if not m:
        print(f"FAIL[{label}]: missing {connect_kw}=<vid>:<pid> (never enumerated)")
        ok = False
    else:
        vid, pid = int(m.group(1), 16), int(m.group(2), 16)
        print(f"  [{label}] {connect_kw}={vid:04x}:{pid:04x}")
        if vid == 0:
            print(f"FAIL[{label}]: {connect_kw} VID is zero (phantom, not enumerated)")
            ok = False
    return ok


def main():
    if len(sys.argv) != 4:
        print("usage: check_markers.py <kbd_hs_uart> <mouse_hs_uart> <kbd_fs_uart>")
        return 2
    a = check(sys.argv[1], "kbd/HS", "KBD_CONNECT")
    b = check(sys.argv[2], "mouse/HS", "MOUSE_CONNECT")
    c = check(sys.argv[3], "kbd/FS", "KBD_CONNECT")
    if a and b and c:
        print("USB_HOST_ENUMERATE=PASS")
        return 0
    print("USB_HOST_ENUMERATE=FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
