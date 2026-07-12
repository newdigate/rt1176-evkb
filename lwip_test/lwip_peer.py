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
    if phase == "ping":
        # Only the ping phase uses the raw socket-netdev conduit (-nic socket,listen=...);
        # udp/tcp run "-nic user" (SLIRP) with no listener on this port, so the
        # unconditional connect() that used to live here would hang ~10s and
        # exit(2) before ever reaching those phases' own sockets.
        sock = connect(host, port)
        print("PEER-CONNECTED phase=%s" % phase)
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
        # 1) ARP request "who has BOARD_IP tell PEER". This phase runs with no
        # DHCP server, so the netif has NO address (and won't answer ARP) until
        # lwip_test.cpp's 5s static-fallback timer fires. Retry the request
        # until a reply lands: inject, wait ~1.25s (skipping any gratuitous
        # ARP, incl. the extra one netif_set_addr's fallback itself issues),
        # re-inject if nothing valid arrived. Up to 8 attempts / ~10s total.
        arp = (b"\xff"*6 + PEER_MAC + b"\x08\x06"
               + b"\x00\x01\x08\x00\x06\x04\x00\x01" + PEER_MAC + PEER_IP + b"\x00"*6 + BOARD_IP)
        r = None
        for attempt in range(1, 9):
            send_frame(sock, arp)
            try:
                r = recv_arp_reply(1.25)
                break
            except (EOFError, socket.timeout):
                print("RETRY no ARP reply yet (attempt %d/8)" % attempt)
        if r is None:
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
    if phase == "udp":
        host, hport = sys.argv[2], 5556
        time.sleep(6)     # let DHCP lease + the echo server come up
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(5)
        msg = b"LWIP-UDP-ECHO-PROBE"
        for attempt in range(4):
            s.sendto(msg, (host, hport))
            try:
                got, _ = s.recvfrom(1024)
                print("UDP got=%r" % got); sys.exit(0 if got == msg else 1)
            except socket.timeout:
                print("UDP retry %d" % attempt)
        print("FAIL: no UDP echo"); sys.exit(1)
    if phase == "tcp":
        host, hport = sys.argv[2], 5555
        time.sleep(6)     # DHCP lease + server up
        s = None
        for attempt in range(5):
            try:
                s = socket.create_connection((host, hport), timeout=5); break
            except OSError:
                print("TCP connect retry %d" % attempt); time.sleep(1)
        if s is None: print("FAIL: no TCP connect"); sys.exit(1)
        s.settimeout(6)
        msg = b"LWIP-TCP-ECHO-PROBE"
        s.sendall(msg)
        try:
            got = recvall(s, len(msg))
        except (EOFError, socket.timeout):
            print("FAIL: no TCP echo"); sys.exit(1)
        print("TCP got=%r" % got); s.close()
        sys.exit(0 if got == msg else 1)
    print("peer phase=%s (skeleton)" % phase)
    sys.exit(0)
