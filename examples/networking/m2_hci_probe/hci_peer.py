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
  avdtp       accept Create_Connection + the full SSP dance to Encryption_Change,
              then act as an L2CAP acceptor, an SDP responder (AudioSink PDL,
              AVDTP 1.3) and an AVDTP acceptor (DISCOVER/GET_CAPABILITIES/
              SET_CONFIGURATION/OPEN/START) over ACL -- ends on an accepted START
  media       runs the SAME avdtp acceptor to an accepted START, then RECEIVES
              on the second (media transport) L2CAP channel opened for AVDTP's
              PSM 0x0019 and validates each frame as an RTP v2 / A2DP SBC media
              packet (V/PT, sequence continuity, per-frame sync byte + length) --
              ends once at least one packet has been received with no framing
              fault or sequence gap
Exit 0 when the phase's last expected opcode was seen (avdtp: when the peer
recorded an accepted START; media: when the media validation above holds).
Prints PEER-* lines.
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
               "starve": OP_RESET, "fwdnld": OP_READ_BD_ADDR, "baud": OP_READ_BUFFER_SIZE,
               "avdtp": 0x0413, "media": 0x0413}   # Set_Connection_Encryption -- last COMMAND before
                                  # signalling; the real end of both avdtp and media is checked
                                  # separately (peer.avdtp["started"] / peer.media, below)
LAST_OPCODE_COUNT = {"baud": 2}   # phases whose terminal opcode must be seen N times (default 1)

def phase_done(phase, peer):
    # avdtp's real end is signalling over ACL (an accepted START), not a
    # command opcode -- 0x0413 (Set_Connection_Encryption) is only the last
    # HCI COMMAND before L2CAP/SDP/AVDTP take over.
    if phase == "avdtp": return peer.avdtp["started"] and not peer.avdtp["error"]
    # media's real end is receiving at least one well-formed RTP/SBC media
    # packet with no sequence gap and no framing fault -- avdtp START is a
    # precondition (handle_media never runs before it), not the terminal check.
    if phase == "media":
        m = peer.media
        return m["pkts"] > 0 and m["seqgaps"] == 0 and m["badsbc"] == 0 and m["badrtp"] == 0
    return peer.cmds.count(LAST_OPCODE[phase]) >= LAST_OPCODE_COUNT.get(phase, 1)

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
def acl(handle, cid, payload):                    # controller -> host ACL, PB=10 (first, auto-flushable)
    hf = (handle & 0x0FFF) | (0x02 << 12)
    return bytes([0x02]) + struct.pack("<HH", hf, len(payload) + 4) + struct.pack("<HH", len(payload), cid) + payload
def ncp(handle, n=1):                             # Number_Of_Completed_Packets: the credit the host's L2cap pacing needs
    return event(0x13, bytes([1]) + struct.pack("<HH", handle, n))
SBC_CIE_EXPECT = bytes.fromhex("21150235")        # 44.1k joint / 16 blk 8 sub loudness / bitpool 2..53 -- the calibration config

