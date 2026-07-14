#!/usr/bin/env python3
"""Read the QEMU usbhid-tap socket: capture two 8-byte HID keyboard reports.
Assert press {00 00 04 00 00 00 00 00} then release {00 00 00 00 00 00 00 00}.
Exit 0 on match, 1 on mismatch/short, 2 on connection failure."""
import socket, sys, time

host, port = sys.argv[1], int(sys.argv[2])

# QEMU opens the listening socket at startup; connect with a short retry.
sock = None
deadline = time.time() + 10
while time.time() < deadline:
    try:
        sock = socket.create_connection((host, port), timeout=1)
        break
    except OSError:
        time.sleep(0.2)
if sock is None:
    print("ERROR: could not connect to usbhid-tap socket")
    sys.exit(2)

# The guest waits for enumeration (< 3 s) then sends the two reports ~100 ms apart.
sock.settimeout(8)
got = b""
try:
    while len(got) < 16:
        chunk = sock.recv(64)
        if not chunk:
            break
        got += chunk
except socket.timeout:
    pass

print("got=%r (%d bytes)" % (got, len(got)))
press   = bytes([0, 0, 4, 0, 0, 0, 0, 0])
release = bytes([0, 0, 0, 0, 0, 0, 0, 0])
ok = len(got) >= 16 and got[0:8] == press and got[8:16] == release
sys.exit(0 if ok else 1)
