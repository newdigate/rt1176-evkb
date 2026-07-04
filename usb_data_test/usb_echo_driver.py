#!/usr/bin/env python3
"""Drive the QEMU usbcdc socket: send a token, read the guest's echo, compare.
Exit 0 on a matching round-trip, 1 on mismatch, 2 on connection failure."""
import socket, sys, time

host, port, token = sys.argv[1], int(sys.argv[2]), sys.argv[3].encode()
payload = token + b"\n"

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
    print("ERROR: could not connect to usbcdc socket")
    sys.exit(2)

# Let the guest reach CDC_RUN (DTR asserted) and prime its RX transfers.
time.sleep(1.0)
sock.settimeout(5)
sock.sendall(payload)

got = b""
try:
    while len(got) < len(payload):
        chunk = sock.recv(64)
        if not chunk:
            break
        got += chunk
except socket.timeout:
    pass

print("sent=%r got=%r" % (payload, got))
sys.exit(0 if got.strip() == token else 1)
