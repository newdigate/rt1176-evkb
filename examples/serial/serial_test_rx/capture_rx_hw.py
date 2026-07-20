#!/usr/bin/env python3
"""Hardware RX-echo test: send 'hello\\n' over the VCOM, assert echo + rx_isr>0.

Usage: python3 capture_rx_hw.py [/dev/tty.usbmodemXXXX]
Requires the board to be running the serial_test_rx firmware (power-cycle after
flash if it sits halted). Uses pyserial (NOT `cat`, which resets baud to 9600).
"""
import sys, time, glob, re, serial

port = sys.argv[1] if len(sys.argv) > 1 else None
if not port:
    ms = sorted(glob.glob("/dev/tty.usbmodem*"))
    if not ms:
        sys.exit("no /dev/tty.usbmodem* found - is the board connected?")
    port = ms[0]

s = serial.Serial(port, 115200, timeout=1)
time.sleep(0.2)
s.reset_input_buffer()
s.write(b"hello\n")           # board echoes via serialEvent1 and reports [status]
end = time.time() + 6
buf = b""
while time.time() < end:
    buf += s.read(256)
s.close()

sys.stdout.write(buf.decode(errors="replace"))
echo_ok = b"hello" in buf
m = None
for line in buf.splitlines():
    mm = re.search(rb"\[status\] rx_isr=(\d+) echoed=(\d+)", line)
    if mm:
        m = mm
rx_isr = int(m.group(1)) if m else 0
if echo_ok and rx_isr > 0:
    print(f"\nPASS: hardware RX echo verified (rx_isr={rx_isr})")
    sys.exit(0)
print(f"\nFAIL: echo={echo_ok} rx_isr={rx_isr} (port={port})")
sys.exit(1)
