#!/usr/bin/env python3
"""Read the QEMU usbhid-tap socket: capture one 12-byte HID joystick report.
Assert button1 + X(512) => 01 00 00 00 00 20 00 00 00 00 00 00.
Exit 0 on match, 1 on mismatch/short, 2 on connection failure."""
import socket, sys, time

host, port = sys.argv[1], int(sys.argv[2])

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

sock.settimeout(8)
got = b""
try:
    while len(got) < 12:
        chunk = sock.recv(64)
        if not chunk:
            break
        got += chunk
except socket.timeout:
    pass

print("got=%r (%d bytes)" % (got, len(got)))
want = bytes([0x01, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
sys.exit(0 if got[:12] == want else 1)
