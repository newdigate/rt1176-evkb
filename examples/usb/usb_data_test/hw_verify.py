#!/usr/bin/env python3
"""Thorough HW proof: 3 varied-payload echo round-trips on the native-USB CDC
port, while capturing the firmware's ECHOED debug markers on the MCU-Link VCOM."""
import serial, threading, time, sys

NATIVE = sys.argv[1] if len(sys.argv) > 1 else '/dev/cu.usbmodem14534401'
VCOM   = sys.argv[2] if len(sys.argv) > 2 else '/dev/cu.usbmodem5DQ2DDHVWO5EI3'

vcom_lines = []
stop = False
def reader():
    try:
        v = serial.Serial(VCOM, 115200, timeout=0.3)
    except Exception as e:
        vcom_lines.append("VCOM open error: %r" % e); return
    while not stop:
        ln = v.readline().decode(errors='replace').strip()
        if ln:
            vcom_lines.append(ln)
threading.Thread(target=reader, daemon=True).start()

s = serial.Serial(NATIVE, 115200, timeout=2)
time.sleep(0.3); s.reset_input_buffer()
ok = True
for i, payload in enumerate([b"HELLO-RT1176\n", b"phase2-bulk-data-path\n", b"0123456789ABCDEF\n"]):
    s.write(payload); s.flush()
    got = b""; t = time.time()
    while len(got) < len(payload) and time.time() - t < 3:
        got += s.read(len(payload) - len(got))
    match = (got == payload)
    ok = ok and match
    print("[%d] len=%2d sent=%-24r got=%-24r %s" % (i, len(payload), payload, got, "OK" if match else "MISMATCH"))
    time.sleep(0.2)

stop = True
time.sleep(0.6)
print("--- firmware VCOM markers (last few) ---")
for ln in vcom_lines[-8:]:
    print("VCOM:", ln)
print("RESULT:", "HW ECHO PASS" if ok else "HW ECHO FAIL")
sys.exit(0 if ok else 1)
