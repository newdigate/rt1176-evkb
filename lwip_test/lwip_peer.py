#!/usr/bin/env python3
"""QEMU socket-netdev peer for the lwIP gate.
Phase arg selects behavior; prints result lines, exits 0 on success."""
import socket, sys, struct, time

def connect(host, port):
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            return socket.create_connection((host, port), timeout=1)
        except OSError:
            time.sleep(0.2)
    print("ERROR: could not connect to lwip socket"); sys.exit(2)

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
    phase, host, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    sock = connect(host, port)
    print("PEER-CONNECTED phase=%s" % phase)
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
        # lwip_test's loop() only polls the netif + services lwIP timeouts, but
        # lwIP itself (unlike enet_test's raw driver) fires one gratuitous ARP
        # announcement the instant netif_set_up() runs (netif_issue_reports() ->
        # etharp_gratuitous(), lwip/src/core/netif.c ~line 930) -- op=1, sender
        # IP == target IP == the board's own address. It shares EtherType
        # 0x0806 with the real reply to our request, so keep the match-by-
        # EtherType skip (proven in enet_peer.py) as defense against unrelated
        # traffic in general.
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
        # ... and layer an op+sender check on top for ARP specifically, since
        # the gratuitous announcement above is EtherType-identical to the
        # reply we're actually waiting for and recv_match can't tell them apart.
        def recv_arp_reply(timeout=6):
            deadline = time.time() + timeout
            while True:
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise socket.timeout()
                r = recv_match(b"\x08\x06", remaining)
                if r[20:22] == b"\x00\x02" and r[22:28] == BOARD_MAC and r[28:32] == BOARD_IP:
                    return r
                print("SKIP non-reply ARP op=%r %r" % (r[20:22], r[:42]))
        # 1) ARP request "who has BOARD_IP tell PEER".
        arp = (b"\xff"*6 + PEER_MAC + b"\x08\x06"
               + b"\x00\x01\x08\x00\x06\x04\x00\x01" + PEER_MAC + PEER_IP + b"\x00"*6 + BOARD_IP)
        send_frame(sock, arp)
        try:
            r = recv_arp_reply(6)
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
    # udp/tcp phases: left for later tasks.
    print("peer phase=%s (skeleton)" % phase)
    sys.exit(0)
