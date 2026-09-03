#!/usr/bin/env python3
# Convert bt_tone_test's M2_BT_ACL_TRACE lines to a btsnoop HCI log
# (datalink 1001 = H4/UART) that Apple PacketLogger / Wireshark decode.
#   acl_trace dir=<in|out> h=<handle> t=<micros> hex=<l2cap pdu bytes>
# Usage: acl-trace-to-btsnoop.py in.log out.btsnoop
import sys, re, struct

LINE = re.compile(r"acl_trace dir=(in|out) h=(\w+) t=(\d+) hex=([0-9A-Fa-f ]*)")

def records(text):
    for m in LINE.finditer(text):
        direction, h, t, hexs = m.group(1), int(m.group(2), 0), int(m.group(3)), m.group(4)
        pdu = bytes(int(b, 16) for b in hexs.split())
        # H4 ACL packet: 0x02 | handle(12)+PB(2=0b10 first automatically flushable)+BC(2) | ACL len | L2CAP PDU
        hf = (h & 0x0FFF) | (0x02 << 12)
        acl = bytes([0x02, hf & 0xFF, (hf >> 8) & 0xFF, len(pdu) & 0xFF, (len(pdu) >> 8) & 0xFF]) + pdu
        yield (direction == "in"), t, acl

def write_btsnoop(recs, out):
    out.write(b"btsnoop\x00")                       # identification
    out.write(struct.pack(">II", 1, 1001))          # version 1, datalink 1001 (H4)
    for is_in, t_us, acl in recs:
        # timestamp: btsnoop microseconds since 2000-01-01 00:00 (0x00E03AB44A676000 offset from epoch us)
        ts = 0x00E03AB44A676000 + t_us
        flags = (1 if is_in else 0)                 # bit0: 1=received (controller->host), 0=sent
        out.write(struct.pack(">IIIIq", len(acl), len(acl), flags, 0, ts))
        out.write(acl)

def main():
    text = open(sys.argv[1]).read()
    with open(sys.argv[2], "wb") as f:
        write_btsnoop(records(text), f)
    print("btsnoop written:", sys.argv[2])

if __name__ == "__main__":
    main()
