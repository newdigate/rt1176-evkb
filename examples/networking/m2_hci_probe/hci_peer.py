#!/usr/bin/env python3
"""Fake HCI controller for the m2_hci_probe [hci] gate.

Speaks H4 over the UNIX socket qemu2 exposes as LPUART2
(-serial unix:PATH,server -- slot 1 = LPUART2; QEMU waits for us before booting the guest).  Every value it returns
is one the firmware cannot invent, so a matching line in the UART capture is
proof the bytes made the round trip.

Phases (argv[1]):
  full        answer everything; inject two inquiry results and their names
  drop-reset  never answer Reset              -> firmware must time out BY NAME
  garbage     3 bytes of 0xFF before the first Reset reply, same burst
                                              -> attempt 1 fails as framing, attempt 2 succeeds
  starve      answer with Num_HCI_Command_Packets=0 -> every later command starves
Exit 0 when the phase's last expected opcode was seen.  Prints PEER-* lines.
"""
import socket, struct, sys, time

MANUFACTURER, HCI_REV, LMP_SUBVER, HCI_VER, LMP_VER = 0x1234, 0xBEEF, 0xCAFE, 0x0B, 0x0B
BD_ADDR = bytes.fromhex("665544332211")          # little-endian on the wire -> prints 11:22:33:44:55:66
ACL_LEN, SCO_LEN, ACL_NUM, SCO_NUM = 1021, 64, 8, 0
DEVICES = [(bytes.fromhex("01EEDDCCBBAA"), 0x240404, b"FAKE-HEADSET-01"),   # prints AA:BB:CC:DD:EE:01
           (bytes.fromhex("02EEDDCCBBAA"), 0x240404, b"FAKE-HEADSET-02")]

OP_RESET, OP_READ_LOCAL_VER, OP_READ_BUFFER_SIZE, OP_READ_BD_ADDR = 0x0C03, 0x1001, 0x1005, 0x1009
OP_INQUIRY, OP_REMOTE_NAME_REQ = 0x0401, 0x0419
LAST_OPCODE = {"full": OP_REMOTE_NAME_REQ, "drop-reset": OP_RESET, "garbage": OP_REMOTE_NAME_REQ, "starve": OP_RESET}

def connect(path):
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(path); return s
        except OSError:
            time.sleep(0.2)
    print("ERROR: could not connect to %s" % path); sys.exit(2)

def event(code, params):               return bytes([0x04, code, len(params)]) + params
def cmd_complete(opcode, ret, ncmd=1): return event(0x0E, bytes([ncmd]) + struct.pack("<H", opcode) + ret)
def cmd_status(opcode, status=0, n=1): return event(0x0F, bytes([status, n]) + struct.pack("<H", opcode))

class Peer:
    def __init__(self, sock, phase):
        self.s, self.phase, self.buf, self.cmds, self.log = sock, phase, b"", [], []
        self.resets, self.pending = 0, []          # pending: (due, bytes)
    def send(self, b, delay=0.0): self.pending.append((time.time() + delay, b))
    def flush(self):
        now = time.time(); keep = []
        for due, b in self.pending:
            if due <= now: self.s.sendall(b)
            else: keep.append((due, b))
        self.pending = keep
    def handle(self, opcode, params):
        self.cmds.append(opcode)
        ncmd = 0 if self.phase == "starve" else 1
        if opcode == OP_RESET:
            self.resets += 1
            if self.phase == "drop-reset": return
            if self.phase == "garbage" and self.resets == 1:
                self.send(b"\xff\xff\xff" + cmd_complete(OP_RESET, b"\x00")); return
            self.send(cmd_complete(OP_RESET, b"\x00", ncmd))
        elif opcode == OP_READ_LOCAL_VER:
            self.send(cmd_complete(opcode, b"\x00" + bytes([HCI_VER]) + struct.pack("<H", HCI_REV)
                                   + bytes([LMP_VER]) + struct.pack("<HH", MANUFACTURER, LMP_SUBVER)))
        elif opcode == OP_READ_BD_ADDR:
            self.send(cmd_complete(opcode, b"\x00" + BD_ADDR))
        elif opcode == OP_READ_BUFFER_SIZE:
            self.send(cmd_complete(opcode, b"\x00" + struct.pack("<HBHH", ACL_LEN, SCO_LEN, ACL_NUM, SCO_NUM)))
        elif opcode == OP_INQUIRY:
            self.send(cmd_status(opcode))
            n = len(DEVICES)
            # Inquiry Result (7.7.2) is FIELD-MAJOR: all BD_ADDRs, all PSRMs, reserved, all CoDs, all clocks.
            body = (bytes([n]) + b"".join(d[0] for d in DEVICES) + bytes([1]) * n + b"\x00\x00" * n
                    + b"".join(struct.pack("<I", d[1])[:3] for d in DEVICES) + struct.pack("<H", 0x1234) * n)
            self.send(event(0x02, body), 0.2)
            self.send(event(0x01, b"\x00"), 0.5)                     # Inquiry Complete
        elif opcode == OP_REMOTE_NAME_REQ:
            self.send(cmd_status(opcode))
            bd = params[:6]
            for d in DEVICES:
                if d[0] == bd:
                    self.send(event(0x07, b"\x00" + bd + d[2].ljust(248, b"\x00")), 0.1); break
            else:
                self.send(event(0x07, b"\x04" + bd + b"\x00" * 248), 0.1)   # 0x04 = Page Timeout
        else:
            self.log.append("PEER-UNKNOWN-OPCODE 0x%04x" % opcode)
            self.send(cmd_complete(opcode, b"\x01"))                    # 0x01 = Unknown HCI Command
    def feed(self, data):
        self.buf += data
        while self.buf:
            if self.buf[0] != 0x01:                                     # only commands come from a host
                self.log.append("PEER-BAD-TYPE 0x%02x" % self.buf[0]); self.buf = self.buf[1:]; continue
            if len(self.buf) < 4: return
            opcode, plen = struct.unpack("<HB", self.buf[1:4])
            if len(self.buf) < 4 + plen: return
            params, self.buf = self.buf[4:4 + plen], self.buf[4 + plen:]
            self.handle(opcode, params)

if __name__ == "__main__":
    phase, path = sys.argv[1], sys.argv[2]
    if phase not in LAST_OPCODE: print("ERROR: unknown phase %s" % phase); sys.exit(2)
    sock = connect(path); sock.settimeout(0.05)
    print("PEER-CONNECTED phase=%s" % phase)
    peer = Peer(sock, phase)
    deadline, last_rx = time.time() + 45, time.time()
    while time.time() < deadline:
        try:
            d = sock.recv(4096)
            if not d: break
            peer.feed(d); last_rx = time.time()
        except socket.timeout:
            pass
        peer.flush()
        if LAST_OPCODE[phase] in peer.cmds and not peer.pending and time.time() - last_rx > 3.0:
            break
    for l in peer.log: print(l)
    print("PEER-DONE phase=%s cmds=%d resets=%d opcodes=%s"
          % (phase, len(peer.cmds), peer.resets, ",".join("%04x" % c for c in peer.cmds)))
    sys.exit(0 if LAST_OPCODE[phase] in peer.cmds else 1)
