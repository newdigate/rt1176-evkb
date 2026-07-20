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
    if phase == "ping":
        time.sleep(1.5)
        BOARD_MAC = bytes.fromhex("020000000001"); PEER_MAC = bytes.fromhex("020000000002")
        BOARD_IP = bytes([192,168,1,50]); PEER_IP = bytes([192,168,1,1])
        def cksum(b):
            s = 0
            for i in range(0, len(b) - 1, 2): s += (b[i] << 8) | b[i+1]
            if len(b) & 1: s += b[-1] << 8
            while s >> 16: s = (s & 0xffff) + (s >> 16)
            return (~s) & 0xffff
        # loop() unconditionally fires the Gate-1 TX probe ~500ms after boot
        # (needed by the mac phase), independent of what this phase is doing.
        # Since we sleep 1.5s before sending anything, that probe is already
        # queued ahead of our replies -- skip non-matching EtherTypes instead
        # of taking whatever frame is next.
        def recv_match(want_et, timeout=6):
            deadline = time.time() + timeout
            while True:
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise socket.timeout()
                r = recv_frame(sock, remaining)
                if r[12:14] == want_et:
                    return r
                print("SKIP stray frame et=%r %r" % (r[12:14], r[:20]))
        # 1) ARP request "who has BOARD_IP tell PEER".
        arp = (b"\xff"*6 + PEER_MAC + b"\x08\x06"
               + b"\x00\x01\x08\x00\x06\x04\x00\x01" + PEER_MAC + PEER_IP + b"\x00"*6 + BOARD_IP)
        send_frame(sock, arp)
        try:
            r = recv_match(b"\x08\x06", 6)
        except (EOFError, socket.timeout):
            print("FAIL: no ARP reply"); sys.exit(1)
        arp_ok = (r[12:14] == b"\x08\x06" and r[20:22] == b"\x00\x02"
                  and r[22:28] == BOARD_MAC and r[28:32] == BOARD_IP)
        print("ARP-REPLY ok=%s %r" % (arp_ok, r[:42]))
        # 2) ICMP echo request.
        ident, seq, data = 0x1234, 1, b"abcdefghij"
        icmp = struct.pack(">BBHHH", 8, 0, 0, ident, seq) + data
        icmp = icmp[:2] + struct.pack(">H", cksum(icmp)) + icmp[4:]
        ihl_tl = struct.pack(">BBHHHBBH", 0x45, 0, 20 + len(icmp), 0, 0, 64, 1, 0) + PEER_IP + BOARD_IP
        ihl_tl = ihl_tl[:10] + struct.pack(">H", cksum(ihl_tl)) + ihl_tl[12:]
        eth = BOARD_MAC + PEER_MAC + b"\x08\x00" + ihl_tl + icmp
        send_frame(sock, eth)
        try:
            r = recv_match(b"\x08\x00", 6)
        except (EOFError, socket.timeout):
            print("FAIL: no ICMP reply"); sys.exit(1)
        off = 14 + (r[14] & 0x0F) * 4
        icmp_ok = (r[12:14] == b"\x08\x00" and r[off] == 0
                   and r[off+8:off+8+len(data)] == data and cksum(r[off:off+8+len(data)]) == 0)
        print("ICMP-REPLY ok=%s type=%d" % (icmp_ok, r[off]))
        sys.exit(0 if (arp_ok and icmp_ok) else 1)
    # Task 2: just confirm the socket is live, then exit 0.
    sys.exit(0)
