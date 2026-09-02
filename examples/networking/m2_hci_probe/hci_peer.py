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
  fwdnld      play the NXP V3 UART BOOTLOADER first: send the start indication
              the real card sends, serve chunk requests, verify the bytes the
              host returns, and only then answer HCI
  baud        vendor set-baud (0xFC09) then identity again at the new rate
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
OP_VS_SET_BAUD = 0xFC09                  # NXP vendor: param = uint32 LE running baud

# --- NXP V3 UART firmware download -----------------------------------------
# The real M2-MAYA-W161 sends exactly these five bytes on power-up, three
# times, and then goes silent (bench capture 2026-08-23):
#     AB 01 72 00 47
# = header 0xAB, chipId 0x7201 (the same hw_version GET_HW_SPEC reports over
# SDIO), loader version 0, CRC-8 0x47.  This peer replays that frame verbatim.
V3_START_IND, V3_DATA_REQ, V3_ACK = 0xAB, 0xA7, 0x7A
BOOT_CHIP_ID, BOOT_LOADER_VER = 0x7201, 0x00
# The gate build compiles in a 1 KB SYNTHETIC image (four 256-byte ramps),
# NOT NXP firmware -- see the example's CMakeLists.  We know its bytes, so we
# can check that the host serves the RIGHT ones at the RIGHT offsets.
SYNTH_LEN = 1024
def synth_image():
    return bytes(((n + i) & 0xFF) for n in range(4) for i in range(256))

def crc8(data, poly=0x07, init=0xFF):
    crc = init
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc

def v3_start_ind():
    f = bytes([V3_START_IND, BOOT_CHIP_ID & 0xFF, BOOT_CHIP_ID >> 8, BOOT_LOADER_VER])
    return f + bytes([crc8(f)])

def v3_data_req(length, offset, err=0):
    f = bytes([V3_DATA_REQ]) + struct.pack("<HIH", length, offset, err)
    return f + bytes([crc8(f)])
LAST_OPCODE = {"full": OP_REMOTE_NAME_REQ, "drop-reset": OP_RESET, "garbage": OP_REMOTE_NAME_REQ,
               "starve": OP_RESET, "fwdnld": OP_READ_BD_ADDR, "baud": OP_READ_BUFFER_SIZE}
