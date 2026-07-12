#!/usr/bin/env python3
import socket, sys, struct, time
def recvall(s,n):
    b=b""
    while len(b)<n:
        c=s.recv(n-len(b))
        if not c: raise EOFError
        b+=c
    return b
if __name__=="__main__":
    phase=sys.argv[1]
    print("peer phase=%s (skeleton)" % phase)
    sys.exit(0)
