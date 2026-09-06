# BT Link Lifecycle (NEW-34 piece 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A2DP streaming that survives the link — detect the drop, tear the media path down, keep the UI/local audio alive, and reconnect in either direction (we page the lost peer; the headset pages us) without a reboot — with per-second health numbers a soak can assert on.

**Architecture:** Approach B — the whole link layer below `Hci` becomes non-blocking. `BtLink` becomes an operation engine (PREPARE/INQUIRY/PAGE/PAIR/DISCONNECT + a page-scan side channel + the incoming-page decision + link state), `A2dpSource` becomes one bring-up **attempt** in either direction (with an AVDTP **acceptor** beside the initiator), and a new `BtSession` holds the policy (boot walk + inquiry, lost-peer retry forever, page-scan-when-idle, retry-cancel-on-incoming, MANUAL mode, callbacks, stats). Every wait is a state with a deadline advanced by one `tick(now)` per loop pass. `BtFwLoader::run` and the boot identity commands stay blocking.

**Tech Stack:** C++11 (M2Radio `bt/`, host-compiled with `-Wall -Wextra -Werror`), Arduino/Teensyduino (evkb examples), ARM GCC 10, CMake, the custom `mimxrt1170-evk` QEMU machine + Python fake HCI peer, `tools/gate-lib.sh` gate harness.

**Spec:** `docs/superpowers/specs/2026-09-06-bt-link-lifecycle-design.md` (read it before starting; every design decision and its rejected alternative is recorded there).

---

## Ground rules (read once, apply in every task)

