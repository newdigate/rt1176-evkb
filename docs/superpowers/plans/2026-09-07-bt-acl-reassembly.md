# L2CAP Reassembly of ACL Continuation Fragments — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The M2Radio BT host reassembles L2CAP PDUs that arrive as several HCI ACL data packets (Packet_Boundary continuation), so a peer that fragments its SDP query — as the Shokz OpenMove does on silicon — gets its answer and AVDTP proceeds.

**Architecture:** `Hci` passes the Packet_Boundary flag to its ACL callback; `L2cap` keeps one reassembly buffer per ACL handle and dispatches a PDU only when its declared length has arrived (zero-copy when a single packet already carries it). The fake peer's `media` phase fragments its SDP query exactly as the headset did, so the existing `[media]` gate is the regression pin (RED on today's library, GREEN on the fix). Spec: `docs/superpowers/specs/2026-09-07-bt-acl-reassembly-design.md`.

**Tech Stack:** C++11 (host tests via `c++`, firmware via ARM GCC 10 + CMake), Python 3 (`hci_peer.py`), the tree's QEMU gates (`./run_qemu_media.sh`), LinkServer for the silicon step.

**Repos:** M2Radio is a sibling checkout at `~/Development/M2Radio` (local-first: evkb builds it from the working tree; push + bump the pin in `~/Development/rt1170/evkb/evkb.cmake` at the end). evkb is `~/Development/rt1170/evkb` on `master`. All work on master (the tree's convention for this programme).

**Order matters:** Task 1 changes the fake peer FIRST and demonstrates the gate RED against the unchanged library — that is the regression demonstration the gate header must quote. Tasks 2–4 are the fix. Task 5 turns the gate GREEN. Task 6 is the close-out. Task 7 is the bench.

---

### Task 1: The fake peer fragments its SDP query; the `[media]` gate goes RED by name

**Files:**
- Modify: `examples/networking/m2_hci_probe/hci_peer.py` (`acl()` at line 157, `rev_maybe_query` at line 637)
- Modify: `examples/audio/bt_tone_test/run_qemu_media.sh` (header comment only)

- [ ] **Step 1: Give `acl()` a Packet_Boundary parameter and add `acl_frag()`**

Replace lines 157–159 of `hci_peer.py` (the `acl` helper) with:

```python
def acl(handle, cid, payload, pb=0x02):           # controller -> host ACL; PB=10 (first, auto-flushable) by default
    hf = (handle & 0x0FFF) | (pb << 12)
    return bytes([0x02]) + struct.pack("<HH", hf, len(payload) + 4) + struct.pack("<HH", len(payload), cid) + payload
def acl_frag(handle, cid, payload, at=13):
    """One L2CAP PDU as TWO HCI ACL packets: a FIRST packet (PB=10) carrying the 4-byte L2CAP header plus the
    first `at` payload bytes, then a CONTINUATION packet (PB=01) with the rest.  This is byte-for-byte how the
    Shokz OpenMove delivered its 22-byte SDP query on silicon (piece-5 soak trace, 2026-09-07: 17 bytes, then
    `03 09 00 09 00`), and a host that does not reassemble answers the truncated first packet with an SDP
    ErrorResponse and drops the tail -- the AVDTP-stall root cause.  Core Vol 4 Part E 5.4.2: the HOST
    reassembles on the PB flag."""
    first = struct.pack("<HH", len(payload), cid) + payload[:at]
    rest  = payload[at:]
    hf1 = (handle & 0x0FFF) | (0x02 << 12)
    hf2 = (handle & 0x0FFF) | (0x01 << 12)
    return (bytes([0x02]) + struct.pack("<HH", hf1, len(first)) + first,
            bytes([0x02]) + struct.pack("<HH", hf2, len(rest)) + rest)
```

- [ ] **Step 2: Fragment the query in the `media` phase only**

Replace the body of `rev_maybe_query` (line 637–641) with:

```python
    def rev_maybe_query(self, handle):
        if self.rev["query_sent"] or not (self.rev["cfg_req_seen"] and self.rev["cfg_rsp_seen"]): return
        self.rev["query_sent"] = True
        # frame 749 of the reference, verbatim: ServiceSearchAttributeRequest, txn 1, {AudioSource 0x110A}, max 32, {0x0009}
        q = bytes.fromhex("060001000d350319110a0020350309000900")
        if self.phase == "media":
            # The `media` peer models the Shokz, which FRAGMENTS this query on silicon (17 + 5 bytes).  Always on
            # here (brainstorm decision 2026-09-07), so every [media] run exercises L2CAP reassembly; the other
            # phases keep sending whole PDUs.
            p1, p2 = acl_frag(handle, self.rev["their_cid"], q, 13)
            self.send(p1, 0.02); self.send(p2, 0.04)
        else:
            self.send(acl(handle, self.rev["their_cid"], q), 0.02)
```

- [ ] **Step 3: Run the `[media]` gate against the UNCHANGED library — expect RED by name**

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test && ./run_qemu_media.sh 2>&1 | tail -8
```

Expected: `FAIL ... [media] the peer's SDP query of our AudioSource record was never answered` (the `PEER-SDP-QUERY-UNANSWERED` tripwire) and/or `[media] the peer never read our AudioSource record`, plus the AVDTP START assertion failing because the peer never answered DISCOVER. Copy the exact failing line(s) — they go in the gate header in Step 4. If the gate PASSES here, STOP: the peer is not fragmenting (check `self.phase == "media"` matches how the gate launches the peer — `grep -n '"media"' run_qemu_media.sh hci_peer.py`).

- [ ] **Step 4: Record the demonstration in the gate header**

Append to the header comment block of `run_qemu_media.sh` (after the `★ HISTORY` block, before the first non-comment line):

```sh
# ★ L2CAP REASSEMBLY REGRESSION (2026-09-07). The peer's SDP query of our
# AudioSource record is sent as TWO ACL packets (PB first + PB continuation,
# 17 + 5 bytes) -- byte-for-byte how the Shokz delivered it on silicon in the
# piece-5 soak, where a host that did not reassemble answered the truncated
# first packet with an SDP ErrorResponse, dropped the tail, and AVDTP stalled
# to its 15 s deadline on 105 of 274 attempts.  DEMONSTRATED RED against the
# pre-reassembly library (M2Radio 9d3da4c):
#     <paste the exact FAIL line(s) from Step 3 here>
# GREEN once L2cap reassembles (M2Radio <sha from Task 6>).  Every [media] run
# now exercises reassembly; the other hci_peer.py phases send whole PDUs.
```

- [ ] **Step 5: Commit (evkb)**

```bash
cd ~/Development/rt1170/evkb && git add examples/networking/m2_hci_probe/hci_peer.py examples/audio/bt_tone_test/run_qemu_media.sh && git commit -m "gate: the [media] peer fragments its SDP query as the Shokz does (PB first + continuation) -- RED by name against the pre-reassembly library

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: `Hci` passes the Packet_Boundary flag to the ACL callback

**Files:**
- Modify: `~/Development/M2Radio/hci/Hci.h:51` (the `AclFn` typedef)
- Modify: `~/Development/M2Radio/hci/Hci.cpp:147-149`
- Test: `~/Development/M2Radio/hci/test/hci_test.cpp` (cases 11 and 13, plus a new case 16)

- [ ] **Step 1: Write the failing tests**

In `hci_test.cpp`, change case 11's callback struct and checks to:

```cpp
    {   // 11. ACL data reaches the ACL callback with the handle masked to 12 bits AND the Packet_Boundary flag
        //     (bits 13:12 of the handle word) passed separately -- 0x2 = first/auto-flushable here.
        FakeIo io; g_io = &io; Hci hci(io); hci.begin();
        struct A { uint16_t h = 0; uint8_t pb = 0xFF; uint16_t len = 0; uint8_t d0 = 0;
                   static void fn(void *c, uint16_t h, uint8_t pb, const uint8_t *d, uint16_t len) { A *a = (A *)c; a->h = h; a->pb = pb; a->len = len; a->d0 = d[0]; } } a;
        hci.onAcl(A::fn, &a);
        io.deliver({0x02, 0x01, 0x20, 0x02, 0x00, 0xAA, 0xBB});   // handle 0x0001 with PB flags 0x2
        hci.service();
        CHECK(a.h == 0x0001); CHECK(a.pb == 0x2); CHECK(a.len == 2); CHECK(a.d0 == 0xAA);
    }
```

In case 13, change the `A` struct the same way (add `uint8_t pb = 0xFF;` and the `uint8_t pb` parameter, storing it) and add `CHECK(a.pb == 0x2);` beside its existing `CHECK(a.h == 0x0001);`.

Append a new case after case 15 (before the final `printf`):

```cpp
    {   // 16. A CONTINUATION fragment (PB = 0b01) reaches the callback as pb == 1 with the same 12-bit handle.
        //     Found 2026-09-07: the Shokz fragments its SDP query (17 + 5 bytes) and the old mask discarded the flag,
        //     so L2cap could never tell a continuation from a new PDU.
        FakeIo io; g_io = &io; Hci hci(io); hci.begin();
        struct A { uint16_t h = 0; uint8_t pb = 0xFF; uint16_t len = 0;
                   static void fn(void *c, uint16_t h, uint8_t pb, const uint8_t *, uint16_t len) { A *a = (A *)c; a->h = h; a->pb = pb; a->len = len; } } a;
        hci.onAcl(A::fn, &a);
        io.deliver({0x02, 0x01, 0x10, 0x03, 0x00, 0x03, 0x09, 0x00});   // handle 0x0001, PB 0x1, 3 bytes
        hci.service();
        CHECK(a.h == 0x0001); CHECK(a.pb == 0x1); CHECK(a.len == 3);
    }
```

- [ ] **Step 2: Run the hci host suite — expect a COMPILE failure (the callback signature does not exist yet)**

```bash
~/Development/M2Radio/hci/test/run.sh 2>&1 | tail -5
```

Expected: `error: ... A::fn ... cannot convert` (or similar) — RED.

- [ ] **Step 3: Change the typedef and the delivery**

`Hci.h:51`:

```cpp
    // ACL data as received: 12-bit handle, the Packet_Boundary flag (bits 13:12 of the handle word: 0b01 =
    // continuation of a fragmented L2CAP PDU, anything else begins one), then the ACL payload.  Core Vol 4
    // Part E 5.4.2: the HOST reassembles on this flag -- L2cap does (2026-09-07).
    typedef void (*AclFn)(void *ctx, uint16_t handle, uint8_t pb, const uint8_t *data, uint16_t len);
```

`Hci.cpp:147-149`:

```cpp
        uint16_t raw    = (uint16_t)(pkt[0] | (pkt[1] << 8));
        uint16_t handle = (uint16_t)(raw & 0x0FFF);
        uint8_t  pb     = (uint8_t)((raw >> 12) & 0x3);
        uint16_t dlen   = (uint16_t)(pkt[2] | (pkt[3] << 8));
        if (m_onAcl) m_onAcl(m_aclCtx, handle, pb, pkt + 4, dlen);
```

- [ ] **Step 4: Run the hci host suite — expect PASS**

```bash
~/Development/M2Radio/hci/test/run.sh 2>&1 | tail -3
```

Expected: `hci_test: N checks, 0 failures` and `HCI-HOST-TESTS: PASS`. (The bt suite will NOT compile until Task 4 updates `A2dpSource`; that is expected between these tasks — do not run it yet.)

- [ ] **Step 5: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add hci/Hci.h hci/Hci.cpp hci/test/hci_test.cpp && git commit -m "hci: AclFn carries the Packet_Boundary flag (a continuation fragment is pb=1); host-tested

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: `L2cap` reassembles continuation fragments

**Files:**
- Modify: `~/Development/M2Radio/bt/L2cap.h` (public: `PB_*`, `onAcl` signature, accessors; private: buffer, counters, `dispatch`)
- Modify: `~/Development/M2Radio/bt/L2cap.cpp` (`begin()`, `onAcl()`, new `dispatch()`)
- Test: `~/Development/M2Radio/bt/test/l2cap_test.cpp` (scenarios R1–R6)

- [ ] **Step 1: Write the failing tests**

Append before the final `printf` in `l2cap_test.cpp`. All six share one helper: a peer-initiated OPEN data channel (the A4a pattern) with `onData` capturing what it is handed.

```cpp
    // ---- R. L2CAP reassembly of ACL continuation fragments (2026-09-07, the piece-5 soak's AVDTP-stall root cause) ----
    // The Shokz delivered its 22-byte SDP query as 17 + 5 bytes (PB first, PB continuation).  Without reassembly the
    // first packet was dispatched as a 13-byte payload (SDP answered with an error) and the tail parsed as garbage.
    struct RxCap { int calls = 0; std::vector<uint8_t> last; uint16_t cid = 0;
                   static void fn(void *c, L2cap::Channel &ch, const uint8_t *p, uint16_t n) { RxCap *r = (RxCap *)c; r->calls++; r->cid = ch.localCid; r->last.assign(p, p + n); } };
    auto openInbound = [](L2cap &l) -> L2cap::Channel * {           // peer opens PSM 0x0001 at us -> our 0x0080, both configs done
        std::vector<uint8_t> rq = l2(0x0001, {0x02, 0x30, 4, 0, 0x01, 0x00, 0x85, 0x0E}); l.onAcl(0x0001, rq.data(), (uint16_t)rq.size()); l.service();
        L2cap::Channel *ch = l.byRemote(0x0E85); if (!ch) return nullptr;
        std::vector<uint8_t> cq = l2(0x0001, {0x04, 0x40, 8, 0, (uint8_t)ch->localCid, (uint8_t)(ch->localCid >> 8), 0, 0, 0x01, 0x02, 0x30, 0x00}); l.onAcl(0x0001, cq.data(), (uint16_t)cq.size());
        std::vector<uint8_t> cr = l2(0x0001, {0x05, 0x50, 6, 0, (uint8_t)ch->localCid, (uint8_t)(ch->localCid >> 8), 0, 0, 0, 0}); l.onAcl(0x0001, cr.data(), (uint16_t)cr.size()); l.service();
        return ch->state == L2cap::OPEN ? ch : nullptr; };
    // The Shokz's PDU, as two ACL packets: [len=0x12][cid=0x0080] + 13 payload bytes, then the 5-byte tail.
    const std::vector<uint8_t> SHOKZ_Q = {0x06,0x00,0x01,0x00,0x0D,0x35,0x03,0x19,0x11,0x0A,0x00,0x20,0x35,0x03,0x09,0x00,0x09,0x00};   // 18 bytes
    auto frag1 = [&]() { std::vector<uint8_t> v = {0x12, 0x00, 0x80, 0x00}; v.insert(v.end(), SHOKZ_Q.begin(), SHOKZ_Q.begin() + 13); return v; };   // 17 bytes
    auto frag2 = [&]() { return std::vector<uint8_t>(SHOKZ_Q.begin() + 13, SHOKZ_Q.end()); };                                             // 5 bytes
    {   // R1. 17 + 5 (pb 2 then 1) -> onData fires ONCE with the whole 18-byte payload; one fragment consumed, no drops.
        CapIo io; L2cap l(io); l.begin(0x0001, 20); l.acceptIncoming(true); l.allowPsm(0x0001);
        L2cap::Channel *ch = openInbound(l); CHECK(ch);
        RxCap rx; l.onData(RxCap::fn, &rx);
        std::vector<uint8_t> a = frag1(), b = frag2();
        l.onAcl(0x0001, a.data(), (uint16_t)a.size(), L2cap::PB_FIRST);
        CHECK(rx.calls == 0);                                                    // nothing dispatched yet -- the RED line today (calls==1, 13 bytes)
        l.onAcl(0x0001, b.data(), (uint16_t)b.size(), L2cap::PB_CONT);
        CHECK(rx.calls == 1 && rx.last == SHOKZ_Q && rx.cid == 0x0080);
        CHECK(l.reasmFrags() == 1 && l.reasmDrops() == 0);
    }
    {   // R2. A continuation with nothing pending is dropped and counted; nothing dispatched.
        CapIo io; L2cap l(io); l.begin(0x0001, 20); l.acceptIncoming(true); l.allowPsm(0x0001);
        CHECK(openInbound(l)); RxCap rx; l.onData(RxCap::fn, &rx);
        std::vector<uint8_t> b = frag2(); l.onAcl(0x0001, b.data(), (uint16_t)b.size(), L2cap::PB_CONT);
        CHECK(rx.calls == 0 && l.reasmDrops() == 1 && l.reasmFrags() == 0);
    }
    {   // R3. A new FIRST packet while a partial is pending discards the partial (counted); the new PDU is delivered.
        CapIo io; L2cap l(io); l.begin(0x0001, 20); l.acceptIncoming(true); l.allowPsm(0x0001);
        CHECK(openInbound(l)); RxCap rx; l.onData(RxCap::fn, &rx);
        std::vector<uint8_t> a = frag1(); l.onAcl(0x0001, a.data(), (uint16_t)a.size(), L2cap::PB_FIRST);
        std::vector<uint8_t> whole = l2(0x0080, {0x06, 0x00, 0x02, 0x00, 0x00});                     // a complete 5-byte PDU
        l.onAcl(0x0001, whole.data(), (uint16_t)whole.size(), L2cap::PB_FIRST);
        CHECK(rx.calls == 1 && rx.last.size() == 5 && rx.last[2] == 0x02);
        CHECK(l.reasmDrops() == 1);
        std::vector<uint8_t> b = frag2(); l.onAcl(0x0001, b.data(), (uint16_t)b.size(), L2cap::PB_CONT);   // the orphaned tail
        CHECK(rx.calls == 1 && l.reasmDrops() == 2);
    }
    {   // R4. A declared length above what we can hold (RX_MTU + 4) is refused on the first packet, counted.
        CapIo io; L2cap l(io); l.begin(0x0001, 20); l.acceptIncoming(true); l.allowPsm(0x0001);
        CHECK(openInbound(l)); RxCap rx; l.onData(RxCap::fn, &rx);
        std::vector<uint8_t> big = {0xF0, 0x03, 0x80, 0x00, 1, 2, 3};                                  // length 0x03F0 = 1008 > 1004
        l.onAcl(0x0001, big.data(), (uint16_t)big.size(), L2cap::PB_FIRST);
        std::vector<uint8_t> b = frag2(); l.onAcl(0x0001, b.data(), (uint16_t)b.size(), L2cap::PB_CONT);
        CHECK(rx.calls == 0 && l.reasmDrops() >= 1);
    }
    {   // R5. A first packet shorter than the 4-byte L2CAP header (2 bytes) still reassembles once the rest arrives.
        CapIo io; L2cap l(io); l.begin(0x0001, 20); l.acceptIncoming(true); l.allowPsm(0x0001);
        CHECK(openInbound(l)); RxCap rx; l.onData(RxCap::fn, &rx);
        std::vector<uint8_t> a = frag1(); std::vector<uint8_t> a1(a.begin(), a.begin() + 2), a2(a.begin() + 2, a.end());
        l.onAcl(0x0001, a1.data(), (uint16_t)a1.size(), L2cap::PB_FIRST);
        l.onAcl(0x0001, a2.data(), (uint16_t)a2.size(), L2cap::PB_CONT);
        CHECK(rx.calls == 0);
        std::vector<uint8_t> b = frag2(); l.onAcl(0x0001, b.data(), (uint16_t)b.size(), L2cap::PB_CONT);
        CHECK(rx.calls == 1 && rx.last == SHOKZ_Q && l.reasmFrags() == 2);
    }
    {   // R6. reset() between fragments discards the partial: a stale tail after a reconnect must not be delivered.
        CapIo io; L2cap l(io); l.begin(0x0001, 20); l.acceptIncoming(true); l.allowPsm(0x0001);
        CHECK(openInbound(l)); RxCap rx; l.onData(RxCap::fn, &rx);
        std::vector<uint8_t> a = frag1(); l.onAcl(0x0001, a.data(), (uint16_t)a.size(), L2cap::PB_FIRST);
        l.reset(); l.begin(0x0001, 20); l.acceptIncoming(true); l.allowPsm(0x0001); CHECK(openInbound(l));
        std::vector<uint8_t> b = frag2(); l.onAcl(0x0001, b.data(), (uint16_t)b.size(), L2cap::PB_CONT);
        CHECK(rx.calls == 0 && l.reasmDrops() == 1);
    }
```

(`openInbound` uses PSM 0x0001 with `allowPsm(0x0001)`; if `allowPsm` is not the accessor's name, use the one A4a/A5 use — `grep -n "allow" bt/L2cap.h`.)

- [ ] **Step 2: Run the bt suite — expect a COMPILE failure (`PB_FIRST`, `reasmFrags` do not exist)**

```bash
~/Development/M2Radio/bt/test/run.sh 2>&1 | grep -m3 "error"
```

- [ ] **Step 3: Implement**

`L2cap.h` — in the public section, replace the `onAcl` declaration (line 34) with:

```cpp
    // Packet_Boundary values as Hci::AclFn passes them: only 0b01 continues a fragmented PDU; 0b00/0b10/0b11 all begin one.
    static const uint8_t PB_CONT = 0x1, PB_FIRST = 0x2;
    // One ACL packet as received.  Reassembles L2CAP PDUs split across packets (Core Vol 4 Part E 5.4.2 -- the
    // HOST's job): a packet that already carries its whole declared PDU is dispatched in place (zero copy); a
    // short first packet is held and completed by continuations.  pb defaults to FIRST so a caller feeding whole
    // PDUs (every host test) is unchanged.
    void onAcl(uint16_t handle, const uint8_t *d, uint16_t len, uint8_t pb = PB_FIRST);
    uint32_t reasmFrags() const { return m_reasmFrags; }   // continuation fragments consumed into delivered PDUs
    uint32_t reasmDrops() const { return m_reasmDrops; }   // partial PDUs discarded (orphan tail, superseded, oversize)
```

In the private section, after the `Tx m_txq[TXQ] ...` line, add:

```cpp
    // Reassembly (one ACL handle per L2cap): the largest PDU the peer may send us is RX_MTU (we advertised it).
    uint8_t  m_rx[RX_MTU + 4] = {}; uint16_t m_rxLen = 0, m_rxNeed = 0;   // held bytes; declared length + 4 (0 = not yet known)
    uint32_t m_reasmFrags = 0, m_reasmDrops = 0;
    void dispatch(const uint8_t *d, uint16_t len);                       // one complete PDU: trace-free demux
```

`L2cap.cpp` — in `begin()`, after `m_creditsMin = credits;` add:

```cpp
    m_rxLen = m_rxNeed = 0; m_reasmFrags = m_reasmDrops = 0;
```

Replace `L2cap::onAcl` (line 47–54) with:

```cpp
void L2cap::onAcl(uint16_t handle, const uint8_t *d, uint16_t len, uint8_t pb) {
    if (handle != m_handle) return;
    if (m_trace) m_trace(m_traceCtx, false, handle, d, len);          // raw, per ACL packet, BEFORE reassembly: the
                                                                        // instrument that found the fragmentation defect
    if (pb == PB_CONT) {
        if (m_rxLen == 0) { m_reasmDrops++; return; }                   // a tail with no head
        if ((uint32_t)m_rxLen + len > sizeof m_rx) { m_rxLen = m_rxNeed = 0; m_reasmDrops++; return; }
        memcpy(m_rx + m_rxLen, d, len); m_rxLen = (uint16_t)(m_rxLen + len); m_reasmFrags++;
        if (m_rxNeed == 0 && m_rxLen >= 4) {
            m_rxNeed = (uint16_t)((m_rx[0] | (m_rx[1] << 8)) + 4);
            if (m_rxNeed > sizeof m_rx) { m_rxLen = m_rxNeed = 0; m_reasmDrops++; return; }
        }
        if (m_rxNeed && m_rxLen >= m_rxNeed) { dispatch(m_rx, m_rxNeed); m_rxLen = m_rxNeed = 0; }
        return;
    }
    if (m_rxLen) { m_rxLen = m_rxNeed = 0; m_reasmDrops++; }            // a new PDU abandons a pending partial
    if (len >= 4) {
        uint16_t need = (uint16_t)((d[0] | (d[1] << 8)) + 4);
        if (len >= need) { dispatch(d, need); return; }                 // whole PDU in one packet: zero copy (the common case)
        if (need > sizeof m_rx) { m_reasmDrops++; return; }             // longer than we advertised: the peer's fault
        m_rxNeed = need;
    }
    if (len > sizeof m_rx) { m_reasmDrops++; m_rxNeed = 0; return; }
    memcpy(m_rx, d, len); m_rxLen = len;                                // short first packet: hold it
}
void L2cap::dispatch(const uint8_t *d, uint16_t len) {
    if (len < 4) return;
    uint16_t cid = (uint16_t)(d[2] | (d[3] << 8));
    if (cid == 0x0001) { handleSig(d, len); return; }
    Channel *ch = byLocal(cid);
    if (ch && m_onData) m_onData(m_dataCtx, *ch, d + 4, (uint16_t)(len - 4));
}
```

Note the one behavioural change for whole packets: `dispatch(d, need)` passes the DECLARED PDU length, not the packet length. Today `handleSig`/`m_onData` got the packet length. They are equal for every packet the fake peer and the IW416 have produced; a packet with trailing bytes beyond its L2CAP length was never seen. The gate sweep in Task 6 is the check.

- [ ] **Step 4: Run the bt suite — expect PASS**

```bash
~/Development/M2Radio/bt/test/run.sh 2>&1 | grep -E "l2cap_test|BT-HOST"
```

Expected: `l2cap_test: N checks, 0 failures` … `BT-HOST-TESTS: PASS`. (`a2dpsource_test`/`btsession_test` may fail to COMPILE until Task 4 — if so, run only l2cap: `c++ -std=c++11 -Wall -Wextra -Werror -I bt -I hci bt/test/l2cap_test.cpp bt/*.cpp hci/H4Parser.cpp hci/Hci.cpp hci/HciEvents.cpp -o /tmp/l2t && /tmp/l2t` from `~/Development/M2Radio`; if `A2dpSource.cpp` fails to compile, do Task 4 Step 1 first, then return.)

- [ ] **Step 5: Mutation-test the pins on a SCRATCH copy** (never commit a mutant)

```bash
cd ~/Development/M2Radio && cp bt/L2cap.cpp /tmp/L2cap.cpp.keep
# mutant 1: continuation appended without a pending check (delete the `if (m_rxLen == 0) ...` line) -> R2 must FAIL
# mutant 2: m_reasmFrags++ removed -> R1/R5 must FAIL
# mutant 3: begin() not clearing m_rxLen (delete the added line) -> R6 must FAIL
# mutant 4: `if (m_rxLen) { ... m_reasmDrops++; }` removed -> R3 must FAIL
# after each: ./bt/test/run.sh 2>&1 | grep -c "^FAIL" ; then cp /tmp/L2cap.cpp.keep bt/L2cap.cpp
```

Expected: each mutant produces at least one `FAIL` line naming the scenario; the restored file passes. Record the four outcomes in the commit message.

- [ ] **Step 6: Commit (M2Radio)**

```bash
cd ~/Development/M2Radio && git add bt/L2cap.h bt/L2cap.cpp bt/test/l2cap_test.cpp && git commit -m "bt: L2cap reassembles ACL continuation fragments (PB flag) -- the Shokz's 17+5-byte SDP query now yields one PDU; R1-R6 host-tested, four mutants RED

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: Consumers — `A2dpSource`, the four firmware thunks, the heartbeat fields

**Files:**
- Modify: `~/Development/M2Radio/bt/A2dpSource.h:55`
- Modify: `examples/audio/bt_tone_test/bt_tone_test.cpp:437` (thunk) and `:592-596` (heartbeat)
- Modify: `examples/display/acid_box/acid_box.cpp:351`
- Modify: `examples/networking/m2_hci_probe/m2_hci_probe.cpp:404-406` and `:415`

- [ ] **Step 1: `A2dpSource` forwards `pb`**

`A2dpSource.h:55`:

```cpp
    void onAcl(uint16_t h, const uint8_t *d, uint16_t len, uint8_t pb = L2cap::PB_FIRST) { m_l2.onAcl(h, d, len, pb); }
```

Run the whole bt suite now — it must compile and PASS: `~/Development/M2Radio/bt/test/run.sh 2>&1 | tail -2` → `BT-HOST-TESTS: PASS`. Commit (M2Radio): `git commit -am "bt: A2dpSource::onAcl forwards the Packet_Boundary flag"` (with the trailer).

- [ ] **Step 2: The four firmware thunks**

`bt_tone_test.cpp:437`:
```cpp
static void onAclThunk(void *, uint16_t h, uint8_t pb, const uint8_t *d, uint16_t l) { src.onAcl(h, d, l, pb); }
```
`acid_box.cpp:351`:
```cpp
static void onAclThunk(void *, uint16_t h, uint8_t pb, const uint8_t *d, uint16_t l) { src.onAcl(h, d, l, pb); }
```
`m2_hci_probe.cpp:404-406`:
```cpp
static void onAclThunk(void *, uint16_t handle, uint8_t pb, const uint8_t *d, uint16_t len) {
    l2.onAcl(handle, d, len, pb);
}
```
`m2_hci_probe.cpp:415`:
```cpp
static void onAclThunk(void *, uint16_t handle, uint8_t pb, const uint8_t *d, uint16_t len) { src.onAcl(handle, d, len, pb); }
```
Then check for any other `AclFn` consumer the grep above missed: `grep -rn "hci.onAcl(" ~/Development/rt1170/evkb/examples --include=*.cpp | grep -v build` — every named handler must have the new signature (the `lbOnAcl` at `m2_hci_probe.cpp:603` — open it and add the `uint8_t pb` parameter after `handle`, forwarding it if it calls `onAcl`).

- [ ] **Step 3: Heartbeat fields — BEFORE `credmin=`, never after**

`run_qemu_soak.sh:255` greps `^bt_hci .* credmin=[0-9]+$` (anchored at end of line), so the new fields must precede `credmin`. Replace `bt_tone_test.cpp:595-596`:

```cpp
        CONSOLE.print(" l2drop="); CONSOLE.print(src.l2().dropped());
        CONSOLE.print(" l2frag="); CONSOLE.print(src.l2().reasmFrags());        // ACL continuation fragments reassembled
        CONSOLE.print(" l2fragdrop="); CONSOLE.print(src.l2().reasmDrops());    // partial PDUs discarded
        CONSOLE.print(" credmin="); CONSOLE.println(src.l2().creditsMin());
```

Confirm no other gate greps the line's shape: `grep -rn "bt_hci" ~/Development/rt1170/evkb/examples/*/*/run_qemu*.sh ~/Development/rt1170/evkb/tools/gate-vacuity.test.sh` — read each hit.

- [ ] **Step 4: Build every touched example (all gate build dirs + the bench dirs)**

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test && for b in build build-media build-lifecycle build-soak build-soak-hw; do cmake --build $b -j 8 2>&1 | grep -E "error|Built target bt_tone_test$" ; done
cd ../../networking/m2_hci_probe && for b in build build-baud build-avdtp build-reconnect; do [ -d $b ] && cmake --build $b -j 8 2>&1 | grep -E "error|Built target m2_hci_probe$"; done
cd ../../display/acid_box && cmake --build build -j 8 2>&1 | grep -E "error|Built target acid_box$"; ls -d build-bt* 2>/dev/null | while read b; do cmake --build $b -j 8 2>&1 | grep -E "error|Built target"; done
```

Expected: `Built target …` for each, no `error`. If a build dir fails to CONFIGURE with "toolchain file not found" / `COMPILERPATH is UNDEFINED`, `rm -rf` it and configure fresh with the example's documented command (CLAUDE.md, the pre-2026-08-14 trap).

- [ ] **Step 5: Commit (evkb)**

```bash
cd ~/Development/rt1170/evkb && git add examples/audio/bt_tone_test/bt_tone_test.cpp examples/display/acid_box/acid_box.cpp examples/networking/m2_hci_probe/m2_hci_probe.cpp && git commit -m "bt examples: ACL thunks pass the Packet_Boundary flag; bt_tone_test heartbeat logs l2frag/l2fragdrop (before credmin -- the [soak] gate anchors on it)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: The `[media]` gate goes GREEN; fixture re-captured; the other bt gates re-run

**Files:**
- Modify: `examples/audio/bt_tone_test/transcript_qemu_media.txt` (re-capture)
- Modify: `examples/audio/bt_tone_test/run_qemu_media.sh` (fill the `<sha>` placeholder later, Task 6)

- [ ] **Step 1: Run `[media]` — expect PASS**

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test && ./run_qemu_media.sh 2>&1 | tail -4
```

Expected: `PASS` and, in `build-media/serial.uart`, a `bt_hci` line with `l2frag=1` (one continuation consumed — the SDP query) — check: `grep -o "l2frag=[0-9]* l2fragdrop=[0-9]*" build-media/serial.uart | sort | uniq -c`. Expected `l2frag=1 l2fragdrop=0` once streaming. If `l2frag=0`, the peer did not fragment: back to Task 1 Step 3.

- [ ] **Step 2: Re-capture the fixture and run the vacuity suite**

```bash
cp build-media/serial.uart transcript_qemu_media.txt
cd ~/Development/rt1170/evkb && ./tools/gate-vacuity.test.sh 2>&1 | tail -3
```

Expected: `43` PASS lines, 0 FAIL (`grep -c "^PASS:"` on the output).

- [ ] **Step 3: Run the other bt-linking gates against the new library**

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test && for g in run_qemu.sh run_qemu_lifecycle.sh run_qemu_soak.sh; do ./$g 2>&1 | tail -1; done
cd ../../networking/m2_hci_probe && for g in run_qemu.sh run_qemu_hci.sh run_qemu_baud.sh run_qemu_avdtp.sh run_qemu_reconnect.sh; do ./$g 2>&1 | tail -1; done
cd ../../display/acid_box && ./run_qemu.sh 2>&1 | tail -1
```

Expected: nine PASS lines. `[hci]`/`[media]`/`[avdtp]` are timing-sensitive on the socket attach — re-run idle once if one flakes; a second failure is a regression (the `dispatch(d, need)` length change is the first suspect — compare `serial.uart` against the committed transcript).

- [ ] **Step 4: Commit (evkb)**

```bash
cd ~/Development/rt1170/evkb && git add examples/audio/bt_tone_test/transcript_qemu_media.txt && git commit -m "gate: [media] GREEN with L2CAP reassembly (l2frag=1 on the fragmented SDP query); fixture re-captured, vacuity 43/43

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: Close-out — push, pin, fresh-user, rebuild, sweep, audit, docs

**Files:**
- Modify: `evkb.cmake` (M2Radio pin line 119), `examples/audio/bt_tone_test/run_qemu_media.sh` (the `<sha>`), `CLAUDE.md`, `docs/superpowers/specs/2026-09-06-bt-resilience-soak-design.md` (§8.3 stub), memory

- [ ] **Step 1: Push M2Radio, bump the pin**

```bash
cd ~/Development/M2Radio && git push origin master && git log --oneline -1
```

In `evkb.cmake:119` replace `9d3da4c67b9ac3cc023c2bb974ac0bbecbc4def7` with the new full SHA (`git -C ~/Development/M2Radio rev-parse HEAD`) and append to that line's comment: ` 2026-09-07: L2cap reassembles ACL continuation fragments (Hci::AclFn carries the PB flag -- a SIGNATURE change: every AclFn thunk must take uint8_t pb after the handle; bt_tone_test/acid_box/m2_hci_probe updated together). The [media] peer now fragments its SDP query, so bt_tone_test[media] will not PASS against anything older.` Fill the `<sha from Task 6>` placeholder in `run_qemu_media.sh` with the short SHA.

- [ ] **Step 2: Fresh-user verification — RUN the gate against the fetched ELF**

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test && rm -rf /tmp/ff-media && cmake -B /tmp/ff-media -DCMAKE_TOOLCHAIN_FILE=$PWD/../../../toolchain/rt1170-evkb.toolchain.cmake -DEVKB_FORCE_FETCH=ON -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 2>&1 | grep -E "M2Radio|clone|error" ; cmake --build /tmp/ff-media -j 8 2>&1 | tail -1
mv build-media build-media.local && ln -s /tmp/ff-media build-media && ./run_qemu_media.sh 2>&1 | tail -1; rm build-media && mv build-media.local build-media
```

Expected: the configure log shows the GitHub clone at the new SHA; the gate PASSes against that ELF. (Check `run_qemu_media.sh` for the exact `-D` set its own configure uses — `grep -n "cmake -B" run_qemu_media.sh` — and mirror it.)

- [ ] **Step 3: Rebuild EVERY bt-linking gate ELF and check freshness by symbol**

```bash
cd ~/Development/rt1170/evkb && for d in examples/audio/bt_tone_test/build examples/audio/bt_tone_test/build-media examples/audio/bt_tone_test/build-lifecycle examples/audio/bt_tone_test/build-soak examples/networking/m2_hci_probe/build examples/networking/m2_hci_probe/build-baud examples/networking/m2_hci_probe/build-avdtp examples/networking/m2_hci_probe/build-reconnect examples/display/acid_box/build; do cmake --build $d -j 8 >/dev/null 2>&1 && printf "%-60s " $d && (/Applications/ARM_10/bin/arm-none-eabi-nm $d/*.elf | grep -q "reasmFrags\|_ZN5L2cap8dispatch" && echo fresh || echo STALE); done
```

Expected: nine `fresh` (the `dispatch` symbol only exists in the new library). `bt_tone_test/build` (card-absent) links L2cap too, so it must read fresh.

- [ ] **Step 4: The sweep, then the audit (never concurrently)**

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh -l | tail -1 && ./tools/run-all-qemu-gates.sh > /tmp/sweep-reasm.txt 2>&1; tail -3 /tmp/sweep-reasm.txt
LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh 2>&1 | tail -1
```

Expected: `-l` reports 132 (no new gate); `gates: 132 passed`, exit 0 — or one load-sensitivity red (`cm4_audio_test`, `m2_rx_demo[txaggr]`, `m2_uap_lwip[uap]`, `bt_tone_test[media]` budget) that passes idle on re-run; `LICENSE-AUDIT: PASS`. Read the gate NAMES, not the count.

- [ ] **Step 5: Docs**

`CLAUDE.md`: in the piece-5 block (the paragraph beginning `★ **The 2 h SILICON soak RAN 2026-09-07`), append a ★ paragraph: the root cause (host never reassembled ACL continuation fragments — `Hci` masked the PB flag, `L2cap::onAcl` treated every packet as a PDU; the Shokz's 22-byte SDP query arrived 17 + 5, the SDP server answered an error, DISCOVER was never answered), the fix (L2cap reassembly, `AclFn` signature change — every thunk takes `pb`), the proof (R1–R6 + four mutants; the `[media]` peer fragments always, RED by name against 9d3da4c, GREEN on the new pin, no new gate, 132), the two OPEN observations (fragmentation grows with headset uptime; the traced build fragmented less), and the sweep line `✅ Measured 2026-09-07: 132 … ` with the actual numbers. Also fix the Architecture-section claim if any says the host reassembles (`grep -n "reassembl" CLAUDE.md`).
Soak spec `2026-09-06-bt-resilience-soak-design.md`: add `### 8.3 Root cause and fix` pointing at the reassembly spec and leaving one line for Task 7's numbers: `Silicon re-run: <pending Task 7>`.
Memory `new34-bt-reconnect.md` + `MEMORY.md` index line: piece 5 root cause found + fixed in software, silicon re-run pending.

- [ ] **Step 6: Commit and push (evkb)**

```bash
cd ~/Development/rt1170/evkb && git add evkb.cmake CLAUDE.md docs examples/audio/bt_tone_test/run_qemu_media.sh && git commit -m "build+docs: M2Radio pin <sha> (L2CAP reassembly of ACL continuation fragments); [media] RED/GREEN recorded; sweep 132/132, audit PASS, fresh-user verified

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" && git fetch origin && git rebase origin/master && git push origin master
```

---

### Task 7: Silicon — the untraced soak re-run against the degraded Shokz

**Files:**
- Modify: `examples/audio/bt_tone_test/transcript_hw_evkb.txt` (SOAK section addendum), soak spec §8.3, `CLAUDE.md`, memory

- [ ] **Step 1: Rebuild `build-soak-hw` on the new library and flash it (VCOM detached)**

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test && cmake --build build-soak-hw -j 8 2>&1 | tail -1 && /Applications/ARM_10/bin/arm-none-eabi-nm build-soak-hw/bt_tone_test.elf | grep -c "_ZN5L2cap8dispatch"
pkill -f rt1170-console.py; pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 2
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build-soak-hw/bt_tone_test.hex 2>&1 | tail -2
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build-soak-hw/bt_tone_test.hex 2>&1 | grep -i "matches"
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
```

Expected: `1` (fresh), `File matches flash`.

- [ ] **Step 2: Attach the reader, ask for SW4, run 30 minutes with the Shokz NOT power-cycled**

```bash
cd ~/Development/rt1170/evkb && nohup /usr/local/Caskroom/miniconda/base/bin/python3 tools/rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > /tmp/p5soak6.log 2>&1 &
```

The headset must be in its DEGRADED state (hours of uptime, NOT power-cycled since the controls) so the comparison is against the 47–60 % stall rate it showed the old library minutes earlier. Ask the user to press SW4; confirm `bonds_boot=1` and the first `a2dp=ok`. After 30 min, report with the piece-5 helpers:

```bash
S=/private/tmp/claude-501/-Users-nicholasnewdigate-Development-rt1170-evkb/8e94b9bd-b541-41da-87f4-0f7c76be8fca/scratchpad
python3 $S/soak_windows.py /tmp/p5soak6.log 10; grep -o "l2frag=[0-9]* l2fragdrop=[0-9]*" /tmp/p5soak6.log | tail -1; grep "^bt_soak" /tmp/p5soak6.log | tail -1
```

Expected (the claim): `avdtp_failed` near zero across every window (the class is gone), `l2frag` counting UP (fragments being consumed — the direct witness that the headset is still fragmenting and we now cope), `l2fragdrop=0`, `reconnects == cycles`, `fails == 0`, invariants flat. If stalls persist with `l2frag` climbing, the stall has a SECOND cause — decode with `$S/stall_decode.py` on a traced build; do not re-golden.

- [ ] **Step 3: Record**

Soak spec §8.3: the numbers; a fresh ✅ line in `CLAUDE.md` if the acceptance is now MET ("piece 5 CLOSED: structural signature flat AND reconnects within bound over N cycles on the reassembling host"); `transcript_hw_evkb.txt` SOAK addendum with the first/last `bt_soak` lines and the `l2frag` witness; memory. Commit + push evkb; comment on NEW-34.

---

## Self-review

- **Spec coverage:** §2.1 (Hci flag, defaulted `onAcl`) → Tasks 2, 3, 4. §2.2 mechanism + counters + heartbeat → Tasks 3, 4. §2.3 unchanged trace → Task 3 Step 3. §3.1 R1–R6 + mutants, hci cases → Tasks 2, 3. §3.2 media peer, RED first, fixture, vacuity, count 132 → Tasks 1, 5. §3.3 close-out → Task 6. §3.4 silicon → Task 7.
- **Placeholders:** the only deliberate ones are the `<sha>` fills in Task 1 Step 4 / Task 6 Step 1 and `<pending Task 7>`, each with the step that fills it.
- **Consistency:** `PB_FIRST`/`PB_CONT` (Task 3) are what Task 4's forwarder and the tests use; `reasmFrags()`/`reasmDrops()` names match across Tasks 3, 4, 5, 7; the callback order `(ctx, handle, pb, data, len)` is identical in Task 2's typedef, its tests and Task 4's four thunks.
