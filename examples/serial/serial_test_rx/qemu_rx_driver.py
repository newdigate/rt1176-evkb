#!/usr/bin/env python3
"""Connect to QEMU's LPUART1 socket, inject bytes, assert echo + rx_isr>0."""
import socket, sys, time, re

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 45455
PAYLOAD = b"hello\n"

def connect(timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            s = socket.create_connection((HOST, PORT), timeout=2.0)
            s.settimeout(2.0)
            return s
        except OSError:
            time.sleep(0.2)
    sys.exit("FAIL: could not connect to QEMU serial socket")

def read_until(s, needle, deadline):
    buf = b""
    while time.time() < deadline:
        try:
            b = s.recv(256)
        except socket.timeout:
            continue
        if not b:
            break
        buf += b
        if needle in buf:
            return buf, True
    return buf, False

s = connect()
# The boot banner ("RT1176 RX echo ready") is a one-shot printed in setup()
# before the socket client can connect, so gate on the periodic [status] line
# instead: that proves the sketch is alive and looping.  (If the connect wins
# the race the banner is accepted too.)
buf, ok = read_until(s, b"[status] rx_isr=", time.time() + 15)
if not ok and b"RT1176 RX echo ready" not in buf:
    sys.exit(f"FAIL: no banner or status line seen; got {buf!r}")
s.sendall(PAYLOAD)
buf2, ok = read_until(s, PAYLOAD.strip(), time.time() + 8)
stream = buf + buf2
if PAYLOAD.strip() not in stream:
    sys.exit(f"FAIL: echo not seen; got {stream!r}")
# Drain status lines emitted AFTER injection.  loop() prints [status] once per
# second, so read a few seconds to guarantee at least one post-echo line, then
# take the LAST match (which reflects the state after serialEvent1 ran).  The
# echo bytes ("hello") land in `after` before the next status line, so anything
# scanned from here is strictly post-injection.
after = stream.split(PAYLOAD.strip(), 1)[1]
buf3 = after
end = time.time() + 4
while time.time() < end:
    try:
        b = s.recv(256)
    except socket.timeout:
        continue
    if not b:
        break
    buf3 += b
m = None
for line in buf3.splitlines():
    mm = re.search(rb"\[status\] rx_isr=(\d+) echoed=(\d+)", line)
    if mm:
        m = mm
if not m:
    sys.exit("FAIL: no post-injection status line seen")
rx_isr, ech = int(m.group(1)), int(m.group(2))
if rx_isr <= 0:
    sys.exit(f"FAIL: rx_isr={rx_isr} (RX interrupt never fired)")
if ech < len(PAYLOAD):
    sys.exit(f"FAIL: echoed={ech} < {len(PAYLOAD)}")
print(f"PASS: echo verified, rx_isr={rx_isr}, echoed={ech}")
s.close()
