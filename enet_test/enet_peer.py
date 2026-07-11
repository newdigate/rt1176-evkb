#!/usr/bin/env python3
"""QEMU socket-netdev peer for the ENET gate.
Phase arg selects behavior; prints result lines, exits 0 on success."""
import socket, sys, time, struct

def connect(host, port):
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            return socket.create_connection((host, port), timeout=1)
        except OSError:
            time.sleep(0.2)
    print("ERROR: could not connect to enet socket"); sys.exit(2)

def send_frame(sock, frame):
    sock.sendall(struct.pack(">I", len(frame)) + frame)

def recvall(sock, n):
    buf = b""
    while len(buf) < n:
        c = sock.recv(n - len(buf))
        if not c: raise EOFError
        buf += c
    return buf

def recv_frame(sock, timeout=5):
    sock.settimeout(timeout)
    (n,) = struct.unpack(">I", recvall(sock, 4))
    return recvall(sock, n)

if __name__ == "__main__":
    host, port, phase = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    sock = connect(host, port)
    print("PEER-CONNECTED phase=%s" % phase)
    if phase == "mac":
        time.sleep(1.0)
        rx = bytes.fromhex("020000000001020000000002") + b"\x88\xb5" + b"ENET-RX-PROBE"
        send_frame(sock, rx)
        try:
            f = recv_frame(sock, timeout=6)
        except (EOFError, socket.timeout):
            print("FAIL: no TX frame from guest"); sys.exit(1)
        ok = f[12:14] == b"\x88\xb5" and f[14:27] == b"ENET-TX-PROBE"
        print("TX-FRAME=%r ok=%s" % (f[:32], ok))
        sys.exit(0 if ok else 1)
    # Task 2: just confirm the socket is live, then exit 0.
    sys.exit(0)
