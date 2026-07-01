#!/usr/bin/env python3
"""Read the LPADC A0 value stream over the VCOM; report the readings.
Usage: python3 adc_hw.py [/dev/tty.usbmodemXXXX] [seconds]
Tie GPIO_AD_06 (A0, LPADC1 ch0) to GND -> ~0, to 3V3 -> ~max (1023 at 10-bit).
"""
import sys, time, glob, re, serial
port = sys.argv[1] if len(sys.argv) > 1 else sorted(glob.glob("/dev/tty.usbmodem*"))[0]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
s = serial.Serial(port, 115200, timeout=1)
end = time.time() + secs
buf = b""
while time.time() < end:
    buf += s.read(256)
s.close()
sys.stdout.write(buf.decode(errors="replace"))
vals = [int(m) for m in re.findall(rb"A0=(\d+)", buf)]
if vals:
    print(f"\nA0 readings: n={len(vals)} min={min(vals)} max={max(vals)} last={vals[-1]}")
    sys.exit(0)
print("\nFAIL: no A0 reading seen")
sys.exit(1)
