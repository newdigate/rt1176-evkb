#!/usr/bin/env python3
"""Open the native-USB CDC port, send bytes, confirm the firmware echoes them."""
import serial, sys, time

port = sys.argv[1]
msg  = b"PHASE2-HW-ECHO\n"
s = serial.Serial(port, 115200, timeout=2)
time.sleep(0.2)
s.reset_input_buffer()
s.write(msg)
s.flush()
got = b""
t = time.time()
while len(got) < len(msg) and time.time() - t < 3:
    got += s.read(len(msg) - len(got))
print("sent=%r got=%r" % (msg, got))
sys.exit(0 if got.strip() == msg.strip() else 1)