LAST_OPCODE_COUNT = {"baud": 2}   # phases whose terminal opcode must be seen N times (default 1)

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
        self.baud_seen = []
        # --- V3 bootloader state (fwdnld phase only) ---
        # While `boot` is True every received byte belongs to the download, not
        # to HCI: the host is answering the bootloader, and H4 framing has not
        # started yet.  The plan is a list of (length, offset) chunks; the last
        # one must reach the end of the image or the host is not done.
        self.boot = (phase == "fwdnld")
        self.img = synth_image()
        self.boot_plan = [(256, 0), (256, 256), (256, 512), (256, 768)]
        self.boot_step = 0
        self.boot_acks = 0
        self.boot_bytes = b""
        self.boot_expect = 0          # bytes of image payload still expected
        self.boot_ok = False
        self.boot_err = None
        self.boot_last_ind = 0.0
        self.boot_inds_sent = 0
        self.boot_started = False      # True once the transfer proper has begun
        self.boot_ack_at = 0.0
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
        elif opcode == OP_VS_SET_BAUD:
            if len(params) != 4:
                self.log.append("PEER-SETBAUD-BAD-LEN %d" % len(params))
                self.send(cmd_complete(opcode, b"\x12")); return       # 0x12 = Invalid HCI Command Parameters
            rate = struct.unpack("<I", params)[0]
            self.baud_seen.append(rate)
            self.log.append("PEER-SETBAUD rate=%d" % rate)
            self.send(cmd_complete(opcode, b"\x00"))
        else:
            self.log.append("PEER-UNKNOWN-OPCODE 0x%04x" % opcode)
            self.send(cmd_complete(opcode, b"\x01"))                    # 0x01 = Unknown HCI Command
    def feed_boot(self, data):
        """Consume host bytes during the V3 download.

        The host's traffic is a strict alternation: an ACK frame [7A][crc] for
        each frame we sent, then -- for a data request -- exactly the bytes we
        asked for.  Anything else is a protocol error and is RECORDED rather
        than ignored, because a downloader that sends the wrong bytes at the
        right time is exactly the bug this phase exists to catch."""
        self.boot_bytes += data
        while self.boot_bytes:
            if self.boot_expect:
                take = min(self.boot_expect, len(self.boot_bytes))
                chunk, self.boot_bytes = self.boot_bytes[:take], self.boot_bytes[take:]
                length, offset = self.boot_plan[self.boot_step - 1]
                want = self.img[offset + (length - self.boot_expect):][:take]
                if chunk != want:
                    self.boot_err = ("wrong image bytes at offset %d (+%d)"
                                     % (offset, length - self.boot_expect))
                self.boot_expect -= take
                if self.boot_expect == 0:
                    if self.boot_step >= len(self.boot_plan):
                        self.boot_ok = True
                        self.boot = False          # HCI starts now
                        self.log.append("PEER-BOOT-COMPLETE chunks=%d bytes=%d"
                                        % (len(self.boot_plan), SYNTH_LEN))
                    else:
                        self.send(v3_data_req(*self.boot_plan[self.boot_step]))
                        self.boot_step += 1
                continue
            if len(self.boot_bytes) < 2:
                return
            if self.boot_bytes[0] != V3_ACK:
                self.boot_err = "expected ACK 0x7A, got 0x%02X" % self.boot_bytes[0]
                self.boot_bytes = self.boot_bytes[1:]
                continue
            if self.boot_bytes[1] != crc8(bytes([V3_ACK])):
                self.boot_err = "ACK crc wrong: 0x%02X" % self.boot_bytes[1]
            self.boot_bytes = self.boot_bytes[2:]
            self.boot_acks += 1
            if not self.boot_started:
                # Still in the greeting phase.  We may have sent several start
                # indications before the firmware was listening (see the retry
                # note in main), so SEVERAL acks can be in flight.  Do not start
                # the transfer on the first one -- wait for the line to go quiet
                # in main(), then start.  Counting them is enough here.
                self.boot_ack_at = time.time()
                continue
            self.boot_expect = self.boot_plan[self.boot_step - 1][0]

    def feed(self, data):
        if self.boot:
            self.feed_boot(data)
            return
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
    if peer.boot:
        # ★ REPEAT the start indication until it is answered, which is what the
        # real card does (it sent exactly three on the bench before giving up).
        # It is also required here for a reason worth recording: QEMU holds the
        # guest until we connect, so anything sent now lands while the firmware
        # is still in its board preamble -- and Serial2.begin() /
        # addMemoryForRead() reset the ring, discarding whatever arrived first.
        # Sending once produced a MID-FRAME byte and a bad_header, which is a
        # faithful reproduction of a race a real host can also lose.
        peer.send(v3_start_ind())
        peer.boot_last_ind = time.time()
    deadline, last_rx = time.time() + 45, time.time()
    while time.time() < deadline:
        try:
            d = sock.recv(4096)
            if not d: break
            peer.feed(d); last_rx = time.time()
        except socket.timeout:
            pass
        peer.flush()
        if peer.boot and not peer.boot_started:
            if peer.boot_acks == 0 and time.time() - peer.boot_last_ind > 0.3:
                if peer.boot_inds_sent < 40:      # ~12 s of retries, then give up
                    peer.send(v3_start_ind()); peer.boot_last_ind = time.time()
                    peer.boot_inds_sent += 1
                    peer.boot_bytes = b""         # drop anything half-read before we were heard
            elif peer.boot_acks and time.time() - peer.boot_ack_at > 0.4:
                # We have been heard, and the line has been quiet for 400 ms --
                # every stale ack from the retries is now drained.  Start the
                # transfer from a known-clean state, so what follows is exactly
                # ack-then-image and any deviation is the driver's.
                peer.boot_bytes = b""
                peer.boot_started = True
                peer.boot_step = 1
                peer.send(v3_data_req(*peer.boot_plan[0]))
        if peer.cmds.count(LAST_OPCODE[phase]) >= LAST_OPCODE_COUNT.get(phase, 1) and not peer.pending and time.time() - last_rx > 3.0:
            break
    for l in peer.log: print(l)
    if phase == "fwdnld":
        print("PEER-BOOT ok=%d acks=%d chunks=%d err=%s"
              % (1 if peer.boot_ok else 0, peer.boot_acks, peer.boot_step,
                 peer.boot_err if peer.boot_err else "none"))
    print("PEER-DONE phase=%s cmds=%d resets=%d opcodes=%s baud=%s"
          % (phase, len(peer.cmds), peer.resets, ",".join("%04x" % c for c in peer.cmds),
             ",".join(str(b) for b in peer.baud_seen) or "none"))
    ok = peer.cmds.count(LAST_OPCODE[phase]) >= LAST_OPCODE_COUNT.get(phase, 1)
    if phase == "fwdnld":
        ok = ok and peer.boot_ok and peer.boot_err is None
    sys.exit(0 if ok else 1)
