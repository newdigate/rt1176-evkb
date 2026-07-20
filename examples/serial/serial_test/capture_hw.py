#!/usr/bin/env python3
"""Capture the EVKB MCU-Link VCOM (LPUART1) at 115200 and verify Serial1 output.

Usage: python3 capture_hw.py [/dev/tty.usbmodemXXXX] [seconds]
NOTE: use pyserial (NOT `cat`, which resets the port to 9600 baud on macOS).
"""
import sys, time, glob, serial

port = sys.argv[1] if len(sys.argv) > 1 else None
if not port or "*" in (port or ""):
    matches = sorted(glob.glob("/dev/tty.usbmodem*"))
    if not matches:
        sys.exit("no /dev/tty.usbmodem* found - is the board connected?")
    port = matches[0]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0

s = serial.Serial(port, 115200, timeout=1)
end = time.time() + secs
buf = b""
while time.time() < end:
    buf += s.read(256)
s.close()

text = buf.decode(errors="replace")
sys.stdout.write(text)
ok_banner = "RT1176 Serial1 up" in text
ok_count = "count=" in text
if ok_banner and ok_count:
    print("\nPASS: hardware serial output verified")
    sys.exit(0)
print(f"\nFAIL: banner={ok_banner} counter={ok_count} (port={port})")
sys.exit(1)
