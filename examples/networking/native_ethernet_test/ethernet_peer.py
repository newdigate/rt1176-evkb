#!/usr/bin/env python3
import socket, sys, time
def recvall(s, n):
    b=b""
    while len(b)<n:
        c=s.recv(n-len(b))
        if not c: raise EOFError
        b+=c
    return b
phase, host = sys.argv[1], sys.argv[2]
if phase == "server":
    hport=5555; time.sleep(6); s=None
    for a in range(5):
        try: s=socket.create_connection((host,hport),timeout=5); break
        except OSError: print("connect retry",a); time.sleep(1)
    if s is None: print("FAIL: no connect"); sys.exit(1)
    s.settimeout(6); msg=b"ETH-TCP-ECHO-PROBE"; s.sendall(msg)
    try: got=recvall(s,len(msg))
    except (EOFError,socket.timeout): print("FAIL: no echo"); sys.exit(1)
    print("TCP got=%r"%got); s.close(); sys.exit(0 if got==msg else 1)
if phase == "udp":
    hport=5556; time.sleep(6)
    s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.settimeout(5); msg=b"ETH-UDP-ECHO-PROBE"
    for a in range(4):
        s.sendto(msg,(host,hport))
        try:
            got,_=s.recvfrom(1024); print("UDP got=%r"%got); sys.exit(0 if got==msg else 1)
        except socket.timeout: print("udp retry",a)
    print("FAIL: no udp echo"); sys.exit(1)
print("skeleton phase",phase); sys.exit(0)