class Peer:
    def __init__(self, sock, phase):
        self.s, self.phase, self.buf, self.cmds, self.log = sock, phase, b"", [], []
        self.resets, self.pending = 0, []          # pending: (due, bytes)
        self.baud_seen = []
        self.peer_bd = None
        # --- avdtp phase: L2CAP acceptor + SDP responder + AVDTP acceptor state ---
        # key: the CID the far end (the firmware) assigned ITSELF and sent us as
        # SCID in its Connection Request (host-owned, not ours) -> value: (the
        # CID we assigned for our side of the channel, psm)
        self.chans = {}
        self.next_cid = 0x0340
        self.avdtp = {"config": None, "opened": False, "started": False, "order": [], "error": False,
                      "sig_cid": None,    # our (peer-assigned) CID for the FIRST psm=0x0019 channel --
                                          # AVDTP signalling; any OTHER psm=0x0019 channel is media
                      # --- the Shokz-shaped behaviours (Mac->Shokz PacketLogger reference, 2026-09-03) ---
                      "discover_pending": None,   # (handle, cid, tl) of a DISCOVER we are HOLDING until the reverse SDP completes
                      "delay_cfg": False,          # the host configured Delay Reporting (category 0x08) in SET_CONFIGURATION
                      "delay_sent": False, "delay_acked": False,   # our DelayReport COMMAND after OPEN, and its ACCEPT
                      "open_pending": None}        # the OPEN accept we hold until our DelayReport is accepted
        # The Shokz SDP-queries the SOURCE (AudioSource 0x110A, attribute 0x0009 = its A2DP profile
        # version) on a channel IT opens, and answers DISCOVER only after that query completes.
        # This models it: on the host's DISCOVER we open PSM 0x0001 at the host, configure it (MTU 48,
        # like the Shokz), send the Shokz's exact query, require the Mac's exact reply, disconnect,
        # and only THEN answer the DISCOVER.  A source with no SDP server hangs at DISCOVERING here
        # exactly as it did on the bench.
        self.rev = {"state": "idle", "my_cid": 0x0E85, "their_cid": None, "cfg_req_seen": False, "cfg_rsp_seen": False,
                    "query_sent": False, "answer": None, "done": False, "handle": None}
        # --- media phase: RTP/SBC validation on the media transport channel ---
        self.media = {"pkts": 0, "frames": 0, "lastseq": None, "seqgaps": 0, "badrtp": 0, "badsbc": 0}
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
        # --- avdtp phase: link bring-up through the full SSP dance -------------
        elif opcode == 0x0C01 or opcode == 0x0C56 or opcode == 0x0C1A:      # Set_Event_Mask, Write_Simple_Pairing_Mode, Write_Scan_Enable
            self.send(cmd_complete(opcode, b"\x00"))
        elif opcode == 0x0405:                                              # Create_Connection -> Command Status, Connection Complete
            self.peer_bd = params[:6]
            self.log.append("PEER-CREATE-CONN role_switch=%d" % params[12])
            self.send(cmd_status(opcode)); self.send(event(0x03, b"\x00" + struct.pack("<H", 0x0001) + params[:6] + b"\x01\x00"), 0.1)
        elif opcode == 0x0C18:                                              # Write_Page_Timeout -> Command Complete
            self.log.append("PEER-PAGE-TIMEOUT slots=0x%04x" % struct.unpack("<H", params[:2])[0])
            self.send(cmd_complete(opcode, b"\x00"))
        elif opcode == 0x0408:                                              # Create_Connection_Cancel -> CC(status, bd) + Connection Complete 0x02
            self.send(cmd_complete(opcode, b"\x00" + params[:6]))
            self.send(event(0x03, b"\x02" + struct.pack("<H", 0x0000) + params[:6] + b"\x01\x00"), 0.05)
        elif opcode == 0x0406:                                              # Disconnect -> Command Status, Disconnection Complete
            self.send(cmd_status(opcode)); self.send(event(0x05, b"\x00" + params[:2] + params[2:3]), 0.05)
        elif opcode == 0x0411:                                              # Authentication_Requested: SSP Just Works, all the way to Auth Complete
            self.send(cmd_status(opcode)); bd = self.peer_bd
            self.send(event(0x17, bd), 0.05)                                # Link_Key_Request
        elif opcode == 0x040C:                                              # Link_Key_Request_Negative_Reply -> IO cap dance
            self.send(cmd_complete(opcode, b"\x00" + params[:6])); self.send(event(0x31, params[:6]), 0.05)   # IO_Capability_Request
        elif opcode == 0x042B:                                              # IO_Capability_Request_Reply -> peer caps, user confirm
            self.send(cmd_complete(opcode, b"\x00" + params[:6]))
            self.send(event(0x32, params[:6] + b"\x03\x00\x04"), 0.05)      # IO_Capability_Response: NoInputNoOutput, no OOB, general bonding
            self.send(event(0x33, params[:6] + struct.pack("<I", 123456)), 0.1)   # User_Confirmation_Request
        elif opcode == 0x042C:                                              # User_Confirmation_Request_Reply -> pairing complete, link key, auth complete
            self.send(cmd_complete(opcode, b"\x00" + params[:6]))
            self.send(event(0x36, b"\x00" + params[:6]), 0.05)              # Simple_Pairing_Complete
            self.send(event(0x18, params[:6] + bytes(range(16)) + b"\x04"), 0.1)   # Link_Key_Notification (unauthenticated combination)
            self.send(event(0x06, b"\x00" + struct.pack("<H", 0x0001)), 0.15)      # Authentication_Complete
        elif opcode == 0x0413:                                              # Set_Connection_Encryption -> Encryption_Change on
            self.send(cmd_status(opcode)); self.send(event(0x08, b"\x00" + struct.pack("<H", 0x0001) + b"\x01"), 0.1)
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
            if self.buf[0] == 0x02:                                     # ACL from the host (avdtp phase only)
                if len(self.buf) < 5: return
                hf, alen = struct.unpack("<HH", self.buf[1:5])
                if len(self.buf) < 5 + alen: return
                data, self.buf = self.buf[5:5 + alen], self.buf[5 + alen:]
                self.send(ncp(hf & 0x0FFF))                                   # every ACL packet frees a buffer
                self.handle_acl(hf & 0x0FFF, data); continue
            if self.buf[0] != 0x01:                                     # only commands/ACL come from a host
                self.log.append("PEER-BAD-TYPE 0x%02x" % self.buf[0]); self.buf = self.buf[1:]; continue
            if len(self.buf) < 4: return
            opcode, plen = struct.unpack("<HB", self.buf[1:4])
            if len(self.buf) < 4 + plen: return
            params, self.buf = self.buf[4:4 + plen], self.buf[4 + plen:]
            self.handle(opcode, params)

    # --- avdtp phase: L2CAP acceptor + SDP responder + AVDTP acceptor ----------
    def sig(self, handle, cmd): self.send(acl(handle, 0x0001, cmd), 0.02)
    def handle_acl(self, handle, d):
        # Any malformed frame from a buggy Task-10 driver must fail the phase
        # CLEANLY -- a PEER-EXCEPTION line, not a raw Python traceback that
        # reads as "the fake peer is broken" instead of "the firmware sent a
        # bad frame".  The happy-path parses below stay unguarded on purpose:
        # this try/except is the safety net, not a substitute for them.
        try:
            if len(d) < 4: return
            l2len, cid = struct.unpack("<HH", d[:4]); pl = d[4:4 + l2len]
            if cid == 0x0001:                                                    # signalling
                code, ident = pl[0], pl[1]; body = pl[4:]
                if code == 0x02:                                                 # Connection Request: psm, scid
                    psm, scid = struct.unpack("<HH", body[:4]); ours = self.next_cid; self.next_cid += 0x40
                    self.chans[scid] = (ours, psm)
                    if psm == 0x0019 and self.avdtp["sig_cid"] is None:
                        self.avdtp["sig_cid"] = ours    # the FIRST 0x0019 channel is signalling; a
                                                        # later one (opened in AVDTP's OPENING state,
                                                        # Avdtp.cpp's second m_l2->connect(PSM, ...))
                                                        # is the media transport channel
                    self.sig(handle, bytes([0x03, ident, 8, 0]) + struct.pack("<HHHH", ours, scid, 0x0001, 0x0000))   # pending first, like the Shokz
                    self.sig(handle, bytes([0x03, ident, 8, 0]) + struct.pack("<HHHH", ours, scid, 0x0000, 0x0000))
                    self.sig(handle, bytes([0x04, ident + 1, 8, 0]) + struct.pack("<HH", scid, 0) + bytes([0x01, 0x02, 0xA0, 0x02]))  # our Config Request: MTU 672
                elif code == 0x03:                                               # Connection Response to OUR reverse Connection Request
                    dcid, scid, result, status = struct.unpack("<HHHH", body[:8])
                    if scid != self.rev["my_cid"]: self.log.append("PEER-L2CAP-CONNRSP-UNKNOWN-SCID 0x%04x" % scid); return
                    if result == 0x0001: return                                  # pending: wait for the final one
                    if result != 0: self.log.append("PEER-REV-CONN-REFUSED result=0x%04x" % result); self.rev["state"] = "failed"; return
                    self.rev["their_cid"] = dcid; self.rev["state"] = "config"
                    self.chans[dcid] = (self.rev["my_cid"], 0x0001)              # keyed by the HOST's cid, like every other channel
                    self.sig(handle, bytes([0x04, 0x0A, 8, 0]) + struct.pack("<HH", dcid, 0) + bytes([0x01, 0x02, 0x30, 0x00]))   # our Config Request: MTU 48 (the Shokz's)
                elif code == 0x04:                                               # Config Request for one of OUR endpoints: accept, echo options
                    dcid = struct.unpack("<H", body[:2])[0]
                    peer = [p for p, (o, _) in self.chans.items() if o == dcid]
                    if not peer: self.log.append("PEER-L2CAP-CFG-UNKNOWN-DCID 0x%04x" % dcid); return
                    opts = body[4:]
                    # ★ Tripwire: the Mac's Config Request to the Shokz carries an MTU option (01 02 EC 03); ours was
                    # option-less until 2026-09-04 and the Shokz never answered DISCOVER after that exchange.
                    has_mtu, i = False, 0
                    while i + 1 < len(opts):
                        t, ln = opts[i] & 0x7F, opts[i + 1]
                        if t == 0x01 and ln == 2: has_mtu = True
                        i += 2 + ln
                    if not has_mtu: self.log.append("PEER-L2CAP-CFGREQ-NO-MTU dcid=0x%04x" % dcid)
                    self.sig(handle, bytes([0x05, ident, 6 + len(opts), 0]) + struct.pack("<HHH", peer[0], 0, 0) + opts)   # SCID = the host's CID
                    if dcid == self.rev["my_cid"]: self.rev["cfg_req_seen"] = True; self.rev_maybe_query(handle)
                elif code == 0x05:                                               # Config Response to ours: check the receiver-side SCID rule
                    scid, flags, result = struct.unpack("<HHH", body[:6])
                    if scid not in [o for (o, _) in self.chans.values()]: self.log.append("PEER-L2CAP-CFGRSP-BAD-SCID 0x%04x" % scid)
                    if scid == self.rev["my_cid"] and result == 0: self.rev["cfg_rsp_seen"] = True; self.rev_maybe_query(handle)
                elif code == 0x06:                                               # Disconnection Request from the host: acknowledge
                    self.sig(handle, bytes([0x07, ident, 4, 0]) + body[:4])
                elif code == 0x07: pass                                          # Disconnection Response to ours (the reverse SDP channel)
                elif code == 0x0A:                                               # Information Request: extended features none / fixed channels
                    itype = struct.unpack("<H", body[:2])[0]
                    data = b"\x00\x00\x00\x00" if itype == 2 else (b"\x02" + b"\x00" * 7 if itype == 3 else b"")
                    self.sig(handle, bytes([0x0B, ident, 4 + len(data), 0]) + struct.pack("<HH", itype, 0 if data else 1) + data)
                elif code == 0x08: self.sig(handle, bytes([0x09, ident, 0, 0]))  # Echo
                return
            # data on one of our channels
            for peer_cid, (ours, psm) in self.chans.items():
                if ours == cid:
                    if psm == 0x0001: self.handle_sdp(handle, peer_cid, pl)
                    elif psm == 0x0019 and ours == self.avdtp["sig_cid"]: self.handle_avdtp(handle, peer_cid, pl)
                    elif psm == 0x0019: self.handle_media(pl)              # the OTHER 0x0019 channel: media transport
                    return
            self.log.append("PEER-ACL-UNKNOWN-CID 0x%04x" % cid)
        except Exception as e:
            self.log.append("PEER-EXCEPTION handle_acl: %r" % e)
            self.avdtp["error"] = True
    def handle_sdp(self, handle, peer_cid, pl):
        try:
            if pl[0] == 0x06:                                                    # the host's query of OUR AudioSink record
                txn = pl[1:3]
                rsp = bytes.fromhex("07") + txn + bytes.fromhex("001E001B36001836001509000435103506190100090019350619001909010300")  # the PDL seen on the wire: AVDTP 1.3
                self.send(acl(handle, peer_cid, rsp), 0.02); self.log.append("PEER-SDP-ANSWERED")
            elif peer_cid == self.rev["their_cid"] and self.rev["query_sent"] and self.rev["answer"] is None:
                # The host's answer to OUR query of ITS AudioSource record.  The Mac answers the Shokz's exact
                # query with exactly these 25 bytes (one record, attribute 0x0009 = { { A2DP 0x110D, v1.3 } }).
                self.rev["answer"] = pl
                expect = bytes.fromhex("07000100140011350f350d0900093508350619110d09010300")
                if pl == expect: self.log.append("PEER-SDP-SOURCE-RECORD ok")
                else:            self.log.append("PEER-SDP-SOURCE-RECORD-BAD hex=%s" % pl.hex())
                # like the Shokz: tear the channel down, THEN answer the DISCOVER we have been holding
                self.sig(handle, bytes([0x06, 0x0B, 4, 0]) + struct.pack("<HH", peer_cid, self.rev["my_cid"]))
                self.rev["done"] = True; self.rev["state"] = "done"
                self.answer_discover()
        except Exception as e:
            self.log.append("PEER-EXCEPTION handle_sdp: %r" % e)
            self.avdtp["error"] = True
    # --- the reverse SDP query (the Shokz's behaviour) ---
    def rev_start(self, handle):
        self.rev["state"] = "connecting"; self.rev["handle"] = handle
        self.sig(handle, bytes([0x02, 0x0A, 4, 0]) + struct.pack("<HH", 0x0001, self.rev["my_cid"]))   # Connection Request: SDP, our SCID
    def rev_maybe_query(self, handle):
        if self.rev["query_sent"] or not (self.rev["cfg_req_seen"] and self.rev["cfg_rsp_seen"]): return
        self.rev["query_sent"] = True
        # frame 749 of the reference, verbatim: ServiceSearchAttributeRequest, txn 1, {AudioSource 0x110A}, max 32, {0x0009}
        self.send(acl(handle, self.rev["their_cid"], bytes.fromhex("060001000d350319110a0020350309000900")), 0.02)
    def answer_discover(self):
        if not self.avdtp["discover_pending"]: return
        h, cid, tl = self.avdtp["discover_pending"]; self.avdtp["discover_pending"] = None
        # SEID 2 (audio SNK -- our MPEG-1,2 SEP) FIRST, then SEID 1 (audio SNK -- SBC): the Shokz's list order.
        self.send(acl(h, cid, bytes([tl | 0x02, 0x01, 2 << 2, 0x08, 1 << 2, 0x08])), 0.02)
    def handle_avdtp(self, handle, peer_cid, pl):
        try:
            hdr, sig = pl[0], pl[1]; tl = hdr & 0xF0; acc = bytes([tl | 0x02])
            if hdr & 0x03:                                                                                                          # a RESPONSE to one of OUR commands
                if sig == 0x0D and (hdr & 0x03) == 0x02:
                    self.avdtp["delay_acked"] = True; self.log.append("PEER-AVDTP-DELAYREPORT-ACCEPTED")
                    if self.avdtp["open_pending"]:                                                                                  # now the OPEN accept the Shokz held back
                        h, c, a = self.avdtp["open_pending"]; self.avdtp["open_pending"] = None; self.send(acl(h, c, a + b"\x06"), 0.02)
                elif sig == 0x0D: self.log.append("PEER-AVDTP-DELAYREPORT-REJECTED mt=%d" % (hdr & 0x03))
                else:             self.log.append("PEER-AVDTP-UNEXPECTED-RSP sig=%d mt=%d" % (sig, hdr & 0x03))
                return
            self.avdtp["order"].append(sig)
            if sig == 0x01:                                                                                                         # DISCOVER: held until our reverse SDP query completes
                self.avdtp["discover_pending"] = (handle, peer_cid, tl)
                if self.rev["done"]:             self.answer_discover()
                elif self.rev["state"] == "idle": self.rev_start(handle)
            elif sig in (0x02, 0x0C):                                                                                               # GET_CAPABILITIES / GET_ALL_CAPABILITIES for a SEID
                seid = pl[2] >> 2
                extra = b"\x04\x02\x02\x00\x08\x00" if sig == 0x0C else b""                                                         # content protection SCMS-T + DELAY REPORTING (0x0C only)
                if seid == 2:   caps = b"\x01\x00" + b"\x07\x06\x00\x01\x3F\x3F\xFF\xFE" + extra                                  # MPEG-1,2 audio -- the Shokz's SEID 2, NOT SBC
                elif seid == 1: caps = b"\x01\x00" + b"\x07\x06\x00\x00\xFF\xFF\x02\x35" + extra                                  # SBC: all, bitpool 2..53 -- the Shokz's SEID 1
                else: self.send(acl(handle, peer_cid, bytes([tl | 0x03, sig, 0x12])), 0.02); return                                # BAD_ACP_SEID
                self.send(acl(handle, peer_cid, acc + bytes([sig]) + caps), 0.02)
            elif sig == 0x03:                                                                                                       # SET_CONFIGURATION: record the CIE, the SEID and the categories
                acp = pl[2] >> 2
                self.avdtp["config"] = pl[10:14] if len(pl) >= 14 else b""
                cats, i = [], 4
                while i + 1 < len(pl): cats.append(pl[i]); i += 2 + pl[i + 1]
                self.avdtp["delay_cfg"] = 0x08 in cats
                self.log.append("PEER-SET-CONFIG cie=%s acp_seid=%d delay_reporting=%d" % (self.avdtp["config"].hex(), acp, 1 if self.avdtp["delay_cfg"] else 0))
                if acp != 1:                                                                                                        # only SEID 1 is SBC: configuring SEID 2 with an SBC CIE is wrong
                    self.log.append("PEER-AVDTP-SETCONFIG-WRONG-SEID %d" % acp)
                    self.send(acl(handle, peer_cid, bytes([tl | 0x03, 0x03, 0x00, 0x12])), 0.02); return
                self.send(acl(handle, peer_cid, acc + b"\x03"), 0.02)
            elif sig == 0x06:                                                                                                       # OPEN
                self.avdtp["opened"] = True
                if self.avdtp["delay_cfg"]:
                    # like the Shokz (frames 816/818/827): send OUR DelayReport COMMAND (tl 1, 200.0 ms) first and hold
                    # the OPEN accept until the host ACCEPTS it -- a host that ignores DelayReport hangs at OPENING.
                    self.avdtp["open_pending"] = (handle, peer_cid, acc); self.avdtp["delay_sent"] = True
                    self.send(acl(handle, peer_cid, bytes([0x10, 0x0D, 2 << 2, 0x07, 0xD0])), 0.02)
                else:
                    self.send(acl(handle, peer_cid, acc + b"\x06"), 0.02)
            elif sig == 0x07:                                                                                                       # START: only legal after OPEN
                if not self.avdtp["opened"]: self.log.append("PEER-AVDTP-START-BEFORE-OPEN"); self.send(acl(handle, peer_cid, bytes([tl | 0x03, 0x07, 1 << 2, 0x31])), 0.02); return  # 0x31 = bad state
                self.avdtp["started"] = True; self.log.append("PEER-AVDTP-STARTED"); self.send(acl(handle, peer_cid, acc + b"\x07"), 0.02)
            else: self.send(acl(handle, peer_cid, bytes([tl | 0x03, sig, 0x19])), 0.02)                                             # unsupported command
        except Exception as e:
            self.log.append("PEER-EXCEPTION handle_avdtp: %r" % e)
            self.avdtp["error"] = True
    def handle_media(self, pl):
        # RTP v2 (RFC 3550) + A2DP v1.3 sec 4.3.4 SBC media payload, validated
        # against values this firmware cannot invent: V=2/PT=96, a strictly
        # incrementing sequence number, and -- for each of the frameCount SBC
        # frames the payload header names -- the frame's own sync byte and the
        # fixed length the negotiated bitpool-53 config produces.
        try:
            m = self.media
            if len(pl) < 13 or pl[0] != 0x80 or pl[1] != 96:
                m["badrtp"] += 1
                return
            seq = (pl[2] << 8) | pl[3]
            if m["lastseq"] is not None and seq != (m["lastseq"] + 1) & 0xFFFF:
                m["seqgaps"] += 1
            m["lastseq"] = seq
            frame_count = pl[12] & 0x0F
            off = 13
            for _ in range(frame_count):
                if off + 119 > len(pl) or pl[off] != 0x9C:
                    m["badsbc"] += 1
                off += 119
            m["pkts"] += 1
            m["frames"] += frame_count
            if m["pkts"] % 20 == 0:
                self.log.append("PEER-MEDIA pkts=%d frames=%d seqgaps=%d badsbc=%d badrtp=%d"
                                % (m["pkts"], m["frames"], m["seqgaps"], m["badsbc"], m["badrtp"]))
        except Exception as e:
            self.log.append("PEER-EXCEPTION handle_media: %r" % e)
            self.avdtp["error"] = True

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
        if phase_done(phase, peer) and not peer.pending and time.time() - last_rx > 3.0:
            break
    for l in peer.log: print(l)
    if phase == "fwdnld":
        print("PEER-BOOT ok=%d acks=%d chunks=%d err=%s"
              % (1 if peer.boot_ok else 0, peer.boot_acks, peer.boot_step,
                 peer.boot_err if peer.boot_err else "none"))
    if phase in ("avdtp", "media"):
        # Name each held-back stage the Shokz model would leave the host stuck in, so a gate fails by cause.
        if peer.rev["query_sent"] and peer.rev["answer"] is None:                print("PEER-SDP-QUERY-UNANSWERED")
        if peer.avdtp["discover_pending"] and not peer.rev["done"]:              print("PEER-AVDTP-DISCOVER-HELD (reverse SDP never completed)")
        if peer.avdtp["delay_sent"] and not peer.avdtp["delay_acked"]:           print("PEER-AVDTP-DELAYREPORT-UNANSWERED")
    if phase == "avdtp":
        print("PEER-AVDTP order=%s config=%s"
              % (",".join(str(x) for x in peer.avdtp["order"]),
                 peer.avdtp["config"].hex() if peer.avdtp["config"] else "none"))
    if phase == "media":
        m = peer.media
        print("PEER-MEDIA pkts=%d frames=%d seqgaps=%d badsbc=%d badrtp=%d"
              % (m["pkts"], m["frames"], m["seqgaps"], m["badsbc"], m["badrtp"]))
    print("PEER-DONE phase=%s cmds=%d resets=%d opcodes=%s baud=%s"
          % (phase, len(peer.cmds), peer.resets, ",".join("%04x" % c for c in peer.cmds),
             ",".join(str(b) for b in peer.baud_seen) or "none"))
    ok = phase_done(phase, peer)
    if phase == "fwdnld":
        ok = ok and peer.boot_ok and peer.boot_err is None
    sys.exit(0 if ok else 1)
