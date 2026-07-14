#!/usr/bin/env python3
"""Read the QEMU usbhid-tap socket: capture one 6-byte HID mouse report.
Assert Mouse.move(10,5) => 01 00 0A 05 00 00 (report-ID 1, buttons 0, dx 10, dy 5).
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
    while len(got) < 6:
        chunk = sock.recv(64)
        if not chunk:
            break
        got += chunk
except socket.timeout:
    pass

print("got=%r (%d bytes)" % (got, len(got)))
want = bytes([0x01, 0x00, 0x0A, 0x05, 0x00, 0x00])
sys.exit(0 if got[:6] == want else 1)