- **Repos.** Library code is `~/Development/M2Radio` (its own git repo, currently HEAD `63101a6`). Firmware/gates/docs are `~/Development/rt1170/evkb` (this repo, on a NEW branch — see Task 0). They are pushed and pinned separately; `evkb.cmake` pins M2Radio by SHA. Never edit files under an example's `build*/` directory.
- **Host suites run FROM `~/Development/M2Radio`** (`bt/test/run.sh`), never from evkb — a suite run from the wrong cwd writes stray artefacts (`sine.sbc`) into the evkb tree. The runner GLOBs `bt/*.cpp`, so a new `bt/BtSession.cpp` is compiled into every suite automatically; a new `bt/test/btsession_test.cpp` is picked up by the `for t in ...` loop only if its name is added to that loop (Task 6).
- **Gates do not build.** Build the example before running its gate, or the sweep reports SKIP (which hides in a count). Run a QEMU gate as `./run_qemu_X.sh`, never `sh run_qemu_X.sh` (it re-execs under `gtimeout`). One real QEMU user at a time; never run the licence audit concurrently with the sweep.
- **Concurrency (subagent execution).** Only ONE implementer touches a given git working tree at a time. Commit with an explicit pathspec (`git commit -m ... -- <paths>`), never `git add -A`, so a concurrent reviewer's staged file is not swept into your commit. Mutation-testing reviewers mutate COPIES in the scratchpad, never the working tree.
- **Commit attribution.** End every commit message with:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`
- **Piece 1 is the reference for discipline.** Every new host-test pin must be shown RED by a mutant before it is trusted. Every gate assertion must be demonstrated RED by name. The `[media]`/`[reconnect]` gate transcripts are re-captured whenever the example's output changes, and the vacuity suite is what catches a stale fixture.
- **The `now`/deadline idiom** used throughout the rewrite: a waiting sub-state stores `m_deadline = now + T` on entry and tests `(int32_t)(now - m_deadline) >= 0` for expiry. Never compare `now - t0 < T` (the old blocking idiom) — there is no `t0` in a tick model.

## File structure

**M2Radio `bt/` (library):**
- `L2cap.{h,cpp}` — add `allowPsm()`, `reset()`, `nextInbound()`, `creditsMin()`/`resetCreditsMin()` (Task 1).
- `MediaPacketizer.{h,cpp}` — `begin(mtu, frameBytes)` so the batcher uses the negotiated frame size, not a hardcoded 119 (Task 2).
- `Avdtp.{h,cpp}` — an acceptor role beside the initiator (Task 2).
- `BtLink.{h,cpp}` — non-blocking operation engine (Task 3) + incoming page, link state, address checks, supervision knob (Task 4).
- `A2dpSource.{h,cpp}` — one attempt in either direction, teardown, config adoption (Task 5).
- `BtSession.{h,cpp}` — NEW; the policy (Task 6).
- `test/{l2cap,avdtp,btlink,a2dpsource}_test.cpp` re-shaped; NEW `test/btsession_test.cpp`; `test/run.sh` grows one name (Tasks 1–6).

**evkb `examples/`:**
- `audio/bt_tone_test/AudioOutputBluetooth.{h,cpp}` — `end()`, paused state, idle/paused counters (Task 7).
- `audio/bt_tone_test/bt_tone_test.cpp` + `CMakeLists.txt` — onto the session + health line; `run_qemu_lifecycle.sh` + `build-lifecycle/` + `transcript_qemu_lifecycle.txt` (Tasks 8, 12); `transcript_qemu_media.txt` recaptured (Task 13).
- `display/acid_box/acid_box.cpp` + `CMakeLists.txt` — onto the session + health line (Task 9).
- `networking/m2_hci_probe/{m2_hci_probe.cpp, hci_peer.py, transcript_qemu_reconnect.txt}` — reconnect helper onto the session; the `lifecycle` peer phase; reconnect transcript recaptured (Tasks 10, 11).

**evkb `tools/` and root:**
- `tools/gate-vacuity.test.sh` — `[lifecycle]` negatives (Task 13).
- `tools/license-audit.sh` — `build-lifecycle` GATES entry (Task 14).
- `evkb.cmake` (M2Radio pin), `CLAUDE.md`, `docs/KNOWN-BROKEN-GATES.md` (Task 14).

---

## Task 0: Branch

**Files:** none (git only).

- [ ] **Step 1: Create the evkb feature branch**

```bash
cd ~/Development/rt1170/evkb && git checkout master && git pull --ff-only && git checkout -b nicnewdigate/new-34-piece2-link-lifecycle && git status
```
Expected: `On branch nicnewdigate/new-34-piece2-link-lifecycle`, working tree clean. (The spec commit `2bb5b36` is already on master.)

- [ ] **Step 2: Confirm M2Radio is on master at the pinned SHA**

```bash
cd ~/Development/M2Radio && git checkout master && git pull --ff-only && git rev-parse --short HEAD
```
Expected: `63101a6` (or a descendant; if a descendant, note it — the pin in `evkb.cmake` will be bumped in Task 14 regardless).

---

## Task 1: L2cap — inbound PSM allow-list, reset, inbound iterator, credit floor

**Files:**
- Modify: `~/Development/M2Radio/bt/L2cap.h`
- Modify: `~/Development/M2Radio/bt/L2cap.cpp`
- Test: `~/Development/M2Radio/bt/test/l2cap_test.cpp`

**Context for the implementer:** `L2cap` is basic-mode L2CAP over one ACL link (read `bt/L2cap.h` and `bt/L2cap.cpp` in full first). Today `acceptIncoming(true)` accepts an inbound CONN_REQ for ANY PSM into a free channel (see `service()`'s `if (m_accept) for (auto &c : m_ch) ...`). Piece 2 needs: (a) accept only AVDTP (0x0019) and SDP (0x0001) inbound, refusing others with result 0x0002 (PSM not supported) — a headset opens an AVCTP (0x0017) channel for AVRCP, which is piece 3, so it must be refused now, not silently accepted into a slot; (b) `reset()` to drop every channel between attempts; (c) an iterator over peer-initiated OPEN channels so the AVDTP acceptor can find the signalling/media channels the headset opened; (d) a running minimum of the ACL credit count, piece 4's instrument, cheap to carry now.

- [ ] **Step 1: Write the failing tests**

Add these four scenarios to `bt/test/l2cap_test.cpp`, immediately before the final `printf("l2cap_test: ...`. They use the existing `CapIo`, `l2()` helper and `CHECK` macro already in that file.

```cpp
    {   // A1. allowPsm(): an inbound CONN_REQ for an allowed PSM is accepted; for any other PSM it is
        //     refused with result 0x0002 (PSM not supported) -- an AVCTP (0x0017) channel from a
        //     headset (AVRCP, piece 3) must NOT consume a slot.
        CapIo io; L2cap l(io); l.begin(0x0001, 7); l.acceptIncoming(true);
        l.allowPsm(0x0019); l.allowPsm(0x0001);
        // peer CONN_REQ: code 0x02, id 0x20, len 4, psm 0x0017 (AVCTP), scid 0x0055
        std::vector<uint8_t> req = l2(0x0001, {0x02, 0x20, 4, 0, 0x17, 0x00, 0x55, 0x00});
        l.onAcl(0x0001, req.data(), (uint16_t)req.size()); l.service();
        bool sawRefuse = false;
        for (auto &t : io.tx) if (t.size() >= 9 + 12 && t[9] == 0x03) {              // CONN_RSP (12-byte L2CAP payload; cf. line ~92)
            CHECK(t[9 + 4] == 0x00 && t[9 + 5] == 0x00);                             // DCID 0 (no channel)
            CHECK(t[9 + 8] == 0x02 && t[9 + 9] == 0x00);                             // result 0x0002 PSM not supported
            sawRefuse = true;
        }
        CHECK(sawRefuse); CHECK(l.byPsm(0x0017) == nullptr);
        // an allowed PSM IS accepted
        io.tx.clear();
        std::vector<uint8_t> ok = l2(0x0001, {0x02, 0x21, 4, 0, 0x19, 0x00, 0x56, 0x00});
        l.onAcl(0x0001, ok.data(), (uint16_t)ok.size()); l.service();
        bool sawAccept = false;
        for (auto &t : io.tx) if (t.size() >= 9 + 12 && t[9] == 0x03 && t[9 + 8] == 0x00 && t[9 + 9] == 0x00) sawAccept = true;
        CHECK(sawAccept); CHECK(l.byPsm(0x0019) != nullptr);
    }
    {   // A2. With NO allow-list set (default), acceptIncoming(true) keeps today's behaviour: any PSM is
        //     accepted -- so the existing avdtp/media/[avdtp]-gate paths, which never call allowPsm, are unchanged.
        CapIo io; L2cap l(io); l.begin(0x0001, 7); l.acceptIncoming(true);
        std::vector<uint8_t> req = l2(0x0001, {0x02, 0x22, 4, 0, 0x17, 0x00, 0x57, 0x00});
        l.onAcl(0x0001, req.data(), (uint16_t)req.size()); l.service();
        CHECK(l.byPsm(0x0017) != nullptr);                                            // accepted, as before
    }
    {   // A3. reset(): every channel goes FREE and the tx queue empties, so the next attempt starts clean.
        CapIo io; L2cap l(io); l.begin(0x0001, 7);
        CHECK(l.connect(0x0019, 0x0041) != nullptr);
        l.reset();
        CHECK(l.byLocal(0x0041) == nullptr && l.byPsm(0x0019) == nullptr);
        l.service();                                                                  // nothing queued survives reset()
        CHECK(io.tx.empty());
    }
    {   // A4a. nextInbound(): iterates peer-initiated OPEN channels of a PSM, in slot order.
        //      Ample credits (20): accepting each inbound channel costs 3 signalling packets (CONN_RSP +
        //      our CFG_REQ + our CFG_RSP), so two channels need 6 -- with only 5, the second never reaches OPEN.
        CapIo io; L2cap l(io); l.begin(0x0001, 20); l.acceptIncoming(true); l.allowPsm(0x0019);
        for (uint8_t k = 0; k < 2; k++) {
            std::vector<uint8_t> rq = l2(0x0001, {0x02, (uint8_t)(0x30 + k), 4, 0, 0x19, 0x00, (uint8_t)(0x60 + k), 0x00});
            l.onAcl(0x0001, rq.data(), (uint16_t)rq.size()); l.service();
            L2cap::Channel *ch = l.byRemote((uint16_t)(0x0060 + k)); CHECK(ch);
            std::vector<uint8_t> cq = l2(0x0001, {0x04, (uint8_t)(0x40 + k), 8, 0, (uint8_t)ch->localCid, (uint8_t)(ch->localCid >> 8), 0, 0, 0x01, 0x02, 0x7F, 0x03});
            l.onAcl(0x0001, cq.data(), (uint16_t)cq.size());
            std::vector<uint8_t> cr = l2(0x0001, {0x05, (uint8_t)(0x50 + k), 6, 0, (uint8_t)ch->localCid, (uint8_t)(ch->localCid >> 8), 0, 0, 0, 0});
            l.onAcl(0x0001, cr.data(), (uint16_t)cr.size()); l.service();
            CHECK(ch->state == L2cap::OPEN);
        }
        const L2cap::Channel *a = l.nextInbound(0x0019, nullptr); CHECK(a && a->remoteCid == 0x0060);
        const L2cap::Channel *b = l.nextInbound(0x0019, a);       CHECK(b && b->remoteCid == 0x0061);
        CHECK(l.nextInbound(0x0019, b) == nullptr);
    }
    {   // A4b. creditsMin(): the running minimum credit since resetCreditsMin(); an NCP refill does NOT raise
        //      the floor.  Standalone (no channel setup to consume credits): send() queues on any cid, service()
        //      transmits while credits>0.  Start 5, send 3 (->2), NCP(+2) (->4); the floor stays at 2.
        CapIo io; L2cap l(io); l.begin(0x0001, 5);
        l.resetCreditsMin();
        for (int i = 0; i < 3; i++) { const uint8_t d[4] = {0, 1, 2, 3}; l.send(0x0040, d, 4); }
        l.service();
        CHECK(l.credits() == 2 && l.creditsMin() == 2);
        uint8_t ncp[] = { 0x01, 0x01, 0x00, 0x02, 0x00 };  l.onEvent(0x13, ncp, sizeof ncp);
        CHECK(l.credits() == 4 && l.creditsMin() == 2);
    }
```

- [ ] **Step 2: Run the tests to verify they fail to compile**

```bash
cd ~/Development/M2Radio && ./bt/test/run.sh 2>&1 | head -30
```
Expected: a COMPILE error — `allowPsm`, `reset`, `nextInbound`, `creditsMin`, `resetCreditsMin` are not members of `L2cap`.

- [ ] **Step 3: Add the declarations to `bt/L2cap.h`**

In the `public:` section, after `void acceptIncoming(bool yes) { m_accept = yes; }` (line ~43), add:

```cpp
    // Restrict which PSMs a peer-initiated CONN_REQ may open (in addition to acceptIncoming()).  Up to
    // two.  With NONE added, acceptIncoming() accepts any PSM (today's behaviour, unchanged).  With any
    // added, a CONN_REQ for a PSM not in the list is refused with result 0x0002 (PSM not supported) --
    // an AVCTP (0x0017/AVRCP, piece 3) channel from a headset lands here rather than consuming a slot.
    void allowPsm(uint16_t psm) { if (m_nAllow < 2) m_allow[m_nAllow++] = psm; }
    // Drop every channel and empty the tx queue -- used between reconnect attempts (== begin(0,0)).
    void reset() { begin(0, 0, m_aclMax); m_nAllow = 0; }
    // Iterate peer-initiated OPEN channels of a PSM in slot order: nextInbound(psm, nullptr) returns the
    // first, nextInbound(psm, prev) the next, nullptr at the end.  The AVDTP acceptor uses it to find the
    // signalling channel the headset opened (first) and, later, the media channel (next).
    const Channel *nextInbound(uint16_t psm, const Channel *after) const;
    // Running minimum ACL credit seen since resetCreditsMin() (piece 4's air-link-starvation floor).
    uint8_t  creditsMin() const { return m_creditsMin; }
    void     resetCreditsMin() { m_creditsMin = m_credits; }
```

In the `private:` section (after the `bool m_accept;` group, near line ~66), add:

```cpp
    uint16_t m_allow[2] = {0, 0}; uint8_t m_nAllow = 0;
    uint8_t  m_creditsMin = 0;
```

- [ ] **Step 4: Implement in `bt/L2cap.cpp`**

In `begin()` (line ~5-7) add credit-floor init at the end of the function body:

```cpp
    m_creditsMin = credits;
```
(Leave `m_allow`/`m_nAllow` to `reset()` and their in-class initialisers; `begin()` must NOT clear the allow-list, because `A2dpSource` calls `allowPsm()` after `begin()` — see Task 5. `reset()` clears it because a reset tears the whole attempt down.)

In `onEvent()` (the 0x13 credit path, line ~27-34), after the line that raises `m_credits`, add a floor update. Replace:
```cpp
        if (h == m_handle) { uint32_t v = (uint32_t)m_credits + c; m_credits = v > m_maxCredits ? m_maxCredits : (uint8_t)v; }
```
with:
```cpp
        if (h == m_handle) { uint32_t v = (uint32_t)m_credits + c; m_credits = v > m_maxCredits ? m_maxCredits : (uint8_t)v; }
        if (m_credits < m_creditsMin) m_creditsMin = m_credits;   // (NCP only raises credits, but keep the guard uniform)
```

In the credit-paced write loop in `service()` (line ~119-125), after `m_credits--;`, add:
```cpp
        if (m_credits < m_creditsMin) m_creditsMin = m_credits;
```

In the inbound-accept block of `service()` (the `if (m_p.connReq)` block, line ~85-97) apply the allow-list. Replace the channel-selection line:
```cpp
            if (m_accept) for (auto &c : m_ch) if (c.state == FREE || c.state == CLOSED) { ch = &c; break; }
```
with:
```cpp
            bool psmOk = (m_nAllow == 0);
            for (uint8_t i = 0; i < m_nAllow; i++) if (m_allow[i] == m_p.connPsm) psmOk = true;
            if (m_accept && psmOk) for (auto &c : m_ch) if (c.state == FREE || c.state == CLOSED) { ch = &c; break; }
```
and change the result line so a disallowed PSM is refused with 0x0002 rather than 0x0004:
```cpp
            m_p.connRspRes = ch ? 0x0000 : (psmOk ? 0x0004 : 0x0002); m_p.connRspLocal = 0;   // 0x0004 no resources, 0x0002 PSM not supported
```

Add `nextInbound()` after `byPsm()` (near line ~40):
```cpp
const L2cap::Channel *L2cap::nextInbound(uint16_t psm, const Channel *after) const {
    bool seen = (after == nullptr);
    for (const auto &ch : m_ch) {
        if (ch.state != OPEN || !ch.peerInitiated || ch.psm != psm) continue;
        if (seen) return &ch;
        if (&ch == after) seen = true;
    }
    return nullptr;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cd ~/Development/M2Radio && ./bt/test/run.sh 2>&1 | tail -20
```
Expected: `l2cap_test: N checks, 0 failures` (N grew), and `BT-HOST-TESTS: PASS`.

- [ ] **Step 6: Mutation-check the new pins**

For each mutation: apply it to a SCRATCH COPY, compile+run just l2cap_test, confirm the named failure, discard the copy.
```bash
cd ~/Development/M2Radio && D=/private/tmp/claude-501/*/*/scratchpad/l2mut && rm -rf $D && mkdir -p $D && cp -r bt hci $D/ && cd $D
# mutant 1: allow-list ignored (accept any PSM) -> A1 refuse arm fails
sed -i.bak 's/bool psmOk = (m_nAllow == 0);/bool psmOk = true;/' bt/L2cap.cpp
c++ -std=c++11 -Wall -Wextra -Werror -Ibt -Ihci bt/test/l2cap_test.cpp bt/*.cpp hci/H4Parser.cpp hci/Hci.cpp hci/HciEvents.cpp -o /tmp/l2m && /tmp/l2m; echo "exit=$?"
```
Expected: `FAIL ... A1` (or the refuse-arm CHECK) and `exit=1`. Restore (`mv bt/L2cap.cpp.bak bt/L2cap.cpp`) and repeat for: `nextInbound` returning `nullptr` always (A4a iterator fails); `m_creditsMin` never lowered (A4b floor fails). Then `rm -rf $D`.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/M2Radio && git commit -m "bt: L2cap inbound PSM allow-list, reset(), nextInbound(), credit floor (NEW-34 piece 2)

allowPsm() refuses a non-allowed peer CONN_REQ with result 0x0002 (an AVCTP/AVRCP channel no longer consumes a slot); reset() drops all channels between reconnect attempts; nextInbound() lets the AVDTP acceptor find the channels the headset opened; creditsMin() is piece 4's air-link-starvation floor. With no allowPsm() call the behaviour is unchanged, so every existing path is untouched.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>" -- bt/L2cap.h bt/L2cap.cpp bt/test/l2cap_test.cpp && git log --oneline -1
```

---

## Task 2: MediaPacketizer variable frame length + Avdtp acceptor

**Files:**
- Modify: `~/Development/M2Radio/bt/MediaPacketizer.h`
- Modify: `~/Development/M2Radio/bt/MediaPacketizer.cpp`
- Modify: `~/Development/M2Radio/bt/Avdtp.h`
- Modify: `~/Development/M2Radio/bt/Avdtp.cpp`
- Test: `~/Development/M2Radio/bt/test/avdtp_test.cpp`

**Context:** Read `bt/Avdtp.h` and `bt/Avdtp.cpp` in full. Today `Avdtp` is an initiator (DISCOVER→START) plus a tiny acceptor for a peer DISCOVER (answered with one audio-SRC SEP, SEID 1) and a peer DelayReport (ACCEPT); everything else gets a General Reject (see `onSignalling()`/`service()`). Piece 2 needs a full acceptor for the headset-driven path: GET_(ALL_)CAPABILITIES, SET_CONFIGURATION (validated, config adopted), OPEN (+ media-channel adoption), START, SUSPEND, CLOSE/ABORT, RECONFIGURE, GET_CONFIGURATION, plus the initiator/acceptor collision rule. Because an accepted config may use a bitpool other than 53, the media frame is no longer always 119 bytes, so `MediaPacketizer` must batch by the real frame size.

### Part A — MediaPacketizer frame length

- [ ] **Step 1: Write the failing test**

Add to `bt/test/mediapacketizer_test.cpp` before its final `printf`. (Read the file's top for its `CHECK`/scaffolding; it constructs a `MediaPacketizer`, calls `begin`, `push`es frames and drains through a capturing `SendFn`.)

```cpp
    {   // Frame length is a begin() parameter: an 83-byte frame (bitpool 35, joint stereo) batches
        //   floor((mtu-13)/83) per packet, not floor((mtu-13)/119).
        MediaPacketizer pk; pk.begin(/*mtu*/ 400, /*frameBytes*/ 83);
        CHECK(pk.framesPerPacket() == (400 - Rtp::HEADER_LEN) / 83);   // == 4, not 3
        uint8_t f[83]; memset(f, 0x9C, sizeof f);
        for (int i = 0; i < 4; i++) pk.push(f, 83);
        struct Cap { static bool s(void *c, const uint8_t *p, uint16_t n){ auto *v=(std::vector<uint16_t>*)c; v->push_back(n); return true; } };
        std::vector<uint16_t> lens; pk.drain(Cap::s, &lens);
        CHECK(lens.size() == 1 && lens[0] == Rtp::HEADER_LEN + 4 * 83);
    }
```

- [ ] **Step 2: Verify it fails to compile** — `./bt/test/run.sh` reports `begin(...)` takes 1 argument, not 2.

- [ ] **Step 3: Implement.** In `bt/MediaPacketizer.h`, change the `begin` declaration to `void begin(uint16_t mtu, uint16_t frameBytes = FRAME_BYTES);` and add `uint16_t m_frameBytes = FRAME_BYTES;` to the private members. In `bt/MediaPacketizer.cpp`, change `begin` to store and use the parameter:
```cpp
void MediaPacketizer::begin(uint16_t mtu, uint16_t frameBytes) {
    m_mtu = mtu; m_frameBytes = frameBytes ? frameBytes : FRAME_BYTES;
    uint16_t avail = mtu > Rtp::HEADER_LEN ? mtu - Rtp::HEADER_LEN : 0;
    m_perPkt = avail / m_frameBytes;
    if (m_perPkt == 0) m_perPkt = 1;
    if (m_perPkt > 8)  m_perPkt = 8;
    m_wr = m_rd = 0; m_seq = 0; m_ts = 0;
}
```
The default argument keeps every existing `begin(mtu)` caller (bitpool 53 ⇒ 119) byte-identical.

- [ ] **Step 4: Verify pass** — `./bt/test/run.sh` → `mediapacketizer_test: N checks, 0 failures`.

### Part B — Avdtp acceptor

- [ ] **Step 5: Write the failing tests**

Add these to `bt/test/avdtp_test.cpp` before the final `printf`. They reuse the file's `CapIo`, `feed`, `drain`, `eq`, `onData`, `tick` helpers. Add one acceptor bring-up helper at the top of the file (after `openSignalling`):

```cpp
// The peer OPENS signalling at us: bring an inbound AVDTP channel (peerInitiated) to OPEN, then hand the
// Avdtp its inbound signalling channel so it acts as ACCEPTOR.  Returns the peer-assigned remote CID.
static uint16_t openInboundSignalling(CapIo &io, L2cap &l, Avdtp &a) {
    l.begin(0x0001, 100); l.acceptIncoming(true); l.allowPsm(Avdtp::PSM); l.onData(onData, &a);
    // peer CONN_REQ psm 0x0019 scid 0x00C0 -> we accept (our CID assigned by L2cap)
    feed(l, 0x0001, { 0x02, 0x20, 4, 0, 0x19, 0x00, 0xC0, 0x00 }); l.service();
    const L2cap::Channel *ch = l.byRemote(0x00C0); CHECK(ch);
    uint16_t our = ch->localCid;
    feed(l, 0x0001, { 0x04, 0x21, 8, 0, (uint8_t)our, (uint8_t)(our >> 8), 0, 0, 0x01, 0x02, 0x7F, 0x03 });   // peer CFG_REQ
    feed(l, 0x0001, { 0x05, 0x22, 6, 0, (uint8_t)our, (uint8_t)(our >> 8), 0, 0, 0, 0 });                     // peer CFG_RSP
    l.service(); CHECK(l.byRemote(0x00C0)->state == L2cap::OPEN);
    a.begin(l, 0, 0);                 // no initiator CIDs; acceptor discovers its channel from L2cap
    a.adoptInbound(l); drain(io);
    return 0x00C0;
}
```

```cpp
    {   // B1. ACCEPTOR: the peer drives DISCOVER/GET_ALL_CAPABILITIES/SET_CONFIGURATION/OPEN/START and we
        //     answer from our source SEP; the adopted config is what sbcConfig() reports.
        CapIo io; L2cap l(io); Avdtp a; openInboundSignalling(io, l, a);
        CHECK(a.role() == Avdtp::ACCEPTOR);
        // DISCOVER (peer tl 3) -> our one audio-SOURCE SEP, SEID 1
        a.onSignalling(std::vector<uint8_t>{ 0x30, 0x01 }.data(), 2); tick(l, a); auto o = drain(io);
        CHECK(o.size() == 1 && eq(o[0], { 0x32, 0x01, 1 << 2, 0x00 }));                 // ACCEPT: SEID 1, audio, SRC (in-use bit 0)
        // GET_ALL_CAPABILITIES SEID 1 -> media transport + SBC caps (all modes, blocks 4..16, sub 4/8, bitpool 2..53) + delay reporting
        a.onSignalling(std::vector<uint8_t>{ 0x40, 0x0C, 1 << 2 }.data(), 3); tick(l, a); o = drain(io);
        CHECK(o.size() == 1 && o[0][0] == 0x42 && o[0][1] == 0x0C);
        CHECK(o[0][4] == 0x01 && o[0][5] == 0x00 && o[0][6] == 0x07 && o[0][7] == 0x06 && o[0][8] == 0x00 && o[0][9] == 0x00);
        CHECK(o[0][10] == 0xFF && o[0][11] == 0xFF && o[0][12] == 0x02 && o[0][13] == 0x35);   // rates/modes/blocks/sub/alloc all, bitpool 2..53
        // SET_CONFIGURATION at bitpool 35 (cie 21 15 02 23), acp seid 1, int seid 5 -> ACCEPT, config adopted
        a.onSignalling(std::vector<uint8_t>{ 0x50, 0x03, 1 << 2, 5 << 2, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x21, 0x15, 0x02, 0x23 }.data(), 14);
        tick(l, a); o = drain(io);
        CHECK(o.size() == 1 && eq(o[0], { 0x52, 0x03 }));                               // bare ACCEPT
        CHECK(a.sbcConfig().maxBitpool == 35 && a.sbcConfig().mode == Avdtp::JOINT_STEREO);
        CHECK(a.configChanged());                                                       // consumed by the app once
        // OPEN -> ACCEPT
        a.onSignalling(std::vector<uint8_t>{ 0x60, 0x06, 1 << 2 }.data(), 3); tick(l, a); o = drain(io);
        CHECK(o.size() == 1 && eq(o[0], { 0x62, 0x06 }));
        // the peer opens the media channel; adoptInbound picks it up
        feed(l, 0x0001, { 0x02, 0x23, 4, 0, 0x19, 0x00, 0xC1, 0x00 }); l.service();
        const L2cap::Channel *m = l.byRemote(0x00C1);
        feed(l, 0x0001, { 0x04, 0x24, 8, 0, (uint8_t)m->localCid, (uint8_t)(m->localCid >> 8), 0, 0, 0x01, 0x02, 0x7F, 0x03 });
        feed(l, 0x0001, { 0x05, 0x25, 6, 0, (uint8_t)m->localCid, (uint8_t)(m->localCid >> 8), 0, 0, 0, 0 });
        l.service(); a.adoptInbound(l); tick(l, a); drain(io);
        CHECK(a.mediaRemoteCid() == 0x00C1);
        // START -> ACCEPT -> STREAMING
        a.onSignalling(std::vector<uint8_t>{ 0x70, 0x07, 1 << 2 }.data(), 3); tick(l, a); o = drain(io);
        CHECK(o.size() == 1 && eq(o[0], { 0x72, 0x07 }));
        CHECK(a.state() == Avdtp::STREAMING);
    }
    {   // B2. SET_CONFIGURATION at a config we cannot serve (48 kHz) is REJECTED with the media-codec category
        //     byte and error 0x29 (unsupported configuration).  48k = cie byte0 0x10 | mode.
        CapIo io; L2cap l(io); Avdtp a; openInboundSignalling(io, l, a);
        a.onSignalling(std::vector<uint8_t>{ 0x30, 0x03, 1 << 2, 5 << 2, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x11, 0x15, 0x02, 0x35 }.data(), 14);
        tick(l, a); auto o = drain(io);
        CHECK(o.size() == 1 && o[0][0] == 0x33 && o[0][1] == 0x03 && o[0][2] == 0x07 && o[0][3] == 0x29);  // REJECT, category 0x07, 0x29
        CHECK(a.state() != Avdtp::STREAMING);
    }
    {   // B3. GET_CAPABILITIES / SET_CONFIGURATION for a SEID that is not ours (2) -> REJECT BAD_ACP_SEID 0x12.
        CapIo io; L2cap l(io); Avdtp a; openInboundSignalling(io, l, a);
        a.onSignalling(std::vector<uint8_t>{ 0x30, 0x0C, 2 << 2 }.data(), 3); tick(l, a); auto o = drain(io);
        CHECK(o.size() == 1 && o[0][0] == 0x33 && o[0][1] == 0x0C && o[0][2] == 0x12);
    }
    {   // B4. Collision: our initiator has already SENT SET_CONFIGURATION (CONFIGURING) when the peer sends its
        //     own -> REJECT BAD_STATE 0x31, and our initiator is untouched.
        CapIo io; L2cap l(io); Avdtp a; openSignalling(io, l, a);        // initiator path (from the existing helper)
        tick(l, a); drain(io);                                          // DISCOVER
        a.onSignalling(std::vector<uint8_t>{ 0x12, 0x01, 1 << 2, 0x08 }.data(), 4); tick(l, a); drain(io);   // caps
        static const uint8_t caps[12] = { 0x22, 0x0C, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0xFF, 0xFF, 0x02, 0x35 };
        a.onSignalling(caps, 12); tick(l, a); drain(io);                // we send SET_CONFIGURATION -> CONFIGURING
        CHECK(a.state() == Avdtp::CONFIGURING);
        a.onSignalling(std::vector<uint8_t>{ 0x50, 0x03, 1 << 2, 5 << 2, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x21, 0x15, 0x02, 0x35 }.data(), 14);
        tick(l, a); auto o = drain(io);
        CHECK(o.size() == 1 && o[0][0] == 0x53 && o[0][1] == 0x03 && o[0][3] == 0x31);   // REJECT BAD_STATE
        CHECK(a.state() == Avdtp::CONFIGURING);                          // initiator unharmed
    }
    {   // B5. SUSPEND pauses (started() false, state SUSPENDED), a later START resumes to STREAMING.
        CapIo io; L2cap l(io); Avdtp a; openInboundSignalling(io, l, a);
        // fast-path to STREAMING via the acceptor (reuse B1's sequence up to START); helper below:
        acceptorToStreaming(io, l, a);
        a.onSignalling(std::vector<uint8_t>{ 0x30, 0x09, 1 << 2 }.data(), 3); tick(l, a); auto o = drain(io);
        CHECK(o.size() == 1 && eq(o[0], { 0x32, 0x09 }) && a.state() == Avdtp::SUSPENDED && !a.started());
        a.onSignalling(std::vector<uint8_t>{ 0x40, 0x07, 1 << 2 }.data(), 3); tick(l, a); o = drain(io);
        CHECK(o.size() == 1 && eq(o[0], { 0x42, 0x07 }) && a.state() == Avdtp::STREAMING && a.started());
    }
    {   // B6. CLOSE returns the SEP to idle so the attempt can re-open (state back to a non-streaming, non-configured state).
        CapIo io; L2cap l(io); Avdtp a; openInboundSignalling(io, l, a); acceptorToStreaming(io, l, a);
        a.onSignalling(std::vector<uint8_t>{ 0x30, 0x08, 1 << 2 }.data(), 3); tick(l, a); auto o = drain(io);
        CHECK(o.size() == 1 && eq(o[0], { 0x32, 0x08 }));
        CHECK(a.state() != Avdtp::STREAMING && !a.started() && a.mediaRemoteCid() == 0);
    }
```

Add the `acceptorToStreaming` helper near `openInboundSignalling` (it replays B1's DISCOVER…START so B5/B6 do not repeat it):
```cpp
static void acceptorToStreaming(CapIo &io, L2cap &l, Avdtp &a) {
    a.onSignalling(std::vector<uint8_t>{ 0x30, 0x01 }.data(), 2); tick(l, a); drain(io);
    a.onSignalling(std::vector<uint8_t>{ 0x40, 0x0C, 1 << 2 }.data(), 3); tick(l, a); drain(io);
    a.onSignalling(std::vector<uint8_t>{ 0x50, 0x03, 1 << 2, 5 << 2, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x21, 0x15, 0x02, 0x35 }.data(), 14); tick(l, a); drain(io);
    a.onSignalling(std::vector<uint8_t>{ 0x60, 0x06, 1 << 2 }.data(), 3); tick(l, a); drain(io);
    feed(l, 0x0001, { 0x02, 0x23, 4, 0, 0x19, 0x00, 0xC1, 0x00 }); l.service();
    const L2cap::Channel *m = l.byRemote(0x00C1);
    feed(l, 0x0001, { 0x04, 0x24, 8, 0, (uint8_t)m->localCid, (uint8_t)(m->localCid >> 8), 0, 0, 0x01, 0x02, 0x7F, 0x03 });
    feed(l, 0x0001, { 0x05, 0x25, 6, 0, (uint8_t)m->localCid, (uint8_t)(m->localCid >> 8), 0, 0, 0, 0 });
    l.service(); a.adoptInbound(l); tick(l, a); drain(io);
    a.onSignalling(std::vector<uint8_t>{ 0x70, 0x07, 1 << 2 }.data(), 3); tick(l, a); drain(io);
}
```

- [ ] **Step 6: Verify the tests fail to compile** (`role`, `adoptInbound`, `sbcConfig`, `configChanged`, `SUSPENDED`, acceptor states are not members).

- [ ] **Step 7: Extend `bt/Avdtp.h`**

Add to the `enum State` (before `FAILED`): `SUSPENDED`. Add a role enum and the acceptor API to the public section:
```cpp
    enum Role : uint8_t { RNONE, INITIATOR, ACCEPTOR };
    Role role() const { return m_role; }
    void reset();                                        // back to IDLE, RNONE, media released
    void adoptInbound(L2cap &l);                         // find our peer-initiated signalling channel (and later media) from L2cap
    bool started() const { return m_state == STREAMING; }
    const SbcConfig &sbcConfig() const { return m_acceptCfg; }   // the config the peer SET (acceptor); valid once configChanged() fired
    bool configChanged() { bool c = m_cfgChanged; m_cfgChanged = false; return c; }   // one-shot: the app restarts the encoder on true
```
Add to the private members:
```cpp
    Role m_role = RNONE;
    SbcConfig m_acceptCfg = { 44100, JOINT_STEREO, 16, 8, LOUDNESS, 2, 53 };
    bool m_cfgChanged = false;
    // peer-command recording for the FULL acceptor (beyond m_peerDiscover/m_peerDelayRpt/m_peerReject):
    bool m_peerCaps = false;    uint8_t m_peerCapsHdr = 0, m_peerCapsSeid = 0, m_peerCapsSig = 0;
    bool m_peerSetCfg = false;  uint8_t m_peerSetHdr = 0; uint8_t m_peerSetPl[20]; uint16_t m_peerSetLen = 0;
    bool m_peerOpen = false;    uint8_t m_peerOpenHdr = 0, m_peerOpenSeid = 0;
    bool m_peerStart = false;   uint8_t m_peerStartHdr = 0;
    bool m_peerSuspend = false; uint8_t m_peerSuspendHdr = 0;
    bool m_peerClose = false;   uint8_t m_peerCloseHdr = 0, m_peerCloseSig = 0;
    static const uint8_t OUR_SEID = 1;
```

- [ ] **Step 8: Implement the acceptor in `bt/Avdtp.cpp`.**

Add `reset()`:
```cpp
void Avdtp::reset() {
    m_state = IDLE; m_role = RNONE; m_media = nullptr; m_rspSeen = false; m_cfgChanged = false;
    m_peerDiscover = m_peerDelayRpt = m_peerReject = false;
    m_peerCaps = m_peerSetCfg = m_peerOpen = m_peerStart = m_peerSuspend = m_peerClose = false;
    m_nCand = 0; m_candIdx = 0; m_acp = 0;
}
```

Add `adoptInbound()` — find the peer-initiated signalling channel, and once OPENING, the media channel:
```cpp
void Avdtp::adoptInbound(L2cap &l) {
    if (!m_sig) {                                              // not yet acting as acceptor: adopt the first inbound AVDTP channel as signalling
        const L2cap::Channel *c = l.nextInbound(PSM, nullptr);
        if (c) { m_l2 = &l; m_sig = const_cast<L2cap::Channel *>(c); m_sigCid = c->localCid; m_role = ACCEPTOR; m_state = IDLE; }
        return;
    }
    if (m_role == ACCEPTOR && m_state == OPENING && !m_media) {  // the peer's SECOND AVDTP channel is media
        const L2cap::Channel *c = l.nextInbound(PSM, m_sig);
        if (c) { m_media = const_cast<L2cap::Channel *>(c); m_mediaCid = c->localCid; }
    }
}
```
Note: `m_sig` must not be `const` (it is already a `Channel *`). Because the acceptor's `send()` uses `m_sig->remoteCid`, adopting the peer channel makes `send()` work unchanged.

In `onSignalling()`, extend the COMMAND branch (currently records only 0x01 DISCOVER, 0x0D DelayReport, else General-Reject). Replace the `else { m_peerReject = ... }` arm so the full set is recorded:
```cpp
        else if (sig == 0x02 || sig == 0x0C) { m_peerCaps = true; m_peerCapsHdr = p[0]; m_peerCapsSig = sig; m_peerCapsSeid = (len >= 3) ? (uint8_t)(p[2] >> 2) : 0; }
        else if (sig == 0x03) { m_peerSetCfg = true; m_peerSetHdr = p[0]; m_peerSetLen = len > sizeof m_peerSetPl ? (uint16_t)sizeof m_peerSetPl : len; memcpy(m_peerSetPl, p, m_peerSetLen); }
        else if (sig == 0x06) { m_peerOpen = true; m_peerOpenHdr = p[0]; m_peerOpenSeid = (len >= 3) ? (uint8_t)(p[2] >> 2) : OUR_SEID; }
        else if (sig == 0x07) { m_peerStart = true; m_peerStartHdr = p[0]; }
        else if (sig == 0x09) { m_peerSuspend = true; m_peerSuspendHdr = p[0]; }
        else if (sig == 0x08 || sig == 0x0A) { m_peerClose = true; m_peerCloseHdr = p[0]; m_peerCloseSig = sig; }
        else { m_peerReject = true; m_peerRejHdr = p[0]; m_peerRejSig = sig; }
```

In `service()`, after the existing `m_peerDiscover`/`m_peerDelayRpt`/`m_peerReject` handlers and BEFORE the `m_kickoff`/initiator block, add the acceptor handlers. Each follows the "build, send, clear-on-success" pattern (so a full TXQ retries next tick). Add a private helper `buildSbcCaps` and `validateSetConfig`; full bodies:

```cpp
// GET_(ALL_)CAPABILITIES reply body for our SOURCE SEP: media transport + SBC codec (all rates/modes,
// blocks 4..16, subbands 4/8, both alloc, bitpool 2..53) + delay reporting (only for 0x0C).
uint16_t Avdtp::buildCapsAccept(uint8_t *o, uint8_t hdr, uint8_t sig) {
    o[0] = (uint8_t)((hdr & 0xF0) | ACCEPT); o[1] = sig;
    o[2] = 0x01; o[3] = 0x00;                                   // media transport
    o[4] = 0x07; o[5] = 0x06; o[6] = 0x00; o[7] = 0x00;         // media codec: audio, SBC
    o[8] = 0xFF; o[9] = 0xFF; o[10] = 0x02; o[11] = 0x35;       // rates/modes all; blocks/sub/alloc all; bitpool 2..53
    if (sig == 0x02) return 12;
    o[12] = 0x08; o[13] = 0x00;                                 // delay reporting (GET_ALL_CAPABILITIES only)
    return 14;
}
```
Add the declaration `uint16_t buildCapsAccept(uint8_t *o, uint8_t hdr, uint8_t sig);` and `bool parseAcceptCfg(const uint8_t *p, uint16_t len, SbcConfig &c, uint8_t &badCat);` to `Avdtp.h` private.

```cpp
// Validate a peer SET_CONFIGURATION against our one SEP; on success fill c.  badCat is the offending
// service-category byte for the REJECT.  Requires media transport (0x01) and an in-range SBC codec (0x07).
bool Avdtp::parseAcceptCfg(const uint8_t *p, uint16_t len, SbcConfig &c, uint8_t &badCat) {
    bool haveTransport = false, haveCodec = false; badCat = 0;
    for (uint16_t i = 4; i + 1 < len; ) {
        uint8_t cat = p[i], l = p[i + 1];
        if (cat == 0x01) haveTransport = true;
        else if (cat == 0x07 && l >= 6 && i + 2 + 6 <= len && p[i + 2] == 0x00 && p[i + 3] == 0x00) {
            const uint8_t *e = p + i + 4;
            uint8_t rateBit = (uint8_t)(e[0] >> 4), modeBits = (uint8_t)(e[0] & 0x0F);
            uint8_t blkBits = (uint8_t)(e[1] >> 4), subBit = (uint8_t)((e[1] >> 2) & 0x03), allocBit = (uint8_t)(e[1] & 0x03);
            // exactly one bit per field, and 44.1 kHz (0x20) only -- the audio graph runs at 44.1
            auto one = [](uint8_t b){ return b && !(b & (b - 1)); };
            if (rateBit != 0x20 || !one(modeBits) || !one(blkBits) || !one(subBit) || !one(allocBit)) { badCat = 0x07; return false; }
            c.rate = 44100;
            c.mode = (Mode)modeBits; c.alloc = (Alloc)allocBit;
            c.blocks = blkBits == 0x80 ? 4 : blkBits == 0x40 ? 8 : blkBits == 0x20 ? 12 : 16;
            c.subbands = subBit == 0x08 ? 4 : 8;
            c.minBitpool = e[2]; c.maxBitpool = e[3];
            if (c.maxBitpool < 2 || c.maxBitpool > 53) { badCat = 0x07; return false; }
            haveCodec = true;
        }
        if ((uint32_t)i + 2 + l >= len) break;
        i = (uint16_t)(i + 2 + l);
    }
    if (!haveTransport) { badCat = 0x01; return false; }
    if (!haveCodec)     { badCat = 0x07; return false; }
    return true;
}
```

The acceptor service block (insert into `service()` after the existing three peer handlers):
```cpp
    if (m_peerCaps) { uint8_t b[16];
        if (m_peerCapsSeid != OUR_SEID) { uint8_t r[3] = { (uint8_t)((m_peerCapsHdr & 0xF0) | REJECT), m_peerCapsSig, 0x12 }; if (send(r, 3)) m_peerCaps = false; }
        else { uint16_t n = buildCapsAccept(b, m_peerCapsHdr, m_peerCapsSig); if (send(b, n)) m_peerCaps = false; } }
    if (m_peerSetCfg) {
        uint8_t acp = m_peerSetLen >= 3 ? (uint8_t)(m_peerSetPl[2] >> 2) : 0, badCat = 0;
        if (m_role == INITIATOR && m_state >= CONFIGURING && m_state != FAILED) {           // collision: we already sent ours
            uint8_t r[4] = { (uint8_t)((m_peerSetHdr & 0xF0) | REJECT), 0x03, 0x00, 0x31 }; if (send(r, 4)) m_peerSetCfg = false;
        } else if (acp != OUR_SEID) {
            uint8_t r[4] = { (uint8_t)((m_peerSetHdr & 0xF0) | REJECT), 0x03, 0x00, 0x12 }; if (send(r, 4)) m_peerSetCfg = false;
        } else { SbcConfig c;
            if (parseAcceptCfg(m_peerSetPl, m_peerSetLen, c, badCat)) {
                uint8_t r[2] = { (uint8_t)((m_peerSetHdr & 0xF0) | ACCEPT), 0x03 };
                if (send(r, 2)) { m_peerSetCfg = false; m_acceptCfg = c; m_cfgChanged = true; m_role = ACCEPTOR; m_state = CONFIGURING; }
            } else {
                uint8_t r[4] = { (uint8_t)((m_peerSetHdr & 0xF0) | REJECT), 0x03, badCat, 0x29 }; if (send(r, 4)) m_peerSetCfg = false;
            } } }
    if (m_peerOpen) { uint8_t r[2] = { (uint8_t)((m_peerOpenHdr & 0xF0) | ACCEPT), 0x06 };
        if (send(r, 2)) { m_peerOpen = false; m_role = ACCEPTOR; m_state = OPENING; } }        // adoptInbound() will pick up the media channel
    if (m_peerStart) { uint8_t r[2] = { (uint8_t)((m_peerStartHdr & 0xF0) | ACCEPT), 0x07 };
        if (send(r, 2)) { m_peerStart = false; m_state = STREAMING; } }
    if (m_peerSuspend) { uint8_t r[2] = { (uint8_t)((m_peerSuspendHdr & 0xF0) | ACCEPT), 0x09 };
        if (send(r, 2)) { m_peerSuspend = false; m_state = SUSPENDED; } }
    if (m_peerClose) { uint8_t r[2] = { (uint8_t)((m_peerCloseHdr & 0xF0) | ACCEPT), m_peerCloseSig };
        if (send(r, 2)) { m_peerClose = false; m_media = nullptr; m_mediaCid = 0; m_state = IDLE; } }
```

Also change the existing peer-DISCOVER responder so it reflects the SEP as SOURCE (it already answers SEID 1, audio, SRC via `buildDiscoverAcceptOneSource` — no change needed) and, importantly, guard `m_kickoff`/initiator advancement so the acceptor path does not run the initiator: the initiator block already keys on `m_state` values it only enters via `start()`, so acceptor states (STREAMING/SUSPENDED/OPENING as ACCEPTOR) fall through the initiator `switch` default. Verify by inspection that `m_role == ACCEPTOR` never satisfies `m_kickoff` (only `start()` sets `m_kickoff`).

Add the self-START for the acceptor (either side may START; the peer usually does, but if the media channel is OPEN and no START has arrived we send one — same rule as the initiator's MEDIA_CONNECTING). Add to `service()` after the acceptor block:
```cpp
    if (m_role == ACCEPTOR && m_state == OPENING && m_media && m_media->state == L2cap::OPEN) {
        // give the peer START_WAIT first; the attempt layer (A2dpSource) owns that timer and calls startSelf()
    }
```
(The self-START timer lives in `A2dpSource` — Task 5 — because `Avdtp` has no clock. `Avdtp` exposes `bool mediaReady() const { return m_role == ACCEPTOR && m_state == OPENING && m_media && m_media->state == L2cap::OPEN; }` and `void startSelf()` which sends START with a fresh tl and moves to STARTING; add both. In STARTING the existing `case STARTING: m_state = STREAMING;` handles the ACCEPT.)

- [ ] **Step 9: Verify pass** — `./bt/test/run.sh` → `avdtp_test: N checks, 0 failures`, `BT-HOST-TESTS: PASS`. If B5/B6's `acceptorToStreaming` needs `startSelf` wiring, resolve by having the peer send START (as the tests do) rather than self-START (self-START is exercised in Task 5's a2dpsource_test).

- [ ] **Step 10: Mutation-check** (scratch copy): (a) drop the `acp != OUR_SEID` guard → B3 fails; (b) accept any bitpool (remove the `>53` check) → add a bitpool-99 arm if not caught, else confirm B2 covers rate; (c) drop the collision guard → B4 fails; (d) `m_cfgChanged` never set → B1 `configChanged()` fails.

- [ ] **Step 11: Commit**

```bash
cd ~/Development/M2Radio && git commit -m "bt: Avdtp acceptor role + MediaPacketizer variable frame length (NEW-34 piece 2)

Avdtp answers a peer-driven session (DISCOVER/GET_(ALL_)CAPABILITIES/SET_CONFIGURATION with validation/OPEN/START/SUSPEND/CLOSE/ABORT) beside the initiator, adopts the peer's signalling+media channels via L2cap::nextInbound, and adopts the peer's chosen SBC config (sbcConfig()/configChanged()). Initiator-vs-acceptor SET_CONFIGURATION collision -> BAD_STATE. MediaPacketizer::begin(mtu, frameBytes) batches by the negotiated frame size (an adopted bitpool != 53 is not 119 bytes); the default keeps every existing caller byte-identical.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>" -- bt/Avdtp.h bt/Avdtp.cpp bt/MediaPacketizer.h bt/MediaPacketizer.cpp bt/test/avdtp_test.cpp bt/test/mediapacketizer_test.cpp && git log --oneline -1
```

---

## Task 3: BtLink — the non-blocking operation engine (PREPARE/INQUIRY/PAGE/PAIR/DISCONNECT)

**Files:**
- Modify: `~/Development/M2Radio/bt/BtLink.h`
- Modify: `~/Development/M2Radio/bt/BtLink.cpp`
- Test: `~/Development/M2Radio/bt/test/btlink_test.cpp`

**Context — read first, in this order:** `bt/BtLink.h`, `bt/BtLink.cpp` (all of `connect`, `page`, `pairAndEncrypt`, `disconnect`, `onEvent`), and `bt/test/btlink_test.cpp` (all 16 scenarios). The `Hci` API is `submit(opcode, params, plen, DoneFn, ctx)` (async, `DoneFn(ctx, Error, const Reply*)`) and `run(...)` (blocking, kept for boot only). Read `hci/Hci.h`'s `Error`, `Reply`, `submit`, `busy`.

This task converts every blocking wait in `page()`, `pairAndEncrypt()`, `disconnect()` and the setup portion of `connect()` into a state advanced by `tick(now)`, WITHOUT changing what goes on the wire or the log lines. `onEvent()` is unchanged in this task except that its handlers already set the `m_*Done`/`m_*Status` flags the new states poll — Task 4 adds the incoming-page/address-check work. The result codes and log strings are preserved exactly (piece 1's gate greps them).

**The model.** BtLink runs at most one *operation* at a time. Each operation is a small state machine. `tick(now)`:
1. If a command this op issued via `submit` is outstanding, wait (the done-callback clears `m_cmdBusy` and stores `m_cmdErr`/`m_cmdReply`).
2. Otherwise advance the current op's sub-state, issuing the next command or checking the next event flag against `m_deadline`.
3. The page-scan side channel (Task 4 wires the value; the mechanism is here) issues Write_Scan_Enable whenever `m_wantScan != m_haveScan` and no command is outstanding.

**Command issue helper** (add to `BtLink.cpp`, file-static-free — it is a member): commands whose Command Status/Complete BtLink must inspect (Create_Connection, Create_Connection_Cancel, Authentication_Requested, Set_Connection_Encryption, Disconnect, Read_BD_ADDR) go through `issue()`; fire-and-forget setup commands (Set_Event_Mask, Write_SSP_Mode, Write_Page_Timeout, Write_Scan_Enable) may also go through `issue()` so PREPARE can sequence them. The event-driven waits (Connection_Complete, Authentication_Complete, Encryption_Change, Disconnection_Complete) are NOT command completions — they arrive via `onEvent` and set `m_connDone`/`m_authDone`/`m_encDone`/`m_discDone`, exactly as today.

```cpp
// Done-callback for issue(): records the reply and clears the busy flag.  submit() is fire-and-forget for
// the event-driven commands (Auth_Requested etc.), but PAGE/DISCONNECT need the Command Status, so those
// go through issue() and read m_cmdErr / m_cmdReply.
void BtLink::cmdDone(void *ctx, Hci::Error e, const Hci::Reply *r) {
    BtLink *self = (BtLink *)ctx;
    self->m_cmdErr = e; if (r) self->m_cmdReply = *r; else { self->m_cmdReply.status = 0xFF; self->m_cmdReply.statusEvent = false; }
    self->m_cmdBusy = false;
}
bool BtLink::issue(uint16_t op, const uint8_t *p, uint8_t plen) {
    m_cmdBusy = true; m_cmdErr = Hci::OK;
    Hci::Error e = m_hci.submit(op, p, plen, &BtLink::cmdDone, this);
    if (e != Hci::OK) { m_cmdBusy = false; m_cmdErr = e; return false; }   // QUEUE_FULL/BUSY: retry next tick
    return true;
}
```

**Sub-state deadlines** (translate the quoted old bodies; each `while (!flag && now-t0 < T) idle()` becomes a state with `m_deadline = now + T`):

| Op | sub-states and their event deadline |
|---|---|
| PREPARE | issue Set_Event_Mask → Write_SSP_Mode(legacy?0:1) → Write_Page_Timeout(0x2000), each awaiting its Command Complete (1 s). Then `result()=OK`. |
| INQUIRY | issue Inquiry (Command Status) → wait `m_inqComplete` (15 s) → per hit: issue Remote_Name_Request → wait that hit's `named` (5 s) → choose target → `result()`=OK/`NO_INQUIRY_HIT`. |
| PAGE | per attempt: issue Create_Connection → check `m_cmdReply.statusEvent` (Command Status) → wait `m_connDone` (10 s). Then the SAME status logic as the old `page()`: 0x00 OK; 0x04 with attempts left → next attempt; else CONNECT_STATUS; no event by deadline → CANCEL sub-state (issue Create_Connection_Cancel, 1 s for a racing `m_connDone`), then next attempt or TIMEOUT. |
| PAIR | Authentication_Requested (Command Status) → wait `m_authDone` (25 s) → the piece-1 ladder EXACTLY (stored rung / 0x05/0x06 erase+refresh / SSP-off+PIN / then Set_Connection_Encryption → wait `m_encDone` 10 s). Inbound variant: first WAIT_PEER_SECURE sub-state (deadline `m_encWaitMs`, default 2000) for `m_encDone && m_encrypted`; on timeout fall into the ordinary ladder. |
| DISCONNECT | issue Disconnect(0x13) (Command Status) → wait `m_discDone` (3 s); handle cleared either way. No handle → immediate OK. |

**Every wire byte and every `logf(...)` string is copied verbatim from the old bodies** — only the control flow changes.

- [ ] **Step 1: Re-shape the test harness onto tick, then re-point every existing scenario**

At the top of `bt/test/btlink_test.cpp`, replace `idle10()` with a `runUntil` that also ticks the link, and add a global link pointer:
```cpp
static BtLink *g_link = nullptr;
static void idle10() { g_io->now += 10; g_hci->service(); if (g_link) g_link->tick(g_io->now); }
// Drive time+service+tick until pred() or a fake-time budget elapses; returns pred()'s final value.
static bool runUntil(std::function<bool()> pred, uint32_t ms) {
    uint32_t end = g_io->now + ms;
    while (g_io->now < end) { if (pred()) return true; idle10(); }
    return pred();
}
```
Every scenario that today calls `link.connect(...)`/`link.page(...)`/`link.pairAndEncrypt(...)`/`link.disconnect(...)` is converted to: set `g_link = &link;`, call `link.begin(g_io->now);`, `link.startX(...)`, then `runUntil([&]{ return !link.busy(); }, T)` with a generous `T` (e.g. 40000), then assert `link.result()`. The wire-count and log assertions are unchanged. Do this conversion for ALL 16 scenarios. Example — scenario 8 (the stored-key page) becomes:
```cpp
        g_link = &link; link.begin(io.now); link.setBonds(&bonds);
        link.startPage(BD, 1, 0, false, "OpenMove by Shokz", 1);
        CHECK(runUntil([&]{ return !link.busy(); }, 20000) && link.result() == BtLink::OK);
        // ... the io.count()/pairedBy() asserts are unchanged, but pairing is a SEPARATE op now:
        link.startPair(false);
        CHECK(runUntil([&]{ return !link.busy(); }, 40000) && link.result() == BtLink::OK);
```
(Scenarios that called `connect()` — the inquiry path — become `startInquiry(name)` then, on `result()==OK`, `startPage(link.target()...)`; add a `BtLink::target(Target&)` convenience — see Step 2 — so the test can page the chosen hit. Where a scenario asserted `connect(...) == CONNECT_STATUS`/`TIMEOUT`/`NO_INQUIRY_HIT` for the inquiry+page combination, assert the corresponding op's `result()`.)

- [ ] **Step 2: Rewrite `bt/BtLink.h`**

Replace the class body with the operation-engine API. Keep `Result`, `resultName`, `LogFn`, the ctor, `setLog/setPin/setLegacyPin/setBonds/bonds`, `PAGE_ATTEMPTS`, `onEvent`, `handle/peer/encrypted/pairedBy`, and the `Hit` struct. Add:
```cpp
    enum Op : uint8_t { NONE, PREPARE, INQUIRY, PAGE, PAIR, DISCONNECT };
    void begin(uint32_t now);                      // reset op state (call once per session; safe to re-call)
    bool startPrepare();                           // Set_Event_Mask/Write_SSP_Mode/Write_Page_Timeout, once per session
    bool startInquiry(const char *nameSubstr);     // returns false if an op is already running
    bool startPage(const uint8_t bd[6], uint8_t psrm, uint16_t clk, bool clkValid, const char *name, uint8_t attempts);
    bool startPair(bool inbound);
    bool startDisconnect();
    void tick(uint32_t now);                       // advance the current op + the page-scan side channel
    bool   busy()   const { return m_op != NONE; }
    Op     op()     const { return m_op; }
    Result result() const { return m_result; }     // the last finished op's result
    // Inquiry target for the caller's page (valid after an INQUIRY op returned OK):
    struct Target { uint8_t bd[6]; uint8_t psrm; uint16_t clk; bool clkValid; char name[249]; bool valid; };
    Target target() const;
    void wantPageScan(bool on) { m_wantScan = on; } // reconciled in tick() via Write_Scan_Enable
```
Add private members for the engine (keep every existing `m_*` used by `onEvent`):
```cpp
    Op m_op = NONE; Result m_result = OK; uint8_t m_sub = 0; uint32_t m_deadline = 0;
    // pending-command slot for issue()
    bool m_cmdBusy = false; Hci::Error m_cmdErr = Hci::OK; Hci::Reply m_cmdReply{};
    static void cmdDone(void *ctx, Hci::Error e, const Hci::Reply *r);
    bool issue(uint16_t op, const uint8_t *p, uint8_t plen);
    // op parameters captured by startX()
    uint8_t m_attempt = 0, m_attempts = 0; bool m_pairInbound = false; uint8_t m_hitIdx = 0;
    const char *m_inqFilter = nullptr;
    // page scan
    bool m_wantScan = false; bool m_haveScan = false; bool m_scanKnown = true;   // true: post-Reset scanning is OFF, so reconcileScan is a no-op until wantPageScan() creates a delta (a false here fires a spurious Write_Scan_Enable(0x00) on every op's first tick)
    uint32_t m_encWaitMs = 2000;
```
(Keep `PENDING` OUT of `Result` — `busy()` is the "still running" signal; `result()` is only read once `busy()` is false.)

- [ ] **Step 3: Rewrite `bt/BtLink.cpp`**

Keep the anonymous-namespace opcode/event enums and `resultName`/`logf` unchanged. Add `OP_WRITE_SCAN_ENABLE = 0x0C1A` to the opcode enum. Add `cmdDone`/`issue` (above). Add `begin`:
```cpp
void BtLink::begin(uint32_t now) {
    m_op = NONE; m_result = OK; m_sub = 0; m_deadline = now; m_cmdBusy = false;
    m_wantScan = false; m_haveScan = false; m_scanKnown = true;   // see the header note: NOT false (would emit an unsolicited scan-disable each op)
    // note: does NOT clear m_bd/m_handle/link state -- begin() may be re-called mid-session by the app's reset path
}
```
Add the `startX` setters (each refuses if `m_op != NONE`, captures parameters, sets `m_op`, `m_sub = 0`, and for the event-driven ops copies the piece-1 preamble state, e.g. `startPage` does the `memcpy(m_bd,...)`, `m_keyOffered=false`, `m_handle=0`, `m_pageName`, attempt clamp exactly as the old `page()` head):
```cpp
bool BtLink::startPage(const uint8_t bd[6], uint8_t psrm, uint16_t clk, bool clkValid, const char *name, uint8_t attempts) {
    if (m_op != NONE) return false;
    memcpy(m_bd, bd, 6); m_psrm = psrm; m_clk = clk; m_clkValid = clkValid;   // add m_clkValid member
    BondTable::copyName(m_pageName, name);
    m_keyOffered = false; m_handle = 0;
    m_attempts = attempts ? attempts : 1; m_attempt = 1;
    m_op = PAGE; m_sub = 0; return true;
}
```
(Add `bool m_clkValid = false;` to the header private members — the old `page()` took `clkValid` as an argument each call; the engine stores it.)

Rewrite `tick()` as the dispatcher. Full skeleton — fill each op body by translating the quoted old function line-for-line into the sub-state cases:
```cpp
void BtLink::tick(uint32_t now) {
    reconcileScan();                        // the page-scan side channel (below)
    if (m_op == NONE) return;
    if (m_cmdBusy) return;                   // wait for the outstanding command's done-callback
    switch (m_op) {
        case PREPARE:    tickPrepare(now);    break;
        case INQUIRY:    tickInquiry(now);    break;
        case PAGE:       tickPage(now);       break;
        case PAIR:       tickPair(now);       break;
        case DISCONNECT: tickDisconnect(now); break;
        default: m_op = NONE; break;
    }
}
void BtLink::finish(Result r) { m_result = r; m_op = NONE; m_sub = 0; }
void BtLink::reconcileScan() {
    if (m_cmdBusy) return;
    if (m_scanKnown && m_haveScan == m_wantScan) return;
    uint8_t s = m_wantScan ? 0x02 : 0x00;    // page scan only (not inquiry scan)
    if (issue(OP_WRITE_SCAN_ENABLE, &s, 1)) { m_haveScan = m_wantScan; m_scanKnown = true; logf("page_scan=%s", m_wantScan ? "on" : "off"); }
}
```
`tickPage(now)` — translate the old `page()` loop. `m_sub`: 0 = issue Create_Connection (and on the first attempt, this is where the PREPARE commands would have run — but PREPARE is now its own op, so `startPage` assumes PREPARE already ran; A2dpSource/BtSession call `startPrepare()` once before the first page — see Task 5). Worked body:
```cpp
void BtLink::tickPage(uint32_t now) {
    if (m_sub == 0) {                                  // issue Create_Connection for this attempt
        uint8_t p[13]; memcpy(p, m_bd, 6);
        p[6] = 0x18; p[7] = 0xCC; p[8] = m_psrm; p[9] = 0x00;
        p[10] = (uint8_t)(m_clk & 0xFF); p[11] = (uint8_t)((m_clk >> 8) | (m_clkValid ? 0x80 : 0x00)); p[12] = 0x00;
        m_connDone = false; m_connStatus = 0xFF;
        if (!issue(OP_CREATE_CONNECTION, p, sizeof p)) return;   // QUEUE_FULL: retry next tick
        m_sub = 1; return;                              // next tick: check the Command Status (in m_cmdReply)
    }
    if (m_sub == 1) {
        if (m_cmdErr != Hci::OK || !m_cmdReply.statusEvent) {
            logf("connect=fail reason=%s status=0x%02X attempt=%u", m_cmdErr == Hci::OK ? "not_command_status" : Hci::errorName(m_cmdErr), m_cmdReply.status, m_attempt);
            finish(TIMEOUT); return;
        }
        m_deadline = now + 10000; m_sub = 2; return;    // wait for Connection_Complete
    }
    if (m_sub == 2) {
        if (m_connDone) { m_sub = 4; return; }          // got it -> evaluate status
        if ((int32_t)(now - m_deadline) < 0) return;    // still waiting
        // timeout -> cancel (the old cancel-a-silent-page path)
        logf("connect=timeout (no Connection_Complete) attempt=%u ncmd=%u -> Create_Connection_Cancel", m_attempt, m_hci.ncmd());
        if (m_hci.ncmd() == 0) m_hci.reclaimCredit();
        if (!issue(OP_CREATE_CONN_CANCEL, m_bd, 6)) return;
        m_deadline = now + 1000; m_sub = 3; return;
    }
    if (m_sub == 3) {                                    // post-cancel: 1 s for a racing Connection_Complete
        if (!m_connDone && (int32_t)(now - m_deadline) < 0) return;
        logf("connect_cancel: st=%s status=0x%02X conn_complete=%s", m_cmdErr == Hci::OK ? "ok" : Hci::errorName(m_cmdErr), m_cmdReply.status, m_connDone ? "seen" : "none");
        if (m_connDone && m_connStatus == 0x00) { logf("connect=ok (raced the cancel) handle=0x%04X attempt=%u", (unsigned)m_handle, m_attempt); finish(OK); return; }
        if (m_attempt >= m_attempts) { finish(TIMEOUT); return; }
        m_attempt++; m_sub = 0; return;
    }
    if (m_sub == 4) {
        if (m_connStatus == 0x04 && m_attempt < m_attempts) { logf("connect=page_timeout attempt=%u -> retry", m_attempt); m_attempt++; m_sub = 0; return; }
        if (m_connStatus != 0x00) { logf("connect=fail status=0x%02X attempt=%u", m_connStatus, m_attempt); finish(CONNECT_STATUS); return; }
        logf("connect=ok handle=0x%04X attempt=%u", (unsigned)m_handle, m_attempt); finish(OK); return;
    }
}
```
Translate `tickPrepare`, `tickInquiry`, `tickPair`, `tickDisconnect` the same way from the quoted old `page()` head / `connect()` / `pairAndEncrypt()` / `disconnect()`. For `tickPair`, the ladder branches are numerous — keep the EXACT structure and every `logf` from the old `pairAndEncrypt()`; the only change is that each `while (!m_authDone && now-t0<25000) idle()` becomes a `m_sub` wait with `m_deadline = now + 25000`. The inbound WAIT_PEER_SECURE sub-state is a NEW sub-state 0 for `m_pairInbound`: wait `m_encDone && m_encrypted` up to `m_encWaitMs`; on success `finish(OK)` with `m_pairedBy` = "stored" if `m_keyOffered` else set a new literal "peer"; on timeout jump to the ordinary ladder's Authentication_Requested sub-state.

Add `target()`:
```cpp
BtLink::Target BtLink::target() const {
    Target t; memset(&t, 0, sizeof t); t.valid = false;
    if (m_target < 0) return t;
    const Hit &h = m_hits[m_target];
    memcpy(t.bd, h.bd, 6); t.psrm = h.psrm; t.clk = h.clk; t.clkValid = true;
    if (h.named) memcpy(t.name, h.name, strlen(h.name) + 1); t.valid = true; return t;
}
```

- [ ] **Step 4: Build + run the suite**

```bash
cd ~/Development/M2Radio && ./bt/test/run.sh 2>&1 | tail -25
```
Expected: `btlink_test: N checks, 0 failures` and `BT-HOST-TESTS: PASS`. Iterate until every converted scenario passes with the SAME wire counts and log assertions as before.

- [ ] **Step 5: Confirm the page-scan mechanism with a focused test**

Add a scenario: `link.begin(now); link.wantPageScan(true);` then `runUntil([&]{ return io.count(0x0C1A) >= 1; }, 500)`; assert the Write_Scan_Enable param is `0x02` and a `page_scan=on` log line appeared; then `link.wantPageScan(false)` → a `0x00` write and `page_scan=off`.

- [ ] **Step 6: Mutation-check** (scratch copy): (a) drop the `m_connStatus == 0x04 && m_attempt < m_attempts` retry → scenario 3 (page-timeout-then-success) fails; (b) `finish(OK)` instead of `CONNECT_STATUS` in sub 4 → scenario 7 fails; (c) reconcileScan writes 0x03 (page+inquiry scan) → the Step-5 param assert fails.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/M2Radio && git commit -m "bt: BtLink non-blocking operation engine (NEW-34 piece 2)

connect()/page()/pairAndEncrypt()/disconnect() become startInquiry/startPage/startPair/startDisconnect + tick(now); every blocking wait is now a sub-state with an absolute deadline, driven one pass per loop. Adds startPrepare() (Set_Event_Mask/SSP/Page_Timeout, once per session) and the wantPageScan() side channel (Write_Scan_Enable reconciled in tick). Every wire byte and log string is preserved; the host suite is re-shaped onto runUntil and all piece-1 pins hold.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>" -- bt/BtLink.h bt/BtLink.cpp bt/test/btlink_test.cpp && git log --oneline -1
```

---

## Task 4: BtLink — incoming page, link state, address checks, supervision knob

**Files:**
- Modify: `~/Development/M2Radio/bt/BtLink.h`
- Modify: `~/Development/M2Radio/bt/BtLink.cpp`
- Test: `~/Development/M2Radio/bt/test/btlink_test.cpp`

**Context:** With the engine in place (Task 3), add the inbound half and close piece 1's deferrals. `onEvent` gains Connection_Request (0x04), Role_Change (0x12), an address check on Connection_Complete, address-scoped key flags, and a link-state field. `Accept_Connection_Request` opcode is `0x0409`, `Reject_Connection_Request` is `0x040A`, `Write_Link_Supervision_Timeout` is `0x0C37`.

- [ ] **Step 1: Write the failing tests**

Add scenarios to `btlink_test.cpp`:
```cpp
    {   // 17. Incoming page while idle with a bond: accept (role 0x01 remain slave), link comes up as incoming.
        FakeIo io; g_io = &io; Hci hci(io); g_hci = &hci; BtLink link(hci); g_link = &link; hci.onEvent(evThunk, &link);
        link.setLog(logFn, nullptr); g_log.clear(); link.begin(io.now);
        BondTable bonds; seedBond(bonds, KEY1, "OpenMove by Shokz"); link.setBonds(&bonds);
        io.onCmd = [](FakeIo &f, uint16_t op, const std::vector<uint8_t> &prm) {
            if (op == 0x0409) { f.cc(op, withBd({ 0x00 }, prm)); f.ev(0x03, connComplete(0x00)); return; }   // Accept -> Connection_Complete
            f.cc(op, { 0x00 });
        };
        // Connection_Request: bd(6) cod(3) link_type(1 = ACL 0x01)
        std::vector<uint8_t> cr(BD, BD + 6); cr.push_back(0x04); cr.push_back(0x04); cr.push_back(0x24); cr.push_back(0x01);
        link.onEvent(0x04, cr.data(), (uint8_t)cr.size());
        CHECK(runUntil([&]{ return link.linkState() == BtLink::LINK_UP; }, 2000));
        const std::vector<uint8_t> *acc = io.last(0x0409);
        CHECK(acc && acc->size() == 7 && memcmp(acc->data(), BD, 6) == 0 && (*acc)[6] == 0x01);   // role 0x01 remain slave
        CHECK(link.incoming() && memcmp(link.peer(), BD, 6) == 0 && link.inboundUp());
        CHECK(io.count(0x040A) == 0);
    }
    {   // 18. Incoming page from an UNKNOWN address is rejected 0x0F (host rejected: unacceptable BD_ADDR); no link.
        FakeIo io; g_io = &io; Hci hci(io); g_hci = &hci; BtLink link(hci); g_link = &link; hci.onEvent(evThunk, &link);
        link.setLog(logFn, nullptr); g_log.clear(); link.begin(io.now);
        BondTable bonds; seedBond(bonds, KEY1, "OpenMove by Shokz"); link.setBonds(&bonds);
        io.onCmd = [](FakeIo &f, uint16_t op, const std::vector<uint8_t> &prm) { f.cc(op, withBd({ 0x00 }, prm)); };
        static const uint8_t UNK[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01 };
        std::vector<uint8_t> cr(UNK, UNK + 6); cr.push_back(0); cr.push_back(0); cr.push_back(0); cr.push_back(0x01);
        link.onEvent(0x04, cr.data(), (uint8_t)cr.size());
        CHECK(runUntil([&]{ return io.count(0x040A) >= 1; }, 500));
        const std::vector<uint8_t> *rej = io.last(0x040A);
        CHECK(rej && rej->size() == 7 && memcmp(rej->data(), UNK, 6) == 0 && (*rej)[6] == 0x0F);
        CHECK(link.linkState() == BtLink::LINK_NONE && io.count(0x0409) == 0);
    }
    {   // 19. Incoming page while a link is UP is rejected 0x0D (limited resources).
        FakeIo io; g_io = &io; Hci hci(io); g_hci = &hci; BtLink link(hci); g_link = &link; hci.onEvent(evThunk, &link);
        link.setLog(logFn, nullptr); g_log.clear(); link.begin(io.now);
        BondTable bonds; seedBond(bonds, KEY1, "OpenMove by Shokz"); seedBond(bonds, KEY2, "Other"); link.setBonds(&bonds);
        io.onCmd = [](FakeIo &f, uint16_t op, const std::vector<uint8_t> &prm) {
            if (op == 0x0409) { f.cc(op, withBd({ 0x00 }, prm)); f.ev(0x03, connComplete(0x00)); return; }
            f.cc(op, { 0x00 }); };
        std::vector<uint8_t> a(BD, BD + 6); a.push_back(0); a.push_back(0); a.push_back(0); a.push_back(0x01);
        link.onEvent(0x04, a.data(), (uint8_t)a.size());
        CHECK(runUntil([&]{ return link.linkState() == BtLink::LINK_UP; }, 2000));
        // a SECOND request while up: reject busy
        static const uint8_t B2[6] = { 0x22, 0x22, 0x22, 0x22, 0x22, 0x22 };
        std::vector<uint8_t> b(B2, B2 + 6); b.push_back(0); b.push_back(0); b.push_back(0); b.push_back(0x01);
        link.onEvent(0x04, b.data(), (uint8_t)b.size());
        CHECK(runUntil([&]{ return io.count(0x040A) >= 1; }, 500));
        const std::vector<uint8_t> *rej = io.last(0x040A); CHECK(rej && (*rej)[6] == 0x0D);
    }
    {   // 20. Connection_Complete for a DIFFERENT address than the one we paged is IGNORED (not latched).
        FakeIo io; g_io = &io; Hci hci(io); g_hci = &hci; BtLink link(hci); g_link = &link; hci.onEvent(evThunk, &link);
        link.setLog(logFn, nullptr); g_log.clear(); link.begin(io.now);
        io.onCmd = [](FakeIo &f, uint16_t op, const std::vector<uint8_t> &prm) {
            if (preamble(f, op, prm)) return;
            if (op == 0x0405) { f.cs(op);
                std::vector<uint8_t> other = connComplete(0x00, 0x0009);
                static const uint8_t OTH[6] = { 0x77, 0x66, 0x55, 0x44, 0x33, 0x22 };
                memcpy(other.data() + 3, OTH, 6);                 // Connection_Complete for someone else
                f.ev(0x03, other); return; }
            f.cc(op, { 0x01 }); };
        link.startPrepare(); CHECK(runUntil([&]{ return !link.busy(); }, 4000));
        link.startPage(BD, 1, 0, false, "X", 1);
        CHECK(!runUntil([&]{ return !link.busy(); }, 3000));      // the foreign Connection_Complete does NOT complete our page
        CHECK(link.handle() == 0);
    }
    {   // 21. Link supervision timeout write, when master, after a link comes up (a bench knob for range-loss detection).
        FakeIo io; g_io = &io; Hci hci(io); g_hci = &hci; BtLink link(hci); g_link = &link; hci.onEvent(evThunk, &link);
        link.setLog(logFn, nullptr); g_log.clear(); link.begin(io.now); link.setSupervisionSlots(0x1F40);   // 5 s
        io.onCmd = [](FakeIo &f, uint16_t op, const std::vector<uint8_t> &prm) {
            if (preamble(f, op, prm)) return;
            if (op == 0x0405) { f.cs(op); f.ev(0x03, connComplete(0x00)); return; }
            f.cc(op, { 0x00 }); };
        link.startPrepare(); CHECK(runUntil([&]{ return !link.busy(); }, 4000));
        link.startPage(BD, 1, 0, false, "X", 1);
        CHECK(runUntil([&]{ return link.linkState() == BtLink::LINK_UP; }, 12000));
        CHECK(runUntil([&]{ return io.count(0x0C37) >= 1; }, 500));
        const std::vector<uint8_t> *w = io.last(0x0C37);
        CHECK(w && w->size() == 4 && (*w)[2] == 0x40 && (*w)[3] == 0x1F);   // handle + 0x1F40 slots
    }
```

- [ ] **Step 2: Verify fail to compile** (`linkState`, `LINK_UP`, `incoming`, `inboundUp`, `setSupervisionSlots` not members).

- [ ] **Step 3: Extend `bt/BtLink.h`**

Add to the public API:
```cpp
    enum LinkState : uint8_t { LINK_NONE, LINK_UP, LINK_SECURE, LINK_LOST };
    LinkState linkState() const { return m_link; }
    bool     incoming()  const { return m_incoming; }
    uint8_t  role()      const { return m_role; }          // 0 = master (we paged), 1 = slave (we accepted)
    bool     inboundUp() const { return m_inboundUp; } void ackInboundUp() { m_inboundUp = false; }
    bool     lost()      const { return m_link == LINK_LOST; } uint8_t lostReason() const { return m_discReason; } void ackLost() { if (m_link == LINK_LOST) m_link = LINK_NONE; }
    void     setSupervisionSlots(uint16_t slots) { m_supSlots = slots; }
```
Add private members:
```cpp
    LinkState m_link = LINK_NONE; bool m_incoming = false; uint8_t m_role = 0; bool m_inboundUp = false;
    uint16_t m_supSlots = 0; bool m_supDone = false;
```
Add opcode enum members in the .cpp anonymous namespace: `OP_ACCEPT_CONN = 0x0409, OP_REJECT_CONN = 0x040A, OP_WRITE_LINK_SUP_TO = 0x0C37,` and event `EV_CONNECTION_REQUEST = 0x04, EV_ROLE_CHANGE = 0x12,`.

- [ ] **Step 4: Implement in `bt/BtLink.cpp`**

In `onEvent`, add the Connection_Request handler (decided synchronously — the link-key reply already submits from here, so this is safe):
```cpp
    } else if (code == EV_CONNECTION_REQUEST && len >= 10) {
        char bs[18]; hciFormatBd(p, bs);
        uint8_t linkType = p[9];
        bool paging = (m_op == PAGE);
        bool bonded = m_bonds && m_bonds->find(p);
        if (linkType != 0x01) {                                  // not ACL (SCO/eSCO): refuse
            uint8_t r[7]; memcpy(r, p, 6); r[6] = 0x0F; logf("conn_req: bd=%s -> reject(0x0F non-ACL)", bs); m_hci.submit(OP_REJECT_CONN, r, 7, nullptr, nullptr);
        } else if (m_link == LINK_UP || m_link == LINK_SECURE) {
            uint8_t r[7]; memcpy(r, p, 6); r[6] = 0x0D; logf("conn_req: bd=%s -> reject(0x0D busy)", bs); m_hci.submit(OP_REJECT_CONN, r, 7, nullptr, nullptr);
        } else if (paging && memcmp(p, m_bd, 6) != 0) {          // paging someone else: refuse this crossed page
            uint8_t r[7]; memcpy(r, p, 6); r[6] = 0x0D; logf("conn_req: bd=%s -> reject(0x0D paging other)", bs); m_hci.submit(OP_REJECT_CONN, r, 7, nullptr, nullptr);
        } else if (!bonded) {                                    // idle, unknown address: never pair a stranger from an incoming page
            uint8_t r[7]; memcpy(r, p, 6); r[6] = 0x0F; logf("conn_req: bd=%s -> reject(0x0F unknown)", bs); m_hci.submit(OP_REJECT_CONN, r, 7, nullptr, nullptr);
        } else {                                                 // accept: remain slave (role 0x01)
            if (!paging) { memcpy(m_bd, p, 6); m_incoming = true; m_keyOffered = false;
                const Bond *b = m_bonds->find(p); if (b) BondTable::copyName(m_pageName, b->name); }
            uint8_t r[7]; memcpy(r, p, 6); r[6] = 0x01; logf("conn_req: bd=%s -> accept(slave)", bs);
            m_hci.submit(OP_ACCEPT_CONN, r, 7, nullptr, nullptr);
        }
    } else if (code == EV_ROLE_CHANGE && len >= 8) {
        if (p[0] == 0x00 && memcmp(p + 1, m_bd, 6) == 0) { m_role = p[7]; logf("role=%s", m_role ? "slave" : "master"); }
    }
```
Change the Connection_Complete handler (currently unconditional) to address-check and drive link state:
```cpp
    } else if (code == EV_CONNECTION_COMPLETE && len >= 11) {
        if (memcmp(p + 3, m_bd, 6) != 0) { char bs[18]; hciFormatBd(p + 3, bs); logf("connection_complete: bd=%s ignored", bs); return; }
        m_connStatus = p[0];
        if (p[0] == 0x00) {
            m_handle = (uint16_t)(p[1] | (p[2] << 8)); m_link = LINK_UP; m_supDone = false;
            m_role = m_incoming ? 1 : 0;
            if (m_op != PAGE) m_inboundUp = true;                // an accepted incoming link (no page in flight)
        }
        m_connDone = true;
    }
```
In the Disconnection_Complete handler, set link state and clear the handle at once (closing the "keeps the handle" deferral):
```cpp
        if (h == m_handle) { m_discReason = p[3]; m_encrypted = false; m_link = LINK_LOST; m_handle = 0; m_discDone = true; }
```
In the Encryption_Change handler, add `if (m_encrypted) m_link = LINK_SECURE;` after `m_encrypted = (p[3] != 0);`.
Address-scope the key flags: in Link_Key_Notification's handler, the existing code already guards psrm/name on `memcmp(p, m_bd, 6)`; leave `m_haveLinkKey = true` but guard it: change the first line to `if (memcmp(p, m_bd, 6) == 0) m_haveLinkKey = true;` (closing the address-blind `m_haveLinkKey` deferral). `m_keyOffered` is already `m_bd`-scoped (Task-3/piece-1 code).

Drive the supervision write from `reconcileScan()` (it already runs every tick when no command is outstanding) — extend it:
```cpp
    if (!m_supDone && m_supSlots && (m_link == LINK_UP || m_link == LINK_SECURE) && m_role == 0 && !m_cmdBusy) {
        uint8_t w[4] = { (uint8_t)(m_handle & 0xFF), (uint8_t)(m_handle >> 8), (uint8_t)(m_supSlots & 0xFF), (uint8_t)(m_supSlots >> 8) };
        if (issue(OP_WRITE_LINK_SUP_TO, w, 4)) { m_supDone = true; logf("supervision=0x%04X st=submitted", m_supSlots); }
    }
```
(Master-only: the command errors for a slave, so it is skipped when `m_role != 0`.)

- [ ] **Step 5: Build + run + iterate** — `./bt/test/run.sh` → `btlink_test` green, all of 1–21.

- [ ] **Step 6: Mutation-check** (scratch): (a) accept an unknown address (drop the `!bonded` reject) → 18 fails; (b) role byte 0x00 in Accept → 17 fails; (c) drop the Connection_Complete address check → 20 fails; (d) supervision write without the `m_role == 0` guard → still passes 21 (master) but add a slave arm if you want it pinned — optional.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/M2Radio && git commit -m "bt: BtLink incoming page + link state + address checks + supervision knob (NEW-34 piece 2)

onEvent handles Connection_Request (accept a bonded address as slave role 0x01; reject unknown 0x0F, busy 0x0D, crossed-page 0x0D) and Role_Change; Connection_Complete is address-checked and drives LINK_NONE/UP/SECURE/LOST; Disconnection_Complete clears the handle at once; m_haveLinkKey is address-scoped. setSupervisionSlots() writes Write_Link_Supervision_Timeout on a master link (a bench knob for range-loss detection). Closes three piece-1 deferrals.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>" -- bt/BtLink.h bt/BtLink.cpp bt/test/btlink_test.cpp && git log --oneline -1
```

---

## Task 5: A2dpSource — one attempt in either direction

**Files:**
- Modify: `~/Development/M2Radio/bt/A2dpSource.h`
- Modify: `~/Development/M2Radio/bt/A2dpSource.cpp`
- Test: `~/Development/M2Radio/bt/test/a2dpsource_test.cpp`

**Context:** Read `bt/A2dpSource.h`, `bt/A2dpSource.cpp` (all of `connect`, `onData`, `service`), and `bt/test/a2dpsource_test.cpp` (all 8 scenarios). Today `connect()` is one blocking call that does the bonded-candidate walk itself. Piece 2 moves the WALK to `BtSession` (Task 6) and makes `A2dpSource` one *attempt*: given a target (a page target, an inquiry filter, or an inbound link), tick it to STREAMING or a failure, with teardown on loss. Config adoption from the Avdtp acceptor.

- [ ] **Step 1: Rewrite `bt/A2dpSource.h`**

Replace the public API (keep `Result` values, add `LOST`, `STOPPED`; keep `resultName`, ctor, `setLog/setBonds/bonds/setPin/setLegacyPin`, the accessors `hci/l2/avdtp/sdpServer/link/mediaCid/mediaMtu/started/avdtpVersion/sbcParams`, `onEvent/onAcl/service`). Add:
```cpp
    enum Result : uint8_t { OK = 0, CONNECT_FAILED, PAIR_FAILED, L2CAP_FAILED, AVDTP_FAILED, LOST, STOPPED, PENDING };
    struct Target { enum Kind : uint8_t { PAGE, INQUIRY, INBOUND } kind;
                    uint8_t bd[6]; uint8_t psrm; uint16_t clk; bool clkValid; char name[BondTable::NAME_LEN + 1];
                    uint8_t attempts; const char *nameFilter; };
    void begin(uint32_t now, uint8_t aclNum);        // reset the attempt machine; call once per session (with the ACL buffer count)
    bool start(const Target &t);                      // begin one attempt; false if one is already running
    void tick(uint32_t now);                          // advance it
    void stop();                                      // tear down -> FAILED(STOPPED)
    bool     busy()   const { return m_st != IDLE && m_st != DONE; }
    Result   result() const { return m_result; }
    enum St : uint8_t { IDLE, LINKING, PAIRING, L2, SDP, AVDTP_WAIT, AVDTP, STREAMING, DISCONNECTING, DONE };
    St state() const { return m_st; }                 // (was avdtp state; the attempt state now)
```
Add private members: `St m_st = IDLE; Result m_result = OK; uint32_t m_deadline = 0; uint8_t m_aclNum = 0; Target m_t{}; bool m_inbound = false;` and keep the existing `m_hci/m_l2/m_link/m_avdtp/m_sdpServer/m_sdpDone/m_sdpVer/m_params/m_bonds/m_log*`.

- [ ] **Step 2: Rewrite the tests onto the attempt model**

The existing 8 scenarios test the WALK, which has moved to BtSession. Re-home them: the walk pins (order, wildcard, name seam, stop-at-first) move to `btsession_test` (Task 6). What stays in `a2dpsource_test` is ONE attempt: a PAGE target that reaches PAIR_FAILED (controller refuses auth), an INBOUND target that reaches STREAMING via the acceptor, config adoption, teardown on loss. Keep the file's `FakeIo`/controller scaffolding. Convert scenario 1 to:
```cpp
    {   // 1. A PAGE attempt: page the target, pair (controller refuses auth) -> PAIR_FAILED, link torn down.
        Rig r; g_present = SHOKZ; g_link = &r.src.link();
        A2dpSource::Target t{}; t.kind = A2dpSource::Target::PAGE; memcpy(t.bd, SHOKZ, 6); t.psrm = 1; t.attempts = 1;
        r.src.begin(fakeNow(), 0); CHECK(r.src.start(t));
        CHECK(runUntil([&]{ return !r.src.busy(); }, 40000) && r.src.result() == A2dpSource::PAIR_FAILED);
        CHECK(r.io.count(0x0401) == 0 && r.io.count(0x0405) == 1 && r.io.count(0x0406) == 1);   // paged, no inquiry, torn down
    }
```
(Add a `runUntil` in a2dpsource_test mirroring btlink_test's: `io.now += 10; hci.service(); src.tick(io.now); src.service();`.) Add an INBOUND attempt scenario that scripts the acceptor to STREAMING and checks `sbcParams().bitpool` equals the peer's chosen bitpool (config adoption), plus a self-START scenario (media OPEN, no peer START within `START_WAIT_MS` → we send START). Add a loss scenario: reach STREAMING, inject Disconnection_Complete, tick → `result()==LOST`, `l2().byPsm(...)==nullptr` (teardown ran).

- [ ] **Step 3: Implement `bt/A2dpSource.cpp` as the attempt state machine**

`onData` is unchanged EXCEPT the AVDTP demux must route the peer's signalling to `m_avdtp.onSignalling` for the acceptor's channel too. Since `adoptInbound` sets the acceptor's `m_sig` to the peer-opened channel, route by `ch.psm == Avdtp::PSM && (&ch == the avdtp sig channel)`; simplest: route any `ch.psm == Avdtp::PSM` non-media channel to `onSignalling`. Keep the SDP-server and SDP-client branches.

`begin(now, aclNum)`: `m_aclNum = aclNum; m_st = IDLE; m_result = OK;` and `m_link.begin(now); m_link.startPrepare();` (PREPARE once per session — the attempt assumes it has run; BtSession calls `begin` once and waits for PREPARE before the first `start`).

`start(t)`: refuse if `busy()`. Capture `m_t = t; m_inbound = (t.kind == Target::INBOUND);`. Then:
- PAGE: `m_link.startPage(t.bd, t.psrm, t.clk, t.clkValid, t.name, t.attempts); m_st = LINKING;`
- INQUIRY: `m_link.startInquiry(t.nameFilter); m_st = LINKING;` (on the inquiry's OK, LINKING issues the page from `m_link.target()`).
- INBOUND: the link is already UP (BtLink accepted it); `m_link.ackInboundUp(); m_st = PAIRING; m_link.startPair(true);`

`tick(now)`: advance by `m_st`, first checking for a link loss in any non-terminal state:
```cpp
void A2dpSource::tick(uint32_t now) {
    m_link.tick(now);
    if (m_st != IDLE && m_st != DONE && m_st != DISCONNECTING && m_link.lost()) {
        m_link.ackLost(); m_avdtp.reset(); m_l2.reset(); logf("attempt: link lost reason=0x%02X", m_link.lostReason());
        m_result = LOST; m_st = DONE; return;
    }
    switch (m_st) {
    case LINKING:
        if (m_link.busy()) return;
        if (m_t.kind == Target::INQUIRY && m_link.op() == BtLink::NONE && m_link.result() == BtLink::OK && !m_pagedFromInquiry) {
            BtLink::Target h = m_link.target();
            if (!h.valid) { m_result = CONNECT_FAILED; m_st = DISCONNECTING; break; }
            m_pagedFromInquiry = true; m_link.startPage(h.bd, h.psrm, h.clk, h.clkValid, h.name, BtLink::PAGE_ATTEMPTS); return;
        }
        if (m_link.result() != BtLink::OK) { m_result = CONNECT_FAILED; m_st = DISCONNECTING; break; }
        m_link.startPair(m_inbound); m_st = PAIRING; break;
    case PAIRING:
        if (m_link.busy()) return;
        if (m_link.result() != BtLink::OK) { m_result = PAIR_FAILED; m_st = DISCONNECTING; break; }
        m_l2.begin(m_link.handle(), m_aclNum); m_l2.acceptIncoming(true); m_l2.allowPsm(Avdtp::PSM); m_l2.allowPsm(Sdp::PSM);
        m_l2.onData(onData, this);
        m_st = L2; m_deadline = now + 5000;
        if (!m_inbound) { m_sdpChan = m_l2.connect(Sdp::PSM, 0x0040); }   // outbound: query the sink's AVDTP version
        break;
    case L2:  /* outbound: drive SDP client as today; inbound: skip straight to AVDTP_WAIT.  See below. */ break;
    case SDP: /* ... */ break;
    case AVDTP_WAIT:  /* inbound: wait m_avdtpWaitMs for the peer to open signalling; adoptInbound; else initiate */ break;
    case AVDTP: /* outbound initiator as today; acceptor: self-START on mediaReady after START_WAIT_MS; STREAMING on started() */ break;
    case DISCONNECTING:
        if (!m_link.busy() && m_link.op() == BtLink::NONE && m_link.handle() == 0) { m_st = DONE; return; }
        if (m_link.op() != BtLink::DISCONNECT) m_link.startDisconnect();
        break;
    default: break;
    }
    m_l2.service(); m_avdtp.service(); m_sdpServer.service(m_l2);
}
```
Fill L2/SDP/AVDTP_WAIT/AVDTP by translating the quoted old `connect()` tail into states with `m_deadline`. Key differences: the outbound path is the old sequence (SDP client query, then `connect(Avdtp::PSM, 0x0041)`, `m_avdtp.begin(...)`, `m_avdtp.start(want)`, wait STREAMING/FAILED 15 s). The inbound path skips SDP client, enters AVDTP_WAIT (deadline `m_avdtpWaitMs` default 2000): each tick `m_avdtp.adoptInbound(m_l2)`; if `m_avdtp.role() == ACCEPTOR` proceed (the acceptor drives itself via `service()`), else on timeout `m_avdtp.begin(m_l2, 0x0041, 0x0042); m_avdtp.start(want);` as initiator. In AVDTP, for the acceptor: `m_avdtp.adoptInbound(m_l2)` each tick (to pick up the media channel in OPENING); if `m_avdtp.mediaReady()` and `now` past a `START_WAIT_MS` (1000) mark, `m_avdtp.startSelf()`. STREAMING when `m_avdtp.started()` → `m_result = OK; m_st = STREAMING;`. On the failure/OPEN-timeout, `m_st = DISCONNECTING`.

Add members: `bool m_pagedFromInquiry = false; L2cap::Channel *m_sdpChan = nullptr; uint32_t m_avdtpWaitMs = 2000; uint32_t m_startWaitAt = 0;`. Reset `m_pagedFromInquiry=false` in `start()`.

`sbcParams()`: when the acceptor adopted a config, map `m_avdtp.sbcConfig()` → `m_params` (in the AVDTP state when `configChanged()` fires): rate→`Sbc::Rate`, mode bit→`Sbc::Mode`, alloc, blocks, subbands, `bitpool = cfg.maxBitpool`. Otherwise `m_params` stays the initiator default. Add a private `void adoptConfig();` called when `m_avdtp.configChanged()`.

`stop()`: `m_st = DISCONNECTING; m_result = STOPPED;` (the DISCONNECTING state tears down; on completion → DONE).

- [ ] **Step 4: Build + run + iterate** — `./bt/test/run.sh` → `a2dpsource_test` green.

- [ ] **Step 5: Mutation-check** (scratch): (a) skip `m_l2.allowPsm(Avdtp::PSM)` → the inbound acceptor cannot get its channel; the INBOUND scenario fails; (b) don't adopt the config (leave `m_params` default) → the config-adoption bitpool assert fails; (c) don't reset l2/avdtp on loss → the teardown assert fails.

- [ ] **Step 6: Commit**

```bash
cd ~/Development/M2Radio && git commit -m "bt: A2dpSource is one attempt in either direction (NEW-34 piece 2)

start(Target)/tick(now): a PAGE/INQUIRY/INBOUND attempt walks LINKING->PAIRING->L2->SDP->AVDTP_WAIT->AVDTP->STREAMING or fails with teardown; a link loss in any state resets L2cap+Avdtp and ends LOST. Inbound skips the SDP client, waits for the peer to open AVDTP, and adopts the peer's SBC config (sbcParams() reflects the negotiated bitpool). The bonded-candidate walk moves out to BtSession.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>" -- bt/A2dpSource.h bt/A2dpSource.cpp bt/test/a2dpsource_test.cpp && git log --oneline -1
```

---

## Task 6: BtSession — the policy

**Files:**
- Create: `~/Development/M2Radio/bt/BtSession.h`
- Create: `~/Development/M2Radio/bt/BtSession.cpp`
- Create: `~/Development/M2Radio/bt/test/btsession_test.cpp`
- Modify: `~/Development/M2Radio/bt/test/run.sh` (add `btsession_test` to the `for t in ...` loop)

**Context:** `BtSession` owns the policy from the spec (§7). It is Arduino-free, no heap. It ticks `A2dpSource` beneath it, applies the boot walk and the lost-peer retry cadence, drives `wantPageScan`, cancels the retry on an incoming link, keeps stats, and calls the app back. It reads bonds from a `BondTable*` but never touches the EEPROM (the app persists on the attempt-end callback).

- [ ] **Step 1: Write the failing tests → `bt/test/btsession_test.cpp`**

Use the same `FakeIo`/`Hci`/controller scaffolding as `a2dpsource_test.cpp` (copy the FakeIo, controller, `runUntil`, `SHOKZ`/`SINK`/`mk`). Scenarios (each shown RED by a mutant later):
```cpp
// B1. Boot walk: two bonds MRU-first through the name filter, first candidate PAGE_ATTEMPTS, then inquiry.
// B2. Lost-peer retry: reach STREAMING, inject a drop, and assert exactly ONE Create_Connection per RETRY_MS
//     to the LOST address only (never the other bond, never inquiry), forever.
// B3. Page scan wanted: on in WAITING and before the first link; off the moment a link is UP; off in MANUAL.
// B4. Incoming cancels the retry: in WAITING after a drop, an inboundUp() starts an INBOUND attempt and
//     the retry timer does not also fire a Create_Connection.
// B5. Stats: links, lost, lastReason, by (paged/inquiry/incoming), reconnectMs (loss->STREAMING), attempts.
// B6. Callbacks: onStream(true,..,by) on each STREAMING entry; onStream(false,reason,..) on each loss;
//     onAttempt(result, pairedBy) on every attempt end.
// B7. MANUAL: disconnect() -> no attempts, page scan off; resume() -> boot policy again.
```
Write each as: build a `BtSession`, register callbacks that record into vectors, `session.begin(src, &bonds, "Shokz", aclNum); session.setRetryMs(3000);` then drive `runUntil` while counting `io.count(0x0405)` over fake time. For B2, after reaching STREAMING inject `f.ev(0x05, {0x00, handle_lo, handle_hi, 0x08})` and assert the Create_Connection count increments by exactly one per 3000 ms of fake time, all to SHOKZ.

- [ ] **Step 2: Add `btsession_test` to `bt/test/run.sh`** — change the loop to `for t in bondtable_test l2cap_test avdtp_test sdp_test btlink_test a2dpsource_test btsession_test sbc_test rtp_test mediapacketizer_test; do`.

- [ ] **Step 3: Verify fail to compile** — `./bt/test/run.sh` → no such file `BtSession.h`.

- [ ] **Step 4: Write `bt/BtSession.h`**

```cpp
// BtSession -- the A2DP link-lifecycle policy (NEW-34 piece 2): boot walk + inquiry, lost-peer retry
// forever, page-scan-when-idle, retry-cancel-on-incoming, MANUAL mode, stats, callbacks.  Ticks an
// A2dpSource beneath it.  Arduino-free, no heap; the app persists bonds on the attempt-end callback.  MIT.
#pragma once
#include <stdint.h>
#include "A2dpSource.h"
#include "BondTable.h"
class BtSession {
public:
    enum State : uint8_t { IDLE, CONNECTING, WAITING, STREAMING, DISCONNECTING, MANUAL };
    enum By : uint8_t { BY_NONE, BY_PAGED, BY_INQUIRY, BY_INCOMING };
    struct Stats { uint32_t links, lost, attempts, accepts, rejects; uint8_t lastReason; By by; uint32_t reconnectMs, lostAt; };
    typedef void (*StreamFn)(void *ctx, bool streaming, uint8_t reason, By by);
    typedef void (*AttemptFn)(void *ctx, A2dpSource::Result r, const char *pairedBy);
    explicit BtSession(A2dpSource &src) : m_src(src) {}
    void begin(BondTable *bonds, const char *targetName, uint8_t aclNum, uint32_t now);
    void tick(uint32_t now);
    void disconnect();          // -> DISCONNECTING -> MANUAL
    void resume();              // MANUAL -> boot policy
    void retryNow();            // fire the retry timer now (NEW-35 hook)
    void onStream(StreamFn fn, void *ctx)  { m_streamCb = fn; m_streamCtx = ctx; }
    void onAttempt(AttemptFn fn, void *ctx){ m_attemptCb = fn; m_attemptCtx = ctx; }
    void setRetryMs(uint32_t ms) { m_retryMs = ms; }
    State state() const { return m_state; }
    const Stats &stats() const { return m_stats; }
    bool wantPageScan() const { return m_wantScan; }
private:
    void startBootWalk(); void startReconnect(uint32_t now); void beginAttempt(const A2dpSource::Target &t);
    A2dpSource &m_src; BondTable *m_bonds = nullptr; const char *m_target = nullptr; uint8_t m_aclNum = 0;
    State m_state = IDLE; uint32_t m_retryMs = 10000, m_retryAt = 0; bool m_wantScan = false;
    // boot walk cursor
    bool m_boot = true; uint8_t m_walkIdx = 0; bool m_walkFirst = true; bool m_triedInquiry = false;
    // reconnect target (the lost address)
    uint8_t m_lostBd[6] = {0}; bool m_haveLost = false;
    Stats m_stats{}; A2dpSource::Result m_lastResult = A2dpSource::OK;
    StreamFn m_streamCb = nullptr; void *m_streamCtx = nullptr;
    AttemptFn m_attemptCb = nullptr; void *m_attemptCtx = nullptr;
};
```

- [ ] **Step 5: Write `bt/BtSession.cpp`** — the policy engine. Key bodies:

`begin`: `m_bonds = bonds; m_target = targetName; m_aclNum = aclNum; m_src.begin(now, aclNum); m_boot = true; startBootWalk();` (startBootWalk sets `m_state = CONNECTING` and issues the first attempt once `m_src` is ready — but PREPARE runs first inside `m_src.begin`; gate the first `start` on `!m_src.link().busy()` in `tick`).

`tick(now)`:
```cpp
void BtSession::tick(uint32_t now) {
    m_src.tick(now);
    // page scan wanted whenever no link is up and a bond exists, and never in MANUAL
    bool linkUp = m_src.link().linkState() == BtLink::LINK_UP || m_src.link().linkState() == BtLink::LINK_SECURE;
    m_wantScan = (!linkUp && m_bonds && m_bonds->count() > 0 && m_state != MANUAL);
    m_src.link().wantPageScan(m_wantScan);
    // an accepted incoming link cancels any pending retry and starts an INBOUND attempt
    if (m_src.link().inboundUp() && (m_state == WAITING || m_state == IDLE)) {
        A2dpSource::Target t{}; t.kind = A2dpSource::Target::INBOUND; memcpy(t.bd, m_src.link().peer(), 6);
        m_stats.by = BY_INCOMING; beginAttempt(t); return;
    }
    switch (m_state) {
    case CONNECTING:
        if (m_src.busy() || m_src.link().busy()) return;
        handleAttemptEnd(now);                         // fires onAttempt, advances the walk or -> STREAMING/WAITING
        break;
    case STREAMING:
        if (m_src.result() == A2dpSource::LOST || m_src.link().lost()) {   // the attempt reported a drop
            m_stats.lost++; m_stats.lastReason = m_src.link().lostReason(); m_stats.lostAt = now;
            if (m_streamCb) m_streamCb(m_streamCtx, false, m_stats.lastReason, m_stats.by);
            memcpy(m_lostBd, m_src.link().peer(), 6); m_haveLost = true;
            m_boot = false; m_state = WAITING; m_retryAt = now + m_retryMs;   // FIRST retry waits a full cycle
        }
        break;
    case WAITING:
        if ((int32_t)(now - m_retryAt) >= 0 && !m_src.busy() && !m_src.link().busy()) startReconnect(now);
        break;
    case DISCONNECTING:
        if (!m_src.busy() && !m_src.link().busy()) { m_state = MANUAL; }
        break;
    default: break;
    }
}
```
`handleAttemptEnd`: read `m_src.result()`; fire `onAttempt(result, m_src.link().pairedBy())`. On OK → `m_stats.links++;` set `m_stats.by` (PAGED if the target was a page, INQUIRY if it came from inquiry, INCOMING if inbound); if a reconnect, `m_stats.reconnectMs = now - m_stats.lostAt;`; `m_state = STREAMING;` fire `onStream(true, 0, by)`. On failure → advance the boot walk (next candidate, then inquiry, then WAITING+retry) or, if reconnecting, `m_state = WAITING; m_retryAt = now + m_retryMs;`.

`startBootWalk`: build the MRU candidate list from `m_bonds` with the name filter (an empty stored name is a wildcard — same rule as the old `A2dpSource::connect` walk, MOVED here). Issue the first candidate as a PAGE target (`attempts = PAGE_ATTEMPTS` for the first, 1 for later), or if none, an INQUIRY target with the filter. `m_state = CONNECTING`.

`startReconnect`: one PAGE attempt to `m_lostBd` only (look the bond up for psrm/name), `attempts = 1`. `m_state = CONNECTING` (a reconnect is a one-candidate walk).

`disconnect`: `m_src.stop(); m_state = DISCONNECTING;`. `resume`: `m_boot = true; startBootWalk();`. `retryNow`: `m_retryAt = 0;` (fires next tick if WAITING).

- [ ] **Step 6: Build + run + iterate** — `./bt/test/run.sh` → `btsession_test: N checks, 0 failures`, `BT-HOST-TESTS: PASS`, every suite green.

- [ ] **Step 7: Mutation-check** (scratch): (a) reconnect pages every bond, not just the lost one → B2 fails (a page to the other bond appears); (b) first retry fires immediately (no full-cycle wait) → B2's cadence count is off by one; (c) `wantScan` true while linked → B3 fails; (d) incoming does not cancel the retry (remove the inboundUp branch) → B4 sees an extra Create_Connection.

- [ ] **Step 8: Commit**

```bash
cd ~/Development/M2Radio && git commit -m "bt: BtSession -- the A2DP link-lifecycle policy (NEW-34 piece 2)

Boot walk (bonds MRU-first through the name filter, then inquiry) + lost-peer retry forever (one page per RETRY_MS to the lost address only, first retry a full cycle so the headset gets the first move) + page-scan-when-idle + retry-cancel-on-incoming + MANUAL/resume + stats + onStream/onAttempt callbacks. The bonded-candidate walk moves here from A2dpSource; the library never touches the EEPROM.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>" -- bt/BtSession.h bt/BtSession.cpp bt/test/btsession_test.cpp bt/test/run.sh && git log --oneline -1
```

---

## Task 7: AudioOutputBluetooth — end(), paused state, idle/paused counters

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/AudioOutputBluetooth.h`
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/AudioOutputBluetooth.cpp`

**Context:** Read both files in full. This node is an Arduino `AudioStream`, so it is NOT host-tested; it is verified by building and by the `[media]`/`[lifecycle]` gates. Today it has `begin()` (two overloads) and `poll()` but no `end()`, and `update()` silently drops input when `!m_l2`. Piece 2 needs: `end()` (called by the session's `onStream(false)`), a paused state (`update()` discards while `!m_src->started()` — a SUSPEND), and counters so the gap across a reconnect is measured rather than inferred from `drops`.

- [ ] **Step 1: Add `end()` + paused/idle counting to `AudioOutputBluetooth.h`**

After `void poll();` add:
```cpp
    void end();                                  // tear down for a link loss: clears the channel, empties the ring/packetizer
    uint32_t idleBlocks()   const { return m_idleBlocks; }    // blocks dropped while not begun (no channel)
    uint32_t pausedBlocks() const { return m_pausedBlocks; }  // blocks dropped while begun but the stream is SUSPENDED
```
Add a source pointer so `update()` can see `started()`, and the two counters, to the private members:
```cpp
    A2dpSource *m_src = nullptr;                  // set by begin(A2dpSource&); update() pauses when !m_src->started()
    uint32_t m_idleBlocks = 0, m_pausedBlocks = 0;
```

- [ ] **Step 2: Implement in `AudioOutputBluetooth.cpp`**

In `begin(A2dpSource &src)` capture the source: change it to `m_src = &src; begin(src.l2(), src.mediaCid(), src.mediaMtu(), src.sbcParams());`. In the `begin(L2cap&, ...)` overload, pass the negotiated frame length to the packetizer: change `m_pk.begin(mtu);` to `m_pk.begin(mtu, Sbc::frameLength(p));` (Task 2 gave `MediaPacketizer::begin` the frame-length parameter; `Sbc::frameLength(p)` computes it from the adopted config).

Add `end()`:
```cpp
void AudioOutputBluetooth::end() {
    m_l2 = nullptr; m_cid = 0; m_src = nullptr;
    m_pcmHead = m_pcmTail = 0;                    // empty the PCM ring (SPSC: safe when no external clock runs it)
    m_pk.begin(0);                                // reset the packetizer (seq, ring); mtu 0 -> perPkt clamps to 1, harmless until the next begin()
}
```

In `update()`, add the paused branch. After the existing `if (!m_l2) { ...release...; m_idleBlocks++; return; }` (change the existing early return to also count `m_idleBlocks++`), add before the ring copy:
```cpp
    if (m_src && !m_src->started()) {             // SUSPENDED: keep the channel but discard audio
        if (l) release(l); if (r) release(r); m_pausedBlocks++; return;
    }
```

- [ ] **Step 3: Build the example (default, card-absent) to confirm it compiles**

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake >/tmp/bt_cfg.log 2>&1 && cmake --build build >/tmp/bt_bld.log 2>&1 && echo BUILT $(ls -la build/bt_tone_test.elf | awk '{print $5}')
```
Expected: `BUILT <size>`. (This build also pulls the Task 1–6 M2Radio changes via the local-first checkout, so a compile error here can be an M2Radio API mismatch — fix it in M2Radio and re-run the host suite.)

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb && git commit -m "audio: AudioOutputBluetooth end() + paused/idle counters (NEW-34 piece 2)

end() tears the media path down on a link loss; update() discards (and counts pausedBlocks) while the stream is SUSPENDED and (idleBlocks) while not begun, so the reconnect gap is measured, not inferred from drops. begin() passes the negotiated frame length to the packetizer so an adopted bitpool != 53 batches correctly.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>" -- examples/audio/bt_tone_test/AudioOutputBluetooth.h examples/audio/bt_tone_test/AudioOutputBluetooth.cpp && git log --oneline -1
```

---

## Task 8: bt_tone_test onto the session + the health line

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/bt_tone_test.cpp`
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/CMakeLists.txt`

**Context:** Read `bt_tone_test.cpp` in full (setup + loop + the `M2_BT_CONNECT_RETRY` block) and `CMakeLists.txt`'s option list. This task replaces the one-shot/retry connect with `BtSession`, adds the three callbacks around `btout.begin()/end()`, prints the health line, removes `M2_BT_CONNECT_RETRY`, and fixes `bonds_forgotten=` to print the pre-wipe count. It must keep the card-absent gate (`run_qemu.sh`) green: with no card the session never reaches STREAMING, so the heartbeat stays `streaming=0` and the health line must still print.

- [ ] **Step 1: Add the session and callbacks**

Add `#include <BtSession.h>` beside the other M2Radio includes. Replace the `static A2dpSource src(...)` region so a `BtSession` wraps it:
```cpp
static A2dpSource src(hci, hciIo);
static BtSession  session(src);
static BondTable  bonds;
```
Add callback functions near `btLog`:
```cpp
static void onStreamCb(void *, bool streaming, uint8_t reason, BtSession::By by) {
    if (streaming) {
        btout.begin(src);
        const char *bs = by == BtSession::BY_PAGED ? "paged" : by == BtSession::BY_INQUIRY ? "inquiry" : by == BtSession::BY_INCOMING ? "incoming" : "none";
        CONSOLE.print("streaming by="); CONSOLE.print(bs);
        CONSOLE.print(" bitpool="); CONSOLE.print(src.sbcParams().bitpool);
        CONSOLE.print(" frames_per_pkt="); CONSOLE.print(btout.framesPerPacket());
        CONSOLE.print(" media_mtu="); CONSOLE.println(src.mediaMtu());
    } else {
        btout.end();
        CONSOLE.print("bt_dropped reason=0x"); printHex8(reason);
        CONSOLE.print(" links="); CONSOLE.println(session.stats().links);
    }
}
static void onAttemptCb(void *, A2dpSource::Result r, const char *pairedBy) {
    BondStoreEeprom::save(bonds);
    CONSOLE.print("a2dp="); CONSOLE.print(A2dpSource::resultName(r));
    CONSOLE.print(" bonds="); CONSOLE.print(bonds.count());
    CONSOLE.print(" paired_by="); CONSOLE.println(pairedBy);
}
```

- [ ] **Step 2: Replace the setup() connect block**

Replace the whole `#if !defined(M2_BT_CONNECT_RETRY) ... #else ... #endif` region (the setup-time one-shot connect) with the bond-store load, the `bonds_forgotten=` fix, and `session.begin`:
```cpp
#if defined(M2_BT_FORGET_BONDS)
    { uint8_t before = bonds.count(); (void)BondStoreEeprom::wipe(bonds);
      CONSOLE.print("bonds_forgotten="); CONSOLE.println(before); }
#else
    BondStoreEeprom::load(bonds);
#endif
    src.setBonds(&bonds);
    CONSOLE.print("bonds_boot="); CONSOLE.println(bonds.count());
#if defined(M2_BT_ACL_TRACE)
    src.l2().onAclTrace(aclTrace, nullptr);
#endif
#if defined(M2_BT_LEGACY_PIN)
    src.setLegacyPin(true);
#endif
#if defined(M2_BT_SUPERVISION_MS)
    src.link().setSupervisionSlots((uint16_t)((uint32_t)M2_BT_SUPERVISION_MS * 1000u / 625u));  // ms -> 0.625 ms slots
#endif
    session.onStream(onStreamCb, nullptr);
    session.onAttempt(onAttemptCb, nullptr);
#if defined(M2_BT_RETRY_MS)
    session.setRetryMs(M2_BT_RETRY_MS);
#endif
    // begin the session only if HCI came up; with no card it never reaches STREAMING and the heartbeat stays vacuous
    if (s_hciSt == Hci::OK) {
#if defined(M2_BT_TARGET_NAME)
        session.begin(&bonds, M2_BT_TARGET_NAME, s_aclNum, millis());
#else
        session.begin(&bonds, nullptr, s_aclNum, millis());
#endif
    } else {
        CONSOLE.println("a2dp=deferred (no HCI: card absent)");
    }
```
(Note: with no card, `s_hciSt != Hci::OK`, so `session.begin` is skipped — the card-absent gate then sees `bonds_boot=0`, `a2dp=deferred`, and the vacuous heartbeat. Update `run_qemu.sh` in Step 5 accordingly, since it currently expects `a2dp=connect_failed`.)

- [ ] **Step 3: Replace loop()**

Replace the `loop()` body with the session tick + drain + the new heartbeat/health line:
```cpp
void loop() {
    yield();
    session.tick(millis());
    src.service();
    btout.poll();
    static uint32_t last = 0;
    if (millis() - last >= 1000) {
        last = millis();
        const BtSession::Stats &st = session.stats();
        const char *bs = st.by == BtSession::BY_PAGED ? "paged" : st.by == BtSession::BY_INQUIRY ? "inquiry" : st.by == BtSession::BY_INCOMING ? "incoming" : "none";
        CONSOLE.print("hb streaming="); CONSOLE.print(src.started() ? 1 : 0);
        CONSOLE.print(" blocks="); CONSOLE.print(btout.blocks());
        CONSOLE.print(" packets="); CONSOLE.print(btout.packets());
        CONSOLE.print(" drops="); CONSOLE.print(btout.drops());
        CONSOLE.print(" hw="); CONSOLE.println(btout.queueHighWater());
        CONSOLE.print("bt_link links="); CONSOLE.print(st.links);
        CONSOLE.print(" lost="); CONSOLE.print(st.lost);
        CONSOLE.print(" reason=0x"); printHex8(st.lastReason);
        CONSOLE.print(" by="); CONSOLE.print(bs);
        CONSOLE.print(" reconnect_ms="); CONSOLE.print(st.reconnectMs);
        CONSOLE.print(" scan="); CONSOLE.print(session.wantPageScan() ? 1 : 0);
        CONSOLE.print(" role="); CONSOLE.println(src.link().role() ? 's' : (src.link().linkState() >= BtLink::LINK_UP ? 'm' : '-'));
        CONSOLE.print("bt_hci ncmd="); CONSOLE.print(hci.ncmd());
        CONSOLE.print(" timeouts="); CONSOLE.print(hci.timeouts());
        CONSOLE.print(" starved="); CONSOLE.print(hci.starved());
        CONSOLE.print(" l2drop="); CONSOLE.print(src.l2().dropped());
        CONSOLE.print(" credmin="); CONSOLE.println(src.l2().creditsMin());
        CONSOLE.print("bt_mem idle_blocks="); CONSOLE.print(btout.idleBlocks());
        CONSOLE.print(" paused_blocks="); CONSOLE.println(btout.pausedBlocks());
    }
}
```
(The `bt_mem heap=/stack_free_min=` fields belong on acid_box, which has the loopstat instrument; bt_tone_test's `bt_mem` line carries the audio-node counters. Keep it to what this example can cheaply produce.)

- [ ] **Step 4: CMakeLists — remove M2_BT_CONNECT_RETRY, add M2_BT_RETRY_MS / M2_BT_SUPERVISION_MS**

Delete the `option(M2_BT_CONNECT_RETRY ...)` block (and its `add_definitions`). Add:
```cmake
# NEW-34 piece 2: the reconnect retry cadence (ms between lost-peer page attempts).  Default in the library
# is 10 s; the [lifecycle] gate builds with 3000 to keep the run short.
set(M2_BT_RETRY_MS "" CACHE STRING "BtSession retry cadence in ms (empty = library default 10000)")
if(NOT M2_BT_RETRY_MS STREQUAL "")
    add_definitions(-DM2_BT_RETRY_MS=${M2_BT_RETRY_MS})
endif()
# NEW-34 piece 2 bench knob: link supervision timeout in ms (0/empty = controller default ~20 s).
set(M2_BT_SUPERVISION_MS "" CACHE STRING "Write_Link_Supervision_Timeout in ms (empty = controller default)")
if(NOT M2_BT_SUPERVISION_MS STREQUAL "")
    add_definitions(-DM2_BT_SUPERVISION_MS=${M2_BT_SUPERVISION_MS})
endif()
```

- [ ] **Step 5: Update the card-absent gate `run_qemu.sh`** for the new lines

The card-absent path no longer prints `a2dp=connect_failed` (the session is not begun without HCI). Change the assertion:
```bash
grep -q "^a2dp=deferred (no HCI: card absent)[[:space:]]*$" "$OUT" || { echo "FAIL: expected a2dp=deferred with no card"; exit 1; }
```
Change the two heartbeat asserts to match the new `hb` line (which no longer ends in `n=`): the wait-loop token and the two `grep -q "^hb streaming=0 blocks=0 packets=0 drops=0 hw=0[[:space:]]*$"` (there is now one `hb` line per second with no `n=` suffix — wait for a SECOND one by counting). Simplest: wait for `bt_link links=0` to appear twice, and assert `hb streaming=0 ... hw=0`, `bt_link links=0 lost=0 ...`, and that no `streaming by=` line appears. Rewrite the tail of `run_qemu.sh`:
```bash
for _ in $(seq 1 120); do
    [ -f "$OUT" ] && [ "$(grep -c '^bt_link links=0 ' "$OUT" 2>/dev/null)" -ge 2 ] && break
    sleep 0.25
done
gate_reap $P; gate_require_capture "$OUT"; echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 BT tone test up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "^serial2=up_115200[[:space:]]*$" "$OUT" || { echo "FAIL: Serial2 never came up"; exit 1; }
grep -q "^hci_reset=timeout reason=no_response attempts=10 timeouts=10 framing=0 starved=0 qfull=0 late=0[[:space:]]*$" "$OUT" || { echo "FAIL: expected the Reset timeout BY NAME"; exit 1; }
for T in "^hci_version" "^bd_addr=" "^hci_buffer" "^streaming by="; do
    if grep -q "$T" "$OUT"; then echo "FAIL: reported '$T' with nothing on LPUART2"; exit 1; fi
done
grep -q "^a2dp=deferred (no HCI: card absent)[[:space:]]*$" "$OUT" || { echo "FAIL: expected a2dp=deferred with no card"; exit 1; }
grep -q "^hb streaming=0 blocks=0 packets=0 drops=0 hw=0[[:space:]]*$" "$OUT" || { echo "FAIL: no vacuous heartbeat"; exit 1; }
grep -q "^bt_link links=0 lost=0 " "$OUT" || { echo "FAIL: no bt_link health line"; exit 1; }
echo "PASS: A2DP session stays vacuous with no card (no stream, no link, health line present)"
```

- [ ] **Step 6: Build + run the card-absent gate**

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake >/tmp/c.log 2>&1 && cmake --build build >/tmp/b.log 2>&1 && ./run_qemu.sh 2>&1 | tail -8
```
Expected: `PASS: A2DP session stays vacuous with no card ...`.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/rt1170/evkb && git commit -m "audio: bt_tone_test onto BtSession + health line (NEW-34 piece 2)

The one-shot/M2_BT_CONNECT_RETRY connect becomes session.tick() with onStream/onAttempt callbacks around btout.begin()/end(); loop() prints bt_link/bt_hci/bt_mem health once a second. M2_BT_CONNECT_RETRY removed (retry is now the default); M2_BT_RETRY_MS / M2_BT_SUPERVISION_MS added; bonds_forgotten= prints the pre-wipe count. Card-absent gate updated for the new lines.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>" -- examples/audio/bt_tone_test/bt_tone_test.cpp examples/audio/bt_tone_test/CMakeLists.txt examples/audio/bt_tone_test/run_qemu.sh && git log --oneline -1
```

---

## Task 9: acid_box onto the session + health line

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/display/acid_box/acid_box.cpp`
- Modify: `~/Development/rt1170/evkb/examples/display/acid_box/CMakeLists.txt`

**Context:** Read acid_box's `M2_BT_OUT` regions (setup tail `~1262-1303`, loop `~1312-1360`, the BT globals `~336-358`, `idleUi` `~782-796`). This is a BENCH build (`-DM2_BT_OUT=ON`); the gated build is BT-OFF and byte-identical, so this task does not move the acid_box gate. `AudioOutputBluetooth` is externally clocked here (`setSelfClock(false)`). The `bt_mem heap=/stack_free_min=` fields go here because acid_box already has the flash-resident loopstat infrastructure and the ITCM budget accounting.

- [ ] **Step 1: Wire the session**

Add `#include <BtSession.h>` in the `M2_BT_OUT` include region. Add `static BtSession session(src);` beside `static A2dpSource src(...)`. Add the callbacks (acid_box uses `setSelfClock(false)`):
```cpp
static void onStreamCb(void *, bool streaming, uint8_t reason, BtSession::By by) {
    if (streaming) { btout.setSelfClock(false); btout.begin(src); s_btBegun = true;
        CONSOLE.print("bt_streaming by="); CONSOLE.print(by == BtSession::BY_INCOMING ? "incoming" : by == BtSession::BY_INQUIRY ? "inquiry" : "paged");
        CONSOLE.print(" bitpool="); CONSOLE.print(src.sbcParams().bitpool);
        CONSOLE.print(" media_mtu="); CONSOLE.println(src.mediaMtu());
    } else { btout.end(); s_btBegun = false; CONSOLE.print("bt_dropped reason=0x"); CONSOLE.println(reason, HEX); }
}
static void onAttemptCb(void *, A2dpSource::Result r, const char *pairedBy) {
    BondStoreEeprom::save(bonds);
    CONSOLE.print("a2dp="); CONSOLE.print(A2dpSource::resultName(r)); CONSOLE.print(" paired_by="); CONSOLE.println(pairedBy);
}
```

- [ ] **Step 2: Replace the setup BT tail** — after `src.setBonds(&bonds)` / the legacy-pin block, register the callbacks and begin the session (keeping `idleUi` only for the boot download, which already ran above):
```cpp
    session.onStream(onStreamCb, nullptr);
    session.onAttempt(onAttemptCb, nullptr);
    if (s_hciSt == Hci::OK) {
#if defined(M2_BT_TARGET_NAME)
        session.begin(&bonds, M2_BT_TARGET_NAME, s_aclNum, millis());
#else
        session.begin(&bonds, nullptr, s_aclNum, millis());
#endif
    }
```

- [ ] **Step 3: Replace the loop BT block** — remove the 5 s retry `static uint32_t lastTry` block (lines ~1319-1346) and the old `bt_hb` block, replacing with:
```cpp
#if defined(M2_BT_OUT)
    session.tick(millis());
    src.service();
    if (s_btBegun) btout.poll();
    {
        static uint32_t last = 0;
        if (millis() - last >= 1000) {
            last = millis();
            const BtSession::Stats &st = session.stats();
            CONSOLE.print("bt_hb blocks="); CONSOLE.print(btout.blocks());
            CONSOLE.print(" packets="); CONSOLE.print(btout.packets());
            CONSOLE.print(" drops="); CONSOLE.print(btout.drops());
            CONSOLE.print(" pcmdrops="); CONSOLE.print(btout.pcmDrops());
            CONSOLE.print(" hw="); CONSOLE.println(btout.queueHighWater());
            CONSOLE.print("bt_link links="); CONSOLE.print(st.links);
            CONSOLE.print(" lost="); CONSOLE.print(st.lost);
            CONSOLE.print(" reason=0x"); CONSOLE.print(st.lastReason, HEX);
            CONSOLE.print(" reconnect_ms="); CONSOLE.print(st.reconnectMs);
            CONSOLE.print(" scan="); CONSOLE.println(session.wantPageScan() ? 1 : 0);
            CONSOLE.print("bt_mem heap="); CONSOLE.print(btMemHeapUsed());
            CONSOLE.print(" stack_free_min="); CONSOLE.println(btMemStackFreeMin());
        }
    }
    LS_LAP(LS_PRINT);
#endif
```
Add the two memory helpers near the loopstat block (flash-resident, `LOOPSTAT_FN` if that macro is in scope, else plain). `heap` uses newlib `mallinfo`; the stack floor samples `MSP` against `_ebss`:
```cpp
#if defined(M2_BT_OUT)
#include <malloc.h>
extern "C" char *_sbrk(int);
extern unsigned long _ebss;
static uint32_t btMemHeapUsed() { struct mallinfo mi = mallinfo(); return (uint32_t)mi.uordblks; }
static uint32_t btMemStackFreeMin() {
    static uint32_t floor = 0xFFFFFFFF;
    register uint32_t sp __asm__("sp");
    uint32_t freeNow = sp - (uint32_t)&_ebss;      // stack grows down from DTCM top; _ebss is DTCM .bss end
    if (freeNow < floor) floor = freeNow;
    return floor;
}
#endif
```
(If `mallinfo`/`_sbrk` linkage is awkward on this core, fall back to `heap = 0` with a comment — the load-bearing soak metric is `stack_free_min` stability and `l2drop`/`starved`, and heap being flat is asserted on the bench, not in a gate. Prefer the real `mallinfo` if it links.)

- [ ] **Step 4: CMakeLists — add the new M2Radio TUs to the ITCM-relief routing**

The `M2_BT_OUT` linker-script rule lists each `libM2Radio*.a:<file>.cpp.obj(.text* .fastrun)` to route to flash. Add a line for `BtSession.cpp.obj` alongside the existing `BtLink.cpp.obj`/`A2dpSource.cpp.obj` entries. Also confirm `BondTable.cpp.obj` is already there (it is — from piece 1). Keep the `M2_BT_RETRY_MS`/`M2_BT_SUPERVISION_MS` options too (copy the two `set(...)`/`add_definitions` blocks from Task 8 into acid_box's CMakeLists under the `M2_BT_OUT` guard).

- [ ] **Step 5: Build the BT-OFF (default) image and confirm the loadable image is unchanged**

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake >/tmp/ab_cfg.log 2>&1 && cmake --build build >/tmp/ab_bld.log 2>&1 && arm-none-eabi-objcopy -O binary build/acid_box.elf /tmp/ab_new.bin 2>/dev/null || /Applications/ARM_10/bin/arm-none-eabi-objcopy -O binary build/acid_box.elf /tmp/ab_new.bin; ls -la /tmp/ab_new.bin
```
Expected: builds clean; the default build does not link `BtSession` (guarded by `M2_BT_OUT`), so its golden `0x25B30A96` is unaffected (the acid_box gate is run in Task 14's sweep — no per-task golden check needed here since the default path is untouched by construction).

- [ ] **Step 6: Build the BT bench image (confirms ITCM fits with BtSession added)**

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box && rm -rf build-bt && cmake -B build-bt -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DM2_BT_OUT=ON -DM2_BT_TARGET_NAME=Shokz >/tmp/abbt_cfg.log 2>&1 && cmake --build build-bt >/tmp/abbt_bld.log 2>&1 && echo "BT BUILT" && grep -i "region .* overflowed\|will not fit" /tmp/abbt_bld.log || echo "no overflow"
```
Expected: `BT BUILT` and `no overflow`. If ITCM overflows, the routing line for `BtSession.cpp.obj` was not added — fix Step 4.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/rt1170/evkb && git commit -m "display: acid_box onto BtSession + health line (NEW-34 piece 2)

The 5 s retry block becomes session.tick() with onStream/onAttempt around btout.begin()/end(); bt_hb gains bt_link/bt_mem (heap + min free stack) health; idleUi stays only for the boot download. BtSession.cpp added to the M2_BT_OUT ITCM-relief routing. Default (BT-OFF) image untouched.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>" -- examples/display/acid_box/acid_box.cpp examples/display/acid_box/CMakeLists.txt && git log --oneline -1
```

---

## Task 10: m2_hci_probe reconnect helper onto the session

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/networking/m2_hci_probe/m2_hci_probe.cpp`

**Context:** Read `probeReconnect()` and `rcConnect()` (`m2_hci_probe.cpp` ~755-843). The four reconnect phases call the OLD blocking `src.connect()` and `src.link().disconnect()`. Those signatures are gone. Re-home each phase onto the session/attempt: page a target, tick to a verdict, then disconnect. The gate's UART assertions (`reconnect_phase=N result=ok paired_by=...`) and the peer's tally are unchanged, so the printed lines must stay identical.

- [ ] **Step 1: Rewrite `rcConnect` to drive one attempt to a verdict**

`rcConnect(phase)` currently calls the blocking `src.connect(RC_TARGET, ...)`. Replace with a helper that runs ONE attempt via the attempt machine (not the whole session — the probe wants deterministic single connects, not the retry policy):
```cpp
static A2dpSource::Result rcConnect(int phase) {
    A2dpSource::Target t{}; t.kind = A2dpSource::Target::INQUIRY; t.nameFilter = RC_TARGET;   // inquiry+page, like the old connect()
    // but prefer the bonded page when a bond exists (the old connect walked bonds first):
    const Bond *b = bonds.count() ? &bonds.at(0) : nullptr;
    // (the probe's phases 2 and 4 rely on the STORED bond being paged, not an inquiry -- use the session's walk instead)
    // Simplest faithful reproduction: use a short-lived BtSession-less walk via A2dpSource by seeding one attempt
    // per bonded candidate exactly as BtSession does.  Since the probe already validates the walk via the peer tally,
    // drive it through a local mini-walk:
    A2dpSource::Result r = rcRunWalk();     // see Step 2
    CONSOLE.print("reconnect_phase="); CONSOLE.print(phase);
    CONSOLE.print(" result="); CONSOLE.print(A2dpSource::resultName(r));
    CONSOLE.print(" paired_by="); CONSOLE.println(src.link().pairedBy());
    return r;
}
```

- [ ] **Step 2: Add `rcRunWalk()` — the bonded walk the old connect() did, now over the attempt machine**

The old `A2dpSource::connect()` walked bonds MRU-first then inquiry. `BtSession::startBootWalk` now owns that logic, but the probe wants a synchronous "run one full walk to a verdict" call. Use a `BtSession` locally, begin it, and tick until it leaves CONNECTING to STREAMING or the walk exhausts:
```cpp
static BtSession rcSession(src);
static A2dpSource::Result rcRunWalk() {
    static bool begun = false;
    // (re)begin the session's boot walk for this phase; it pages bonds MRU-first then inquiry, exactly as before.
    rcSession.begin(&bonds, RC_TARGET, s_aclNum, millis());
    uint32_t t0 = millis();
    while (millis() - t0 < 25000) {
        rcSession.tick(millis()); src.service(); idleMs();
        if (rcSession.state() == BtSession::STREAMING) return A2dpSource::OK;
        // the walk failed if it fell to WAITING having tried everything (no card answers)
        if (rcSession.state() == BtSession::WAITING) break;
    }
    (void)begun;
    return src.result();          // the attempt's last result (PAIR_FAILED / CONNECT_FAILED / ...)
}
```
Then replace each `src.link().disconnect(nowMs, idleMs);` in `probeReconnect()` with a session-driven disconnect:
```cpp
static void rcDisconnect() {
    rcSession.disconnect();
    uint32_t t0 = millis();
    while (millis() - t0 < 5000 && src.link().handle() != 0) { rcSession.tick(millis()); src.service(); idleMs(); }
}
```
and call `rcDisconnect()` where the old `src.link().disconnect(...)` calls were. `probeReconnect()`'s prints (`bonds_boot=`, `bonds_reload=`, `reconnect_phase=`, `bonds_final=`, `reconnect=done`) are otherwise unchanged.

> **Design note for the implementer:** the probe's four phases each want a FRESH single walk with the retry policy inert (no lost-peer loop between phases). Using a `BtSession` and stopping at the first `WAITING` gives that. If `BtSession::begin` re-arms the boot walk cleanly on each call (it does — it sets `m_boot = true; startBootWalk()`), each `rcRunWalk()` is an independent walk. Verify the peer tally is byte-identical to piece 1's (`create_conns=4`, `key_ok=2`, etc.) after Task 11 — if the walk now pages differently (e.g. an extra page), reconcile by matching `BtSession`'s walk to the old `A2dpSource::connect` walk exactly (first candidate PAGE_ATTEMPTS, rest 1, then inquiry).

- [ ] **Step 3: Build the reconnect bench image**

```bash
cd ~/Development/rt1170/evkb/examples/networking/m2_hci_probe && rm -rf build-reconnect && cmake -B build-reconnect -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DM2_BT_CONNECT=ON -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_RECONNECT=ON >/tmp/rc_cfg.log 2>&1 && cmake --build build-reconnect >/tmp/rc_bld.log 2>&1 && echo BUILT
```
Expected: `BUILT`.

- [ ] **Step 4: Run the reconnect gate (it must still pass, tally unchanged)**

```bash
cd ~/Development/rt1170/evkb/examples/networking/m2_hci_probe && ./run_qemu_reconnect.sh 2>&1 | tail -15
```
Expected: `PASS`. The peer tally line `PEER-RECONNECT inquiries=1 create_conns=4 key_replies=3 key_ok=2 key_rejected=1 neg_replies=2 iocap_dances=2 notified=2 started_links=4` must match. If it differs, the walk changed — reconcile per the design note. If it passes, **recapture the transcript** (the example's output changed — new `bt_link` etc. are NOT printed by the probe, but the reconnect prints may have reordered):
```bash
cp build-reconnect/reconnect.uart transcript_qemu_reconnect.txt 2>/dev/null; git diff --stat transcript_qemu_reconnect.txt
```
(Only commit the transcript if it changed; the vacuity suite replays it, so it must match the current gate assertions.)

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb && git commit -m "networking: m2_hci_probe reconnect helper onto BtSession (NEW-34 piece 2)

The four reconnect phases drive a BtSession walk to a verdict + a session disconnect, replacing the removed blocking src.connect()/src.link().disconnect(). The gate's printed lines and the peer tally are unchanged.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>" -- examples/networking/m2_hci_probe/m2_hci_probe.cpp examples/networking/m2_hci_probe/transcript_qemu_reconnect.txt && git log --oneline -1
```

---

## Task 11: The fake peer `lifecycle` phase

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/networking/m2_hci_probe/hci_peer.py`

**Context:** Read `hci_peer.py` in full — the phase table (`LAST_OPCODE`, `DEADLINE`), `Peer.__init__`, `handle()` (Create_Connection/Disconnect/etc.), `handle_acl`/`handle_avdtp`/`handle_media`, and the main loop. The `lifecycle` phase must: (leg 1) run the existing avdtp+media acceptor to STREAMING then INJECT a Disconnection_Complete (reason 0x08); (leg 2) PAGE the host (Connection_Request), then the host Accepts and the peer drives AVDTP as INITIATOR toward the host's acceptor at bitpool 35, streams media, then drops (reason 0x13); (leg 3) send a Connection_Request from an UNKNOWN address (expect Reject 0x0F), then wait for the host's re-page and run the acceptor again to STREAMING at bitpool 53.

This phase reverses roles for leg 2 (the PEER is the A2DP initiator), which is new. It reuses the existing media validator (`handle_media`) and the acceptor SDP responder.

- [ ] **Step 1: Register the phase**

Add to `LAST_OPCODE`: `"lifecycle": 0x0413,`. Add to `DEADLINE`: `"lifecycle": 55,` (below qrun's 60 s). Add `phase_done` for it — the run completes when three links have streamed and the two drops happened:
```python
    if phase == "lifecycle":
        lc = peer.lc
        return lc["links_streamed"] >= 3 and lc["drops"] >= 2 and lc["rejected_unknown"] and not peer.avdtp["error"] and lc["errors"] == 0
```

- [ ] **Step 2: Add the lifecycle state in `Peer.__init__`**

```python
        self.lc = {"leg": 1, "links_streamed": 0, "drops": 0, "rejected_unknown": False, "errors": 0,
                   "handle": 0x0001, "scan_on": 0, "scan_off": 0, "accepts": 0, "media_lens": [],
                   "bitpool2": 0, "key": None, "did_incoming_page": False, "did_unknown": False}
```
Reuse `reset_link()` between legs (it already resets the L2CAP/AVDTP acceptor state). Model `handle` bumping per link like the reconnect phase.

- [ ] **Step 3: Extend `handle()` for lifecycle commands**

- `0x0C1A Write_Scan_Enable`: count `scan_on`/`scan_off` by the parameter (0x02 → on, 0x00 → off) and Command Complete. Add to the existing `0x0C01 or 0x0C56 or 0x0C1A` handler a lifecycle counter:
```python
        elif opcode == 0x0C1A and self.phase == "lifecycle":
            if params and params[0] & 0x02: self.lc["scan_on"] += 1
            else: self.lc["scan_off"] += 1
            self.log.append("PEER-SCAN-ENABLE 0x%02x" % (params[0] if params else 0)); self.send(cmd_complete(opcode, b"\x00"))
```
- `0x0409 Accept_Connection_Request`: the host Accepts our leg-2/leg-3 page. Command Complete + Connection_Complete (fresh handle), assert the role byte:
```python
        elif opcode == 0x0409:
            bd = params[:6]; role = params[6] if len(params) > 6 else 0xFF
            if role != 0x01: self.log.append("PEER-ACCEPT-BAD-ROLE 0x%02x" % role); self.lc["errors"] += 1
            self.lc["accepts"] += 1; self.lc["handle"] += 1; self.reset_link()
            self.log.append("PEER-ACCEPTED role=0x%02x handle=0x%04x" % (role, self.lc["handle"]))
            self.send(cmd_complete(opcode, b"\x00" + bd)); self.send(event(0x03, b"\x00" + struct.pack("<H", self.lc["handle"]) + bd + b"\x01\x00"), 0.05)
            self.peer_bd = bd
            # the host is slave; WE (the peer/master) now authenticate with the STORED key and drive AVDTP
            self.send(event(0x17, bd), 0.1)                                  # Link_Key_Request -> host replies with the stored key
```
- `0x040A Reject_Connection_Request`: leg 3's unknown-address rejection:
```python
        elif opcode == 0x040A:
            reason = params[6] if len(params) > 6 else 0xFF
            if reason == 0x0F: self.lc["rejected_unknown"] = True
            self.log.append("PEER-REJECTED reason=0x%02x" % reason)
            self.send(cmd_complete(opcode, b"\x00" + params[:6]))
```
- `0x040B Link_Key_Request_Reply` in lifecycle: verify the host's stored key matches the one we notified in leg 1, then Authentication_Complete + Encryption_Change, then (as master) drive AVDTP toward the host. Reuse the reconnect handler's key check pattern; on success, kick off the peer-as-initiator AVDTP (Step 4).

- [ ] **Step 4: Peer-as-initiator AVDTP for leg 2**

After encryption in leg 2, the peer opens an L2CAP AVDTP channel at the host and drives DISCOVER→GET_ALL_CAPABILITIES→SET_CONFIGURATION(bitpool 35)→OPEN→(media channel)→START, validating the host's acceptor answers, then streams a few media packets and validates them (reuse `handle_media`). This is substantial; structure it as a small initiator state machine in the peer keyed on the host's responses in `handle_avdtp` (which currently only handles the peer-as-acceptor case). Add a `self.lc_init` sub-state and, in `handle_avdtp`, when `self.phase == "lifecycle" and self.lc["leg"] == 2`, treat inbound AVDTP as RESPONSES to our commands and advance. On the host's START accept, set streaming and, after ~20 media packets received on the media channel at 83-byte frames (`self.lc["media_lens"]`), inject Disconnection_Complete reason 0x13 and advance to leg 3.

Add a helper to inject a media packet FROM the peer? No — in leg 2 the HOST is the A2DP source still (it streams the tone), the peer is the A2DP SINK receiving media. So the peer VALIDATES media as in `handle_media`, asserting 83-byte frames (bitpool 35). Set `self.lc["bitpool2"] = 35` and check each frame length == `4 + 8 + ceil((8 + 16*35)/8)` = 83. Reuse `handle_media` but parameterise the expected frame length (add `self.media["frame_bytes"]`, default 119, set 83 for leg 2).

- [ ] **Step 5: Drop injection + leg advance in the main loop**

In the main loop (after `peer.feed(d)` / `peer.flush()`), add lifecycle progression: when leg 1 reaches `>=20` media packets, inject the drop and advance:
```python
        if phase == "lifecycle":
            lc = peer.lc
            if lc["leg"] == 1 and peer.media["pkts"] >= 20 and lc["drops"] == 0:
                peer.send(event(0x05, b"\x00" + struct.pack("<H", lc["handle"]) + b"\x08"), 0.05)  # Disconnection_Complete reason 0x08
                lc["drops"] += 1; lc["links_streamed"] += 1; peer.reset_link(); peer.media = fresh_media(); lc["leg"] = 2
                # one second later, PAGE the host (Connection_Request from the bonded address)
                peer.send(event(0x04, DEVICES[0][0] + struct.pack("<I", DEVICES[0][1])[:3] + b"\x01"), 1.0)
            elif lc["leg"] == 2 and peer.media["pkts"] >= 20 and lc["drops"] == 1:
                peer.send(event(0x05, b"\x00" + struct.pack("<H", lc["handle"]) + b"\x13"), 0.05)  # drop reason 0x13
                lc["drops"] += 1; lc["links_streamed"] += 1; peer.reset_link(); peer.media = fresh_media(); lc["leg"] = 3
                # half a second later, an UNKNOWN-address Connection_Request (must be rejected 0x0F)
                peer.send(event(0x04, bytes.fromhex("0102030405DE") + b"\x00\x00\x00\x01"), 0.5)
                lc["did_unknown"] = True
            # leg 3: after the reject, the host RE-PAGES us; the existing avdtp acceptor handles it -> streams at 53
            elif lc["leg"] == 3 and peer.avdtp["started"] and lc["links_streamed"] == 2 and peer.media["pkts"] >= 20:
                lc["links_streamed"] += 1
```
(Provide `fresh_media()` returning the initial media dict; reset `peer.avdtp` between legs via `reset_link()`.)

- [ ] **Step 6: The tally line**

Add to the phase-summary section (near the reconnect summary):
```python
    if phase == "lifecycle":
        lc = peer.lc
        print("PEER-LIFECYCLE links=%d drops=%d accepts=%d rejects=%d scan_on=%d scan_off=%d bitpool2=%d"
              % (lc["links_streamed"], lc["drops"], lc["accepts"], 1 if lc["rejected_unknown"] else 0, lc["scan_on"], lc["scan_off"], lc["bitpool2"]))
```

- [ ] **Step 7: Smoke-test the peer against the built lifecycle image** (deferred to Task 12, which builds `build-lifecycle/` and wires the gate). Commit the peer now; it is exercised there.

```bash
cd ~/Development/rt1170/evkb && python3 -c "import ast; ast.parse(open('examples/networking/m2_hci_probe/hci_peer.py').read()); print('hci_peer.py parses')"
git commit -m "networking: hci_peer.py lifecycle phase (NEW-34 piece 2)

A three-leg phase for the [lifecycle] gate: leg 1 inquiry+SSP+media then an injected drop (0x08); leg 2 the peer pages the host, the host Accepts as slave and the peer drives AVDTP at bitpool 35 (83-byte frames prove config adoption), then drops (0x13); leg 3 an unknown-address Connection_Request must be Rejected 0x0F, then the host re-pages and streams at 53. PEER-LIFECYCLE tally + scan-enable/role/reject tripwires.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>" -- examples/networking/m2_hci_probe/hci_peer.py && git log --oneline -1
```

---

## Task 12: The `audio/bt_tone_test[lifecycle]` gate

**Files:**
- Create: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/run_qemu_lifecycle.sh`
- Create: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/transcript_qemu_lifecycle.txt`

**Context:** Model the new gate on `run_qemu_media.sh` (read it in full — the `build-media/` machinery, the `-serial unix:...,server` peer attach, the GATE_VACUITY reuse, the assertion ordering). The lifecycle gate owns `build-lifecycle/` configured `-DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_RETRY_MS=3000`, runs the `lifecycle` peer phase, and asserts the three legs + tripwires. Infrastructure verdicts (PEER-EOF/PEER-DEADLINE/Traceback) are checked AFTER the UART assertions (piece 1's ordering lesson).

- [ ] **Step 1: Write `run_qemu_lifecycle.sh`**

Copy `run_qemu_media.sh`, then change: `BUILD_DIR="$DIR/build-lifecycle"`; the configure adds `-DM2_BT_RETRY_MS=3000`; the peer phase is `lifecycle`; the wait-loop breaks on the last line the gate parses (`^bt_link links=3 ` in the UART AND `^PEER-LIFECYCLE ` in the peer file). Assertions, in order:
```bash
# --- UART positives (the firmware's own view) ---
grep -q "RT1176 BT tone test up" "$OUT"                     || fail "[lifecycle] banner missing"
grep -q "^bonds_boot=0" "$OUT"                              || fail "[lifecycle] store not empty at boot"
grep -q "^streaming by=inquiry " "$OUT"                     || fail "[lifecycle] leg 1 never streamed by inquiry"
grep -q "^bt_dropped reason=0x08 " "$OUT"                   || fail "[lifecycle] leg 1 drop (0x08) not seen"
grep -q "^streaming by=incoming bitpool=35 " "$OUT"         || fail "[lifecycle] leg 2 did not stream by incoming at the adopted bitpool 35"
grep -q "^bt_dropped reason=0x13 " "$OUT"                   || fail "[lifecycle] leg 2 drop (0x13) not seen"
grep -q "conn_req: .* -> reject(0x0F unknown)" "$OUT"       || fail "[lifecycle] unknown-address page was not rejected 0x0F"
grep -q "^streaming by=paged " "$OUT"                       || fail "[lifecycle] leg 3 re-page did not stream"
grep -qE "^bt_link links=3 lost=2 " "$OUT"                  || fail "[lifecycle] final health line wrong (expected links=3 lost=2)"
grep -qE "^bt_link .* reconnect_ms=[1-9]" "$OUT"            || fail "[lifecycle] reconnect_ms never populated"
# --- peer tripwires (values the firmware cannot invent) ---
if grep -q "PEER-ACL-BAD-HANDLE" "$RES";  then fail "[lifecycle] the host wrote ACL on a dead handle: $(grep -m1 PEER-ACL-BAD-HANDLE "$RES")"; fi
if grep -q "PEER-ACCEPT-BAD-ROLE" "$RES"; then fail "[lifecycle] Accept_Connection_Request used the wrong role: $(grep -m1 PEER-ACCEPT-BAD-ROLE "$RES")"; fi
grep -q "^PEER-LIFECYCLE links=3 drops=2 accepts=1 rejects=1 scan_on=3 scan_off=3 bitpool2=35" "$RES" || fail "[lifecycle] peer tally wrong: $(grep -m1 PEER-LIFECYCLE "$RES")"
# --- infrastructure LAST ---
if grep -q "^PEER-EOF" "$RES";      then fail "[lifecycle] QEMU closed the socket (infrastructure)"; fi
if grep -q "^PEER-DEADLINE" "$RES"; then fail "[lifecycle] the peer gave up at its deadline (sweep load?)"; fi
if grep -q "^Traceback (most recent call last)" "$RES"; then fail "[lifecycle] the fake controller crashed (peer bug)"; fi
echo "PASS: A2DP link lifecycle -- drop, re-page in both directions, config adoption, unknown-address reject; all three links streamed"
```
(`scan_on=3 scan_off=3`: page scan goes on at boot/after each drop and off at each link-up, across three links. Confirm the exact counts against the first green run and adjust the tally to what the implementation actually produces — the NUMBER is what the peer counts; pin it to the observed value and demonstrate a mutant moves it.)

- [ ] **Step 2: Build the lifecycle image + run the gate**

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test && rm -rf build-lifecycle && cmake -B build-lifecycle -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_RETRY_MS=3000 >/tmp/lc_cfg.log 2>&1 && cmake --build build-lifecycle >/tmp/lc_bld.log 2>&1 && chmod +x run_qemu_lifecycle.sh && ./run_qemu_lifecycle.sh 2>&1 | tail -25
```
Expected: iterate peer↔firmware until `PASS: A2DP link lifecycle ...`. This is the integration crucible — most cross-task mismatches (walk cadence, adoption, teardown timing) surface here. Budget real debugging time.

- [ ] **Step 3: Capture the transcript fixture**

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test && cp build-lifecycle/lifecycle.uart transcript_qemu_lifecycle.txt && head -5 transcript_qemu_lifecycle.txt
```
(Match the capture filename to whatever `run_qemu_lifecycle.sh` writes — mirror `run_qemu_media.sh`'s `OUT=`.)

- [ ] **Step 4: Demonstrate RED, at least six ways** (each: mutate, build, run, confirm the NAMED failure, revert, rebuild). Record the exact failing line in the gate header comment (as `run_qemu_media.sh` does). The six:
  1. Node not ended on drop (comment out `btout.end()` in `onStreamCb`'s else) → media continues on a dead handle → `PEER-ACL-BAD-HANDLE`.
  2. Acceptor streams our config not the peer's (force `sbcParams()` to the initiator default) → `bitpool=35` line missing / `bitpool2` wrong.
  3. Accept with role 0x00 (mutate BtLink's Accept role byte) → `PEER-ACCEPT-BAD-ROLE`.
  4. Retry not cancelled on incoming (remove the inboundUp branch in BtSession::tick) → an extra Create_Connection → tally `accepts`/scan counts move, or a bad-handle write.
  5. Unknown address accepted (mutate BtLink to accept it) → `conn_req: ... reject(0x0F unknown)` missing.
  6. Page scan never enabled (make `wantPageScan` a no-op) → `scan_on` count wrong.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb && chmod +x examples/audio/bt_tone_test/run_qemu_lifecycle.sh && git commit -m "audio: bt_tone_test[lifecycle] gate (NEW-34 piece 2)

Three legs against the fake peer: inquiry+SSP+media then a peer-injected drop (0x08); the peer pages us, we Accept as slave and adopt its bitpool-35 config (83-byte media frames prove it), then it drops (0x13); an unknown-address page is Rejected 0x0F and we re-page and stream at 53. Tripwires: ACL on a dead handle, Accept role byte, scan-enable sequence, the peer tally. Demonstrated RED six ways (in the gate header).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>" -- examples/audio/bt_tone_test/run_qemu_lifecycle.sh examples/audio/bt_tone_test/transcript_qemu_lifecycle.txt && git log --oneline -1
```

---

## Task 13: Vacuity negatives + recapture the media transcript

**Files:**
- Modify: `~/Development/rt1170/evkb/tools/gate-vacuity.test.sh`
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/transcript_qemu_media.txt` (if it exists; else create from the media gate)

**Context:** Read `tools/gate-vacuity.test.sh`'s media section (item 10) and its `run_gate`/`report`/`GATE_VACUITY` machinery. The vacuity suite replays each gate's committed `transcript_qemu*.txt` through a fake QEMU (`REAL_QEMU` hook) and asserts the gate FAILS by name on a doctored/absent capture. Add `[lifecycle]` negatives and confirm the `[media]` fixture is fresh (the example's `hb`/`bt_link` lines changed in Task 8, so a committed `transcript_qemu_media.txt` may now be stale).

- [ ] **Step 1: Recapture the media transcript if it moved**

Rebuild `build-media/` (Task 8 changed the example's loop output) and re-run the media gate; if it passes, refresh the fixture:
```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test && rm -rf build-media && cmake -B build-media -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 >/tmp/m_cfg.log 2>&1 && cmake --build build-media >/tmp/m_bld.log 2>&1 && ./run_qemu_media.sh 2>&1 | tail -5
# if PASS and a committed fixture exists, refresh it:
[ -f transcript_qemu_media.txt ] && cp build-media/media.uart transcript_qemu_media.txt; git diff --stat examples/audio/bt_tone_test/transcript_qemu_media.txt 2>/dev/null || true
```
(If `run_qemu_media.sh`'s assertions themselves reference lines that Task 8 removed — e.g. an `hb ... n=` pattern — update those assertions too. The `[media]` gate's `streaming`/`a2dp=ok` semantics are unchanged, but the heartbeat format is; reconcile.)

- [ ] **Step 2: Add the `[lifecycle]` vacuity cases**

After item 10 (the media block), add an item 11 modelled on it. Four negatives:
```bash
# --- 11. bt_tone_test[lifecycle] (NEW-34 piece 2) ---------------------------
bt_lc_elf="$EVKB/$bt_rel/build-lifecycle/bt_tone_test.elf"
if [ ! -x "$bt_lc_elf" ]; then
    echo "SKIP: lifecycle vacuity cases (no build-lifecycle/bt_tone_test.elf -- build it first)"
else
    # (a) absent capture -> the gate must fail on the missing banner/leg-1 stream, by name.
    cat > "$WORK/bt_lc_absent.txt" <<'ABSENT'
RT1176 BT tone test up
serial2=up_115200
hci_reset=timeout reason=no_response attempts=10 timeouts=10 framing=0 starved=0 qfull=0 late=0
bonds_boot=0
a2dp=deferred (no HCI: card absent)
hb streaming=0 blocks=0 packets=0 drops=0 hw=0
bt_link links=0 lost=0 reason=0x00 by=none reconnect_ms=0 scan=0 role=-
ABSENT
    export GATE_VACUITY=1; run_gate "$bt_rel" "run_qemu_lifecycle.sh" "$WORK/bt_lc_absent.txt"; rc=$?; unset GATE_VACUITY
    result=0; [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "\[lifecycle\] leg 1 never streamed by inquiry" || result=1
    report "absent_capture_fails_lifecycle_gate" $result
    # (b) a capture with the streams but NO drop -> must fail on the 0x08 assertion.
    sed '/^bt_dropped/d' "$EVKB/$bt_rel/transcript_qemu_lifecycle.txt" > "$WORK/bt_lc_nodrop.txt"
    export GATE_VACUITY=1; run_gate "$bt_rel" "run_qemu_lifecycle.sh" "$WORK/bt_lc_nodrop.txt"; rc=$?; unset GATE_VACUITY
    result=0; [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "\[lifecycle\] leg 1 drop (0x08) not seen" || result=1
    report "nodrop_capture_fails_lifecycle_gate" $result
    # (c) a capture where leg 2 streams at bitpool 53 (config NOT adopted) -> fail on the bitpool-35 assertion.
    sed 's/^streaming by=incoming bitpool=35 /streaming by=incoming bitpool=53 /' "$EVKB/$bt_rel/transcript_qemu_lifecycle.txt" > "$WORK/bt_lc_wrongbp.txt"
    export GATE_VACUITY=1; run_gate "$bt_rel" "run_qemu_lifecycle.sh" "$WORK/bt_lc_wrongbp.txt"; rc=$?; unset GATE_VACUITY
    result=0; [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "\[lifecycle\] leg 2 did not stream by incoming at the adopted bitpool 35" || result=1
    report "wrongbitpool_capture_fails_lifecycle_gate" $result
    rm -f "$EVKB/$bt_rel"/build-lifecycle/lifecycle.uart "$EVKB/$bt_rel"/build-lifecycle/lifecycle.peer "$EVKB/$bt_rel"/build-lifecycle/lifecycle.dbg
fi
```
(For the `[lifecycle]` vacuity replays the peer must not actually run — like the other vacuity cases, `GATE_VACUITY=1` + a fixture replaces the live QEMU. But `run_qemu_lifecycle.sh` attaches a live peer via `-serial unix:...`. Check how the media gate handles this: item 10 uses `GATE_VACUITY=1` and a fixture with NO peer file, so the gate's peer-file (`$RES`) assertions must degrade to "not found → the UART assertion fires first". Ensure the lifecycle gate checks the UART positives BEFORE the peer tally, so a fixture-only replay fails on a UART line, not on a missing `$RES`. This is already the assertion order in Task 12.)

- [ ] **Step 3: Run the vacuity suite**

```bash
cd ~/Development/rt1170/evkb && ./tools/gate-vacuity.test.sh 2>&1 | tail -20
```
Expected: all cases PASS, including the three new `*_lifecycle_gate` ones. Count grows from 35 to 38.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb && git commit -m "tools: [lifecycle] vacuity negatives + refresh [media] fixture (NEW-34 piece 2)

gate-vacuity.test.sh gains three [lifecycle] cases (absent capture, no-drop capture, wrong-bitpool capture), each failing the gate by name. The [media] transcript is recaptured for the new heartbeat/health lines Task 8 added.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>" -- tools/gate-vacuity.test.sh examples/audio/bt_tone_test/transcript_qemu_media.txt && git log --oneline -1
```

---

## Task 14: Licence audit, sweep, pin bump, CLAUDE.md, memory

**Files:**
- Modify: `~/Development/rt1170/evkb/tools/license-audit.sh` (GATES list)
- Modify: `~/Development/rt1170/evkb/evkb.cmake` (M2Radio pin)
- Modify: `~/Development/rt1170/evkb/CLAUDE.md`
- Modify: `~/Development/rt1170/evkb/docs/superpowers/specs/2026-09-06-bt-link-lifecycle-design.md` (status)
- Modify: `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170-evkb/memory/new34-bt-reconnect.md` + `MEMORY.md`

**Context:** This task pushes M2Radio, bumps the pin, verifies the fresh-user path, runs the full sweep and the licence audit (never concurrently), and updates the count records. The `build-lifecycle` build directory needs a GATES entry (it links M2Radio, so the depfile audit must walk it).

- [ ] **Step 1: Push M2Radio and get the SHA**

```bash
cd ~/Development/M2Radio && git log --oneline -8 && git push origin master && git rev-parse HEAD
```
Expected: the six piece-2 commits (Tasks 1–6) pushed; note the full SHA.

- [ ] **Step 2: Bump the pin in `evkb.cmake`**

Replace the M2Radio SHA on the `teensy_declare_library(M2Radio ...)` line with the new full SHA. Append to that line's trailing comment: ` 2026-09-06: NEW-34 piece 2 -- L2cap allow-list/reset/nextInbound/creditsMin; Avdtp acceptor + MediaPacketizer variable frame length; BtLink non-blocking operation engine + incoming page/link-state/supervision; A2dpSource one-attempt-either-direction; BtSession lifecycle policy. bt_tone_test/[lifecycle], acid_box and m2_hci_probe[reconnect] all use it; will not build against an older pin (BtSession.h missing).`

- [ ] **Step 3: Verify the fresh-user path (FORCE_FETCH) by RUNNING the new gate on fetched source**

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test && rm -rf build-fetch && cmake -B build-fetch -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DEVKB_FORCE_FETCH=ON -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_RETRY_MS=3000 >/tmp/ff_cfg.log 2>&1 && grep -iE "clone|Already at requested ref" /tmp/ff_cfg.log | head -3 && cmake --build build-fetch >/tmp/ff_bld.log 2>&1 && ( ln -sfn build-fetch build-lifecycle-save 2>/dev/null; true ) && echo FETCH_BUILT
# run the lifecycle gate against the fetched-source ELF:
cp -r build-lifecycle build-lifecycle.bak 2>/dev/null; rm -rf build-lifecycle; cp -r build-fetch build-lifecycle && ./run_qemu_lifecycle.sh 2>&1 | tail -3; rm -rf build-lifecycle; mv build-lifecycle.bak build-lifecycle 2>/dev/null; rm -rf build-fetch build-lifecycle-save
```
Expected: the configure log shows a `git clone` of M2Radio at the new pin, the build succeeds, and the gate PASSES against the fetched source. (A configure proves the subdir resolves; only running the gate proves the fetched code behaves — piece 1's rule.)

- [ ] **Step 4: Add the `build-lifecycle` GATES entry to `license-audit.sh`**

In the `GATES=` list, after the `examples/audio/bt_tone_test:bt_tone_test` entry, add:
```
examples/audio/bt_tone_test/build-lifecycle:bt_tone_test \
```
(The plain `bt_tone_test:bt_tone_test` entry covers `build/`; `build-media`/`build-lifecycle` are separate build dirs that link M2Radio and need their own depfile walk. Check whether `build-media` already has an entry; if not and the audit was passing, it is because `build-media` is created only by its gate — add both `build-media` and `build-lifecycle` if the audit walks by build dir. Match the existing convention: piece 1 did NOT add a `build-reconnect` entry, so follow whatever the audit currently does for the other bench build dirs — inspect and match.)

- [ ] **Step 5: Build every gate-owning example, then run the full sweep**

First rebuild the three changed examples' default gate images and confirm freshness (the M2Radio change is behind a header, so an un-rebuilt ELF would sweep vacuously — piece-1/NEW-36 lesson):
```bash
cd ~/Development/rt1170/evkb
for d in examples/audio/bt_tone_test examples/networking/m2_hci_probe examples/display/acid_box; do
  ( cd "$d" && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake >/dev/null 2>&1 && cmake --build build >/dev/null 2>&1 && echo "built $d" )
done
# build the m2_hci_probe bench + bt_tone_test bench dirs the gates need:
( cd examples/networking/m2_hci_probe && for v in hci baud avdtp reconnect; do :; done )   # these own build-<v>/ dirs; build per each gate's own cmake line (see each run_qemu_*.sh header)
```
Then read `docs/KNOWN-BROKEN-GATES.md`, then sweep from a short-path symlink:
```bash
cd ~/Development/rt1170/evkb && cat docs/KNOWN-BROKEN-GATES.md | head -40
ln -sfn "$PWD" /tmp/lc && cd /tmp/lc && ./tools/run-all-qemu-gates.sh -l | tail -3
./tools/run-all-qemu-gates.sh 2>&1 | tail -20
```
Expected: `-l` reports **130** gates; the sweep reports `gates: 130 passed` (or `129 passed / 1 failed` if `cm4_audio_test` hits its nondeterministic red — re-run it idle to confirm). The load-sensitivity class (`bt_tone_test[media]`, `[lifecycle]`, `m2_hci_probe[hci]`, `m2_uap_lwip[uap]`, `m2_rx_demo[txaggr]`) may need idle re-runs — disposition each with evidence, never by weakening a gate.

- [ ] **Step 6: Run the licence audit (AFTER the sweep, never during)**

```bash
cd ~/Development/rt1170/evkb && LICENSE_AUDIT_EVKB="$PWD" ./tools/license-audit.sh 2>&1 | tail -5
```
Expected: `LICENSE-AUDIT: PASS`, with the `build-lifecycle` entry walked.

- [ ] **Step 7: Update `CLAUDE.md`**

Add a measured close-out paragraph at the top of the sweep-count section, in the house style (date, `gates: 130 passed`, `-l` reports 130, the one new gate named `audio/bt_tone_test[lifecycle]` with its three-leg description, the M2Radio pin, fresh-user verified by RUNNING the gate on the fetched ELF, `LICENSE-AUDIT: PASS`, vacuity 38/38). Update the "target is 130 passed / 129+1" line and the KNOWN-BROKEN gate arithmetic (129 → 130, +1 for `[lifecycle]`). Note the piece-2 architecture change: the BT link layer is now non-blocking (ticked from `loop()`), `M2_BT_CONNECT_RETRY` is gone, and `A2dpSource::connect()`/`BtLink::page()`'s blocking forms no longer exist.

- [ ] **Step 8: Update the spec status + memory**

Set the spec's Status line to `IMPLEMENTED and QEMU-gated 2026-09-06 (sweep 130). Silicon bench matrix (Task 15) PENDING.` Update `~/.claude/projects/.../memory/new34-bt-reconnect.md`: piece 2 implemented + gated, silicon pending; carry the durable lessons this piece produced (the non-blocking-rewrite pattern; the acceptor config-adoption proof-by-frame-length; the both-directions page collision handling). Update the `MEMORY.md` index line's hook.

- [ ] **Step 9: Commit**

```bash
cd ~/Development/rt1170/evkb && git commit -m "build+docs: NEW-34 piece 2 close-out -- pin bump, [lifecycle] gate (sweep 130), audit PASS

M2Radio pinned to the piece-2 SHA (fresh-user verified by running bt_tone_test[lifecycle] on the fetched ELF); build-lifecycle GATES entry; CLAUDE.md count 129->130; spec status IMPLEMENTED; memory updated. Silicon bench (Task 15) pending.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>" -- evkb.cmake tools/license-audit.sh CLAUDE.md docs/superpowers/specs/2026-09-06-bt-link-lifecycle-design.md && git log --oneline -1
```

- [ ] **Step 10: Finish the branch** — use the `superpowers:finishing-a-development-branch` skill (verify the host suites + the sweep pass, then present the merge/PR options). Do NOT merge to master without the user's choice.

---

## Task 15: Silicon bench matrix (user, on hardware)

**Files (evidence, written on the bench):**
- `~/Development/rt1170/evkb/examples/audio/bt_tone_test/transcript_hw_evkb.txt`
- `~/Development/rt1170/evkb/examples/display/acid_box/transcript_hw_evkb_bt.txt`

**Context:** These are silicon-only claims QEMU cannot make (a real headset, a real power cycle, a real range loss). This task is the user's bench work; the plan records the procedure and the acceptance so the evidence is unambiguous. Flash with LinkServer (VCOM detached during `flash load`, per CLAUDE.md), free-run with SW4, read the console with `tools/rt1170-console.py`.

- [ ] **R1 — range loss ×5 (Shokz OpenMove).** Stream `bt_tone_test` (or acid_box `-DM2_BT_OUT=ON`) to the Shokz; carry it out of range until `bt_dropped reason=0x08` (~20 s supervision), bring it back. Record `by=` and `reconnect_ms` for each of five cycles. **Answers the open question:** whether the Shokz pages its last source (`by=incoming`) or waits to be paged (`by=paged`).
- [ ] **R2 — power off/on ×5 (Shokz).** Expect `reason=0x13` or `0x16`, then resume; record `by`/`reconnect_ms`.
- [ ] **R3 — ESP32 sink reset ×5.** Expect `reason=0x08`, then `by=paged` within one retry cycle (the sink never pages).
- [ ] **R4 — board reset with the headset live and bonded.** Piece 1's boot page vs the headset's incoming page: ONE link, no duplicate handle, `by=` recorded.
- [ ] **R5 — 30 min soak with five induced drops.** Assert on the health line: `bt_mem heap=` flat, `stack_free_min=` stable, `bt_hci starved=0`, `l2drop` bounded, `pcmdrops=0` between drops, tone audible by ear after every recovery.
- [ ] **acid_box witness** (`-DM2_BT_OUT=ON -DACIDBOX_LOOPSTAT=1`): UI alive throughout a drop/recover cycle (`ACIDBOX_VSYNC timeouts=0`, `svc max_us` in the low ms — the 16.8 s blocking connect is gone), touch p95 unchanged, recovery audible.
- [ ] **Supervision knob:** repeat R1 with `-DM2_BT_SUPERVISION_MS=5000` on a link we paged; record detection time and any spurious drops.
- [ ] **Piece 1 runs A–E** fold in here (R1–R4 exercise the stored-key reconnect path); record them in `bt_tone_test/transcript_hw_evkb.txt` as the piece-1 plan's Task 15 asked.

After the bench: update the spec status to DONE with the measured `by`/`reconnect_ms` figures, update `CLAUDE.md` and the memory with what the Shokz actually does on range loss, and (if the branch was kept rather than merged) finish it.

---

## Self-review (completed by the plan author)

- **Spec coverage:** §2 architecture → Tasks 1–6; §3 BtLink → Tasks 3–4; §4 A2dpSource → Task 5; §5 Avdtp acceptor → Task 2; §6 L2cap/audio node → Tasks 1, 7; §7 BtSession → Task 6; §8 apps/health/knobs → Tasks 8–10; §9 testing/gate → Tasks 6, 12, 13; §10 bench → Task 15; §11 risks → Task 15; §12 non-goals honoured (AVCTP refused in Task 1, no AVRCP/flush/soak). All covered.
- **Type/name consistency:** `BtLink::Result` keeps piece-1 values (no `PENDING` in the enum — `busy()` is the running signal); `A2dpSource::Result` adds `LOST`/`STOPPED`; `BtSession::By`/`Stats`/`StreamFn`/`AttemptFn` are used identically in Tasks 6, 8, 9; `MediaPacketizer::begin(mtu, frameBytes)` default keeps every caller; `Avdtp::sbcConfig()`/`configChanged()`/`role()`/`adoptInbound()`/`reset()`/`mediaReady()`/`startSelf()` used consistently in Tasks 2, 5.
- **Health-line fields** are the same tokens the `[lifecycle]` gate greps (`streaming by=`, `bt_dropped reason=`, `bt_link links=/lost=/reconnect_ms=/scan=`) — Tasks 8 and 12 agree.
- **Frame-length arithmetic:** bitpool 35 joint/16/8 → 83 bytes; bitpool 53 → 119 bytes (checked against the A2DP §12.9 formula and `MediaPacketizer::FRAME_BYTES`). The gate proves adoption by the 83-byte media frames in leg 2.
- **No placeholders:** every code step shows the code or the exact transformation of a quoted existing body; every command has an expected result.
