# BT-3 Phase 4: `AudioOutputBluetooth` — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** stream the audio graph to a paired A2DP sink as an Audio-library output node, so a 1 kHz test tone is audible on the ESP32 sink over Bluetooth.

**Architecture:** Approach B — the audio ISR encodes each 128-sample block to one SBC frame and pushes it into a lock-free ring; the main loop drains the ring, RTP-frames batches up to the media MTU, and sends them on the AVDTP media channel. The ring/drop/RTP logic lives in a host-testable `MediaPacketizer` core, free of the Teensy `AudioStream` base; a reusable `A2dpSource` helper does the connect→pair→AVDTP-START bring-up proven on silicon in phase 2.

**Tech Stack:** C++11, the clean-room `M2Radio/bt` stack (Sbc/L2cap/BtLink/Sdp/Avdtp), the Teensy-derived Audio library, CMake + the two-gate rule (QEMU `run_qemu*.sh` + `hci_peer.py` fake controller, silicon on the EVKB + ESP32 sink).

**Spec:** `docs/superpowers/specs/2026-09-03-bt3-phase4-audiooutputbluetooth-design.md`

**Working directories:** the M2Radio library is the sibling repo `/Users/nicholasnewdigate/Development/M2Radio` (its host tests run via `CXX=c++ ./bt/test/run.sh` from that repo root). The example + gates are in `/Users/nicholasnewdigate/Development/rt1170/evkb`. Both may be a MAIN checkout rather than a worktree — confirm with `git -C <repo> rev-parse --abbrev-ref HEAD` before committing, and stage only named files (never `git add -A`; the checkout is shared).

**Commit trailer** on every commit: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`

---

## Task 1: `Rtp` — RTP + A2DP media payload header

**Files:**
- Create: `M2Radio/bt/Rtp.h`, `M2Radio/bt/Rtp.cpp`
- Create: `M2Radio/bt/test/rtp_test.cpp`

- [ ] **Step 1: Write the failing test** — `M2Radio/bt/test/rtp_test.cpp`

```cpp
#include "Rtp.h"
#include <stdio.h>
static int g_fails = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
int main() {
    uint8_t o[16];
    {   // 1. Header length + fixed fields: V=2/P=0/X=0/CC=0 -> 0x80; M=0|PT=96 -> 0x60.
        uint16_t n = Rtp::header(o, /*seq*/0x1234, /*ts*/0x00ABCDEF, /*frames*/5);
        CHECK(n == 13);
        CHECK(o[0] == 0x80);
        CHECK(o[1] == 96);                                   // 0x60, M bit clear
    }
    {   // 2. seq big-endian at [2..3], timestamp big-endian at [4..7].
        Rtp::header(o, 0x1234, 0x00ABCDEF, 5);
        CHECK(o[2] == 0x12 && o[3] == 0x34);
        CHECK(o[4] == 0x00 && o[5] == 0xAB && o[6] == 0xCD && o[7] == 0xEF);
    }
    {   // 3. SSRC big-endian at [8..11] = Rtp::SSRC.
        Rtp::header(o, 0, 0, 0);
        uint32_t ssrc = ((uint32_t)o[8] << 24) | ((uint32_t)o[9] << 16) | ((uint32_t)o[10] << 8) | o[11];
        CHECK(ssrc == Rtp::SSRC);
    }
    {   // 4. A2DP media payload header at [12]: no fragmentation (top 3 bits 0), frame count in low nibble.
        Rtp::header(o, 0, 0, 8);   CHECK(o[12] == 0x08);
        Rtp::header(o, 0, 0, 1);   CHECK(o[12] == 0x01);
        CHECK((o[12] & 0xE0) == 0);                          // F=S=L=0
    }
    printf("rtp_test: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
```

- [ ] **Step 2: Run it, expect a compile failure** — `cd ~/Development/M2Radio && CXX=c++ ./bt/test/run.sh`
Expected: fails to compile — `Rtp.h: No such file or directory`.
(`run.sh` loops `l2cap_test avdtp_test sbc_test`; it will pick up `rtp_test` after Step 5 wires it in. For this step the failure is the missing header.)

- [ ] **Step 3: `M2Radio/bt/Rtp.h`**

```cpp
// Rtp -- RTP v2 header (RFC 3550) + the A2DP v1.3 SBC media payload header.
// Pure framing: no I/O, no state.  MIT, clean-room from the specs.
#pragma once
#include <stdint.h>
struct Rtp {
    static const uint8_t  PAYLOAD_TYPE = 96;        // dynamic PT used for A2DP media
    static const uint32_t SSRC = 0x00000001u;       // fixed per build; the sink does not care
    static const uint16_t HEADER_LEN = 13;          // 12-byte RTP header + 1-byte A2DP payload header
    // Write HEADER_LEN bytes into out: the 12-byte RTP v2 header (big-endian seq,
    // timestamp, SSRC; V=2, no padding/extension/CSRC, marker clear, PT=96) then the
    // 1-byte SBC media payload header (A2DP v1.3 sec 4.3.4): not fragmented (F=S=L=0),
    // frameCount in the low nibble.  Returns HEADER_LEN.
    static uint16_t header(uint8_t *out, uint16_t seq, uint32_t timestamp, uint8_t frameCount);
};
```

- [ ] **Step 4: `M2Radio/bt/Rtp.cpp`**

```cpp
#include "Rtp.h"
uint16_t Rtp::header(uint8_t *o, uint16_t seq, uint32_t ts, uint8_t frameCount) {
    o[0]  = 0x80;                                    // V=2, P=0, X=0, CC=0
    o[1]  = PAYLOAD_TYPE;                            // M=0, PT=96
    o[2]  = (uint8_t)(seq >> 8);  o[3]  = (uint8_t)seq;
    o[4]  = (uint8_t)(ts >> 24);  o[5]  = (uint8_t)(ts >> 16);
    o[6]  = (uint8_t)(ts >> 8);   o[7]  = (uint8_t)ts;
    o[8]  = (uint8_t)(SSRC >> 24); o[9] = (uint8_t)(SSRC >> 16);
    o[10] = (uint8_t)(SSRC >> 8); o[11] = (uint8_t)SSRC;
    o[12] = (uint8_t)(frameCount & 0x0F);            // F=S=L=0, frame count (<=8 here)
    return HEADER_LEN;
}
```

- [ ] **Step 5: Wire `rtp_test` into the runner** — `M2Radio/bt/test/run.sh`, extend the test loop list.

Change the `for t in l2cap_test avdtp_test sbc_test; do` line to:
```sh
for t in l2cap_test avdtp_test sbc_test rtp_test mediapacketizer_test; do
```
(`mediapacketizer_test` is added in Task 2; `run.sh` skips a test whose `.cpp` is absent via its `[ -f "$DIR/$t.cpp" ] || continue` guard, so adding both names now is safe.)

- [ ] **Step 6: Run, expect pass** — `cd ~/Development/M2Radio && CXX=c++ ./bt/test/run.sh`
Expected tail includes `rtp_test: 8 checks, 0 failures` and `BT-HOST-TESTS: PASS`, `-Werror` clean.

- [ ] **Step 7: Commit** (M2Radio)
```sh
cd ~/Development/M2Radio && git add bt/Rtp.h bt/Rtp.cpp bt/test/rtp_test.cpp bt/test/run.sh
git commit -m "bt: Rtp -- RTP v2 + A2DP SBC media payload header (host-tested)"
```

---

## Task 2: `MediaPacketizer` — ring, drop-oldest, RTP batch (the host-testable core)

**Files:**
- Create: `M2Radio/bt/MediaPacketizer.h`, `M2Radio/bt/MediaPacketizer.cpp`
- Create: `M2Radio/bt/test/mediapacketizer_test.cpp`

- [ ] **Step 1: Write the failing test** — `M2Radio/bt/test/mediapacketizer_test.cpp`

```cpp
#include "MediaPacketizer.h"
#include "Rtp.h"
#include <stdio.h>
#include <string.h>
static int g_fails = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// A fake sink: records every packet; `accept` gates whether drain() may send.
struct FakeSink {
    static const int MAXP = 256;
    uint8_t pkt[MAXP][2048]; uint16_t len[MAXP]; int n = 0;
    bool accept = true;
    static bool send(void *ctx, const uint8_t *p, uint16_t l) {
        FakeSink *s = (FakeSink *)ctx;
        if (!s->accept || s->n >= MAXP) return false;
        memcpy(s->pkt[s->n], p, l); s->len[s->n] = l; s->n++;
        return true;
    }
};
// A 119-byte fake SBC frame whose first byte is the sync word; byte 4 carries `id`.
static void fakeFrame(uint8_t *f, uint8_t id) { memset(f, 0, 119); f[0] = 0x9C; f[4] = id; }

int main() {
    // MTU 1008 -> (1008 - 13) / 119 = 8 frames per packet.
    {   // 1. Batch to MTU: push 8 frames, drain -> exactly ONE packet of 13 + 8*119.
        MediaPacketizer pk; pk.begin(1008);
        uint8_t f[119];
        for (int i = 0; i < 8; i++) { fakeFrame(f, (uint8_t)i); pk.push(f, 119); }
        FakeSink s; pk.drain(FakeSink::send, &s);
        CHECK(s.n == 1);
        CHECK(s.len[0] == 13 + 8 * 119);
        CHECK(s.pkt[0][12] == 8);                            // frame count in the payload header
        CHECK(s.pkt[0][0] == 0x80 && s.pkt[0][1] == 96);     // it is a real RTP header
        CHECK(s.pkt[0][13] == 0x9C);                          // first SBC frame's sync survived
        CHECK(pk.packets() == 1 && pk.frames() == 8 && pk.drops() == 0);
    }
    {   // 2. Sequence increments and timestamp advances by 128 per frame across packets.
        MediaPacketizer pk; pk.begin(1008);
        uint8_t f[119];
        for (int i = 0; i < 10; i++) { fakeFrame(f, (uint8_t)i); pk.push(f, 119); }
        FakeSink s; pk.drain(FakeSink::send, &s);             // 10 frames -> 8 + 2
        CHECK(s.n == 2);
        uint16_t seq0 = (s.pkt[0][2] << 8) | s.pkt[0][3];
        uint16_t seq1 = (s.pkt[1][2] << 8) | s.pkt[1][3];
        CHECK(seq1 == (uint16_t)(seq0 + 1));
        uint32_t ts0 = ((uint32_t)s.pkt[0][4]<<24)|((uint32_t)s.pkt[0][5]<<16)|((uint32_t)s.pkt[0][6]<<8)|s.pkt[0][7];
        uint32_t ts1 = ((uint32_t)s.pkt[1][4]<<24)|((uint32_t)s.pkt[1][5]<<16)|((uint32_t)s.pkt[1][6]<<8)|s.pkt[1][7];
        CHECK(ts1 == ts0 + 8 * 128);                          // first packet carried 8 frames
        CHECK(s.pkt[1][12] == 2);                             // tail packet has 2 frames
    }
    {   // 3. Credit starvation: sink refuses -> nothing sent, frames retained, no drops yet.
        MediaPacketizer pk; pk.begin(1008);
        uint8_t f[119];
        for (int i = 0; i < 4; i++) { fakeFrame(f, (uint8_t)i); pk.push(f, 119); }
        FakeSink s; s.accept = false; pk.drain(FakeSink::send, &s);
        CHECK(s.n == 0 && pk.packets() == 0 && pk.drops() == 0);
        s.accept = true; pk.drain(FakeSink::send, &s);        // now it flushes
        CHECK(s.n == 1 && s.pkt[0][12] == 4);
    }
    {   // 4. Ring overflow drops the OLDEST: fill 64, push 4 more with the sink refusing,
        //    then drain -> exactly 64 frames delivered, drops == 4, and the FIRST four ids are gone.
        MediaPacketizer pk; pk.begin(1008);
        uint8_t f[119];
        for (int i = 0; i < 64 + 4; i++) { fakeFrame(f, (uint8_t)i); pk.push(f, 119); }
        CHECK(pk.drops() == 4);
        CHECK(pk.queueHighWater() == 64);
        FakeSink s; pk.drain(FakeSink::send, &s);
        int delivered = 0; for (int p = 0; p < s.n; p++) delivered += s.pkt[p][12];
        CHECK(delivered == 64);
        CHECK(s.pkt[0][13 + 4] == 4);                         // first surviving frame's id byte is 4, not 0
    }
    printf("mediapacketizer_test: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
```

- [ ] **Step 2: Run it, expect a compile failure** — `cd ~/Development/M2Radio && CXX=c++ ./bt/test/run.sh`
Expected: `MediaPacketizer.h: No such file or directory`.

- [ ] **Step 3: `M2Radio/bt/MediaPacketizer.h`**

```cpp
// MediaPacketizer -- the A2DP media send core: a fixed SPSC ring of whole SBC
// frames (producer = audio ISR, consumer = main loop), drop-OLDEST on overflow,
// and RTP-batched draining up to the media MTU.  No AudioStream, no L2cap, no
// Arduino: pure logic so it host-compiles and is tested like Sbc/L2cap/Avdtp.
// MIT.
#pragma once
#include <stdint.h>
#include "Rtp.h"
class MediaPacketizer {
public:
    static const uint8_t  RING = 65;        // 64 USABLE frames (~190 ms at 344 fps); one slot
                                            // is sacrificed so wr==rd means EMPTY and nextWr==rd
                                            // means FULL -- a lock-free SPSC ring with no count field.
    static const uint16_t FRAME_MAX = 128;  // an SBC frame is 119 B at bitpool 53; round up
    static const uint16_t PKT_MAX = Rtp::HEADER_LEN + 8 * FRAME_MAX;
    // send returns false when the sink is not ready (no L2CAP credit); the packetiser
    // keeps the frames for the next drain().
    typedef bool (*SendFn)(void *ctx, const uint8_t *pkt, uint16_t len);

    void begin(uint16_t mtu);
    // Producer side (call from the ISR).  Copies the frame; drops the OLDEST on a full ring.
    void push(const uint8_t *frame, uint16_t len);
    // Consumer side (call from the main loop).  Batches whole frames up to mtu into RTP
    // packets and sends each via SendFn until the ring is empty or SendFn refuses.
    void drain(SendFn send, void *ctx);

    uint32_t frames()        const { return m_frames; }
    uint32_t packets()       const { return m_packets; }
    uint32_t drops()         const { return m_drops; }
    uint8_t  queueHighWater() const { return m_hw; }
private:
    uint8_t  count() const;                 // frames currently queued
    uint8_t  m_buf[RING][FRAME_MAX]; uint16_t m_len[RING];
    volatile uint8_t m_wr = 0, m_rd = 0;    // SPSC indices, mod RING (RING must divide 256 -> use masking on 64? see .cpp)
    uint16_t m_mtu = 0, m_perPkt = 0;
    uint16_t m_seq = 0; uint32_t m_ts = 0;
    uint32_t m_frames = 0, m_packets = 0, m_drops = 0; uint8_t m_hw = 0;
};
```

- [ ] **Step 4: `M2Radio/bt/MediaPacketizer.cpp`**

```cpp
#include "MediaPacketizer.h"
#include <string.h>
// SPSC ring, one slot sacrificed: wr==rd is EMPTY, (wr+1)%RING==rd is FULL, so RING=65
// gives 64 usable frames.  Only push() writes m_wr (and, on overflow, advances m_rd to
// drop the oldest); only drain() writes m_rd on a successful send.  count() is read-only.
uint8_t MediaPacketizer::count() const {
    return (uint8_t)(((int)m_wr - (int)m_rd + RING) % RING);
}
void MediaPacketizer::begin(uint16_t mtu) {
    m_mtu = mtu;
    uint16_t avail = mtu > Rtp::HEADER_LEN ? mtu - Rtp::HEADER_LEN : 0;
    m_perPkt = avail / 119;                 // whole SBC frames (bitpool 53 = 119 B) per packet
    if (m_perPkt == 0) m_perPkt = 1;
    if (m_perPkt > 8)  m_perPkt = 8;        // A2DP frame-count nibble cap; and PKT_MAX sizing
    m_wr = m_rd = 0; m_seq = 0; m_ts = 0;
    m_frames = m_packets = m_drops = 0; m_hw = 0;
}
void MediaPacketizer::push(const uint8_t *frame, uint16_t len) {
    if (len > FRAME_MAX) len = FRAME_MAX;
    uint8_t nextWr = (uint8_t)((m_wr + 1) % RING);
    if (nextWr == m_rd) {                    // ring full -> drop OLDEST
        m_rd = (uint8_t)((m_rd + 1) % RING);
        m_drops++;
    }
    memcpy(m_buf[m_wr], frame, len); m_len[m_wr] = len;
    m_wr = nextWr;
    m_frames++;
    uint8_t q = count();
    if (q > m_hw) m_hw = q;
}
void MediaPacketizer::drain(SendFn send, void *ctx) {
    uint8_t pkt[PKT_MAX];
    while (m_wr != m_rd) {                   // frames available
        uint8_t rd = m_rd, n = 0; uint16_t off = Rtp::HEADER_LEN;
        // gather up to m_perPkt whole frames that still fit the MTU
        while (n < m_perPkt && rd != m_wr && off + m_len[rd] <= m_mtu) {
            memcpy(pkt + off, m_buf[rd], m_len[rd]); off += m_len[rd];
            rd = (uint8_t)((rd + 1) % RING); n++;
        }
        if (n == 0) break;                   // defensive: a frame larger than the MTU (cannot happen at bitpool 53)
        Rtp::header(pkt, m_seq, m_ts, n);
        if (!send(ctx, pkt, off)) return;    // sink refused -> keep frames, try next drain()
        m_rd = rd; m_seq++; m_ts += (uint32_t)n * 128; m_packets++;
    }
}
```

- [ ] **Step 5: Run, expect pass** — `cd ~/Development/M2Radio && CXX=c++ ./bt/test/run.sh`
Expected: `mediapacketizer_test: N checks, 0 failures`, `BT-HOST-TESTS: PASS`, `-Werror` clean. (Test 4 pushes 68 frames into the 64-usable ring → 64 delivered, `drops==4`, `queueHighWater()==64`; if any count is off, the ring FULL/EMPTY logic is wrong — fix the code, never the assertion.)

- [ ] **Step 6: Commit** (M2Radio)
```sh
cd ~/Development/M2Radio && git add bt/MediaPacketizer.h bt/MediaPacketizer.cpp bt/test/mediapacketizer_test.cpp
git commit -m "bt: MediaPacketizer -- SPSC ring + drop-oldest + RTP batch to MTU (host-tested)"
```

---

## Task 3: `A2dpSource` — the reusable bring-up helper

**Files:**
- Create: `M2Radio/bt/A2dpSource.h`, `M2Radio/bt/A2dpSource.cpp`
- Modify: `M2Radio/bt/test/run.sh` (link the new source into the test binaries so it host-COMPILES; it has no unit test — its behaviour is proven by the `[media]` gate in Task 6)

**Context:** this is the exact sequence `examples/networking/m2_hci_probe/m2_hci_probe.cpp`'s `probeConnect()` runs (read it for the reference), packaged as a reusable object. It owns a `BtLink`, an `L2cap`, and an `Avdtp`, and drives them.

- [ ] **Step 1: `M2Radio/bt/A2dpSource.h`**

```cpp
// A2dpSource -- A2DP source bring-up: inquiry-by-name -> Create_Connection -> pair
// (SSP, or legacy PIN) -> encrypt -> L2cap -> SDP (AVDTP version) -> Avdtp
// DISCOVER..START.  Exposes the ready media channel for AudioOutputBluetooth.
// This is the sequence proven on silicon in BT-3 phase 2, packaged for reuse.  MIT.
#pragma once
#include <stdint.h>
#include "Hci.h"
#include "HciIo.h"
#include "L2cap.h"
#include "BtLink.h"
#include "Sdp.h"
#include "Avdtp.h"
#include "Sbc.h"
class A2dpSource {
public:
    enum Result : uint8_t { OK = 0, CONNECT_FAILED, PAIR_FAILED, L2CAP_FAILED, AVDTP_FAILED };
    static const char *resultName(Result r);
    A2dpSource(Hci &hci, HciIo &io) : m_hci(hci), m_l2(io), m_link(hci) {}
    void setLog(BtLink::LogFn fn, void *ctx) { m_link.setLog(fn, ctx); }
    void setPin(const char *pin4) { m_link.setPin(pin4); }
    void setLegacyPin(bool v)     { m_link.setLegacyPin(v); }
    // Full bring-up.  now()=millis, idle()=pump+yield.  aclNum from Read_Buffer_Size.
    Result connect(const char *name, uint8_t aclNum, uint32_t (*now)(), void (*idle)());
    // Forward from the app's Hci handlers:
    void onEvent(uint8_t code, const uint8_t *p, uint8_t len) { m_link.onEvent(code,p,len); m_l2.onEvent(code,p,len); }
    void onAcl(uint16_t h, const uint8_t *d, uint16_t len)    { m_l2.onAcl(h, d, len); }
    // For AudioOutputBluetooth + poll():
    L2cap   &l2()        { return m_l2; }
    Avdtp   &avdtp()     { return m_avdtp; }
    uint16_t mediaCid()  { return m_avdtp.mediaRemoteCid(); }
    uint16_t mediaMtu()  { return m_avdtp.mediaMtu(); }
    bool     started()   { return m_avdtp.state() == Avdtp::STREAMING; }
    uint16_t avdtpVersion() const { return m_sdpVer; }
    // The negotiated config as Sbc::Params (streams at bitpool 53).
    const Sbc::Params &sbcParams() const { return m_params; }
private:
    static void onData(void *ctx, L2cap::Channel &ch, const uint8_t *p, uint16_t len);
    Hci   &m_hci;
    L2cap  m_l2;
    BtLink m_link;
    Avdtp  m_avdtp;
    volatile bool     m_sdpDone = false;
    volatile uint16_t m_sdpVer  = 0;
    Sbc::Params m_params = { Sbc::RATE_44100, Sbc::JOINT_STEREO, 16, 8, Sbc::LOUDNESS, 53 };
};
```

- [ ] **Step 2: `M2Radio/bt/A2dpSource.cpp`** — the body mirrors `probeConnect()` (read `m2_hci_probe.cpp:638-705`), minus the console prints, using `m_*` members. The `connect()` sequence, verbatim in structure:

```cpp
#include "A2dpSource.h"
#include <string.h>
const char *A2dpSource::resultName(Result r) {
    switch (r) { case OK: return "ok"; case CONNECT_FAILED: return "connect_failed";
        case PAIR_FAILED: return "pair_failed"; case L2CAP_FAILED: return "l2cap_failed";
        case AVDTP_FAILED: return "avdtp_failed"; } return "?";
}
void A2dpSource::onData(void *ctx, L2cap::Channel &ch, const uint8_t *p, uint16_t len) {
    A2dpSource *s = (A2dpSource *)ctx;
    if (ch.psm == Avdtp::PSM && ch.localCid == 0x0041) s->m_avdtp.onSignalling(p, len);
    else if (ch.psm == Sdp::PSM) { s->m_sdpVer = Sdp::parseAvdtpVersion(p, len); s->m_sdpDone = true; }
}
A2dpSource::Result A2dpSource::connect(const char *name, uint8_t aclNum, uint32_t (*now)(), void (*idle)()) {
    if (m_link.connect(name, now, idle) != BtLink::OK) return CONNECT_FAILED;
    if (m_link.pairAndEncrypt(now, idle) != BtLink::OK) return PAIR_FAILED;
    m_l2.begin(m_link.handle(), aclNum);
    m_l2.acceptIncoming(true);
    m_l2.onData(onData, this);
    // (the app wires hci.onAcl -> a thunk that calls this->onAcl; see the example)
    // SDP (informational; failure here does not abort AVDTP)
    L2cap::Channel *sdp = m_l2.connect(Sdp::PSM, 0x0040);
    uint32_t t0 = now();
    if (sdp) while (sdp->state != L2cap::OPEN && now() - t0 < 5000) { m_l2.service(); idle(); }
    if (sdp && sdp->state == L2cap::OPEN) {
        uint8_t q[18]; m_l2.send(sdp->remoteCid, q, Sdp::buildAudioSinkPdlRequest(q, 1));
        m_sdpDone = false; t0 = now();
        while (!m_sdpDone && now() - t0 < 5000) { m_l2.service(); idle(); }
    }
    // AVDTP DISCOVER..START on the signalling channel 0x0041, media 0x0042
    L2cap::Channel *sig = m_l2.connect(Avdtp::PSM, 0x0041);
    if (!sig) return L2CAP_FAILED;
    t0 = now();
    while (sig->state != L2cap::OPEN && now() - t0 < 5000) { m_l2.service(); idle(); }
    if (sig->state != L2cap::OPEN) return L2CAP_FAILED;
    m_avdtp.begin(m_l2, 0x0041, 0x0042);
    Avdtp::SbcConfig want = { 44100, Avdtp::JOINT_STEREO, 16, 8, Avdtp::LOUDNESS, 2, 53 };
    m_avdtp.start(want); t0 = now();
    while (m_avdtp.state() != Avdtp::STREAMING && m_avdtp.state() != Avdtp::FAILED && now() - t0 < 15000) {
        m_l2.service(); m_avdtp.service(); idle();
    }
    return m_avdtp.state() == Avdtp::STREAMING ? OK : AVDTP_FAILED;
}
```

- [ ] **Step 3: Link it into the host test build** so it compiles — `M2Radio/bt/test/run.sh` already globs `"$DIR"/../*.cpp` into every test binary, so `A2dpSource.cpp` is picked up automatically. No edit needed; just confirm.

- [ ] **Step 4: Verify host-compile** — `cd ~/Development/M2Radio && CXX=c++ ./bt/test/run.sh`
Expected: still `BT-HOST-TESTS: PASS` (A2dpSource compiles clean under `-Werror` as part of every test link; it has no test of its own). If it fails to compile, fix the signature mismatches against the real `BtLink`/`L2cap`/`Avdtp`/`Sdp` headers.

- [ ] **Step 5: Commit** (M2Radio)
```sh
cd ~/Development/M2Radio && git add bt/A2dpSource.h bt/A2dpSource.cpp
git commit -m "bt: A2dpSource -- reusable A2DP-source bring-up (connect->pair->SDP->AVDTP START)"
```

---

## Task 4: `AudioOutputBluetooth` — the thin AudioStream node

**Files:**
- Create: `examples/audio/bt_tone_test/AudioOutputBluetooth.h`, `examples/audio/bt_tone_test/AudioOutputBluetooth.cpp`

**Placement note:** the node lives in the EXAMPLE for phase 4 (not the Audio library) to avoid an `Audio → M2Radio` cross-library dependency; the spec named `Audio/AudioOutputBluetooth` aspirationally. Promotion to a library is a phase-5 decision. It is ARM-only (extends `AudioStream`), so it is validated by the `[media]` gate + silicon, not a host test — its logic already lives in the host-tested `MediaPacketizer`.

- [ ] **Step 1: `AudioOutputBluetooth.h`**

```cpp
// AudioOutputBluetooth -- an AudioStream sink node that encodes its two inputs to
// SBC and streams them on an A2DP media channel via MediaPacketizer.  update() runs
// in the audio ISR (encode + enqueue only); poll() runs in the main loop (drain to
// L2CAP).  Nothing in the ISR touches the transport.  MIT.
#pragma once
#include "AudioStream.h"
#include "Sbc.h"
#include "MediaPacketizer.h"
#include "L2cap.h"
class A2dpSource;
class AudioOutputBluetooth : public AudioStream {
public:
    AudioOutputBluetooth() : AudioStream(2, inputQueueArray) {}
    void begin(L2cap &l2, uint16_t mediaCid, uint16_t mediaMtu, const Sbc::Params &p);
    void begin(A2dpSource &src);                 // sugar: pulls l2/cid/mtu/params from src
    virtual void update(void);                   // audio ISR: encode -> pk.push
    void poll();                                 // main loop: pk.drain -> l2.send
    bool     connected()      const { return m_l2 != nullptr && m_cid != 0; }
    uint32_t blocks()         const { return m_blocks; }
    uint32_t packets()        const { return m_pk.packets(); }
    uint32_t drops()          const { return m_pk.drops(); }
    uint8_t  queueHighWater() const { return m_pk.queueHighWater(); }
private:
    static bool sendThunk(void *ctx, const uint8_t *pkt, uint16_t len);
    audio_block_t *inputQueueArray[2];
    Sbc m_sbc; MediaPacketizer m_pk;
    L2cap *m_l2 = nullptr; uint16_t m_cid = 0;
    uint32_t m_blocks = 0;
};
```

- [ ] **Step 2: `AudioOutputBluetooth.cpp`**

```cpp
#include "AudioOutputBluetooth.h"
#include "A2dpSource.h"
void AudioOutputBluetooth::begin(L2cap &l2, uint16_t cid, uint16_t mtu, const Sbc::Params &p) {
    m_l2 = &l2; m_cid = cid; m_sbc.begin(p); m_pk.begin(mtu); m_blocks = 0;
}
void AudioOutputBluetooth::begin(A2dpSource &src) {
    begin(src.l2(), src.mediaCid(), src.mediaMtu(), src.sbcParams());
}
void AudioOutputBluetooth::update(void) {
    audio_block_t *l = receiveReadOnly(0), *r = receiveReadOnly(1);
    static const int16_t silence[AUDIO_BLOCK_SAMPLES] = {0};
    const int16_t *L = l ? l->data : silence;
    const int16_t *R = r ? r->data : silence;
    uint8_t frame[128]; uint16_t n = m_sbc.encode(L, R, frame);
    m_pk.push(frame, n); m_blocks++;
    if (l) release(l); if (r) release(r);
}
bool AudioOutputBluetooth::sendThunk(void *ctx, const uint8_t *pkt, uint16_t len) {
    AudioOutputBluetooth *o = (AudioOutputBluetooth *)ctx;
    return o->m_l2->send(o->m_cid, pkt, len);      // L2cap::send returns false when out of credit/txq
}
void AudioOutputBluetooth::poll() { if (m_l2) m_pk.drain(sendThunk, this); }
```

★ Check `AUDIO_BLOCK_SAMPLES == 128` in the Audio library (it is on this core); the SBC encoder consumes exactly 128 samples/channel per frame, matching one audio block. If the core's block size ever differs, the node needs to accumulate — out of scope here; assert it at `begin()`.
★ Confirm `L2cap::send(cid, buf, len)` returns `bool` (false = no credit/txq full) — Task 6's gate and the silicon run depend on `poll()` respecting that backpressure.

- [ ] **Step 3: Commit** (evkb) — deferred to Task 5's commit (the node has no standalone build; it is compiled by the example).

---

## Task 5: `bt_tone_test` example + card-absent gate

**Files:**
- Create: `examples/audio/bt_tone_test/bt_tone_test.cpp`
- Create: `examples/audio/bt_tone_test/CMakeLists.txt`
- Create: `examples/audio/bt_tone_test/run_qemu.sh`

- [ ] **Step 1: `CMakeLists.txt`** — model on `examples/networking/m2_hci_probe/CMakeLists.txt` (the firmware-blob + `M2_BT_*` knobs are identical) AND on an existing `examples/audio/*` `CMakeLists.txt` for the `import_evkb_library(Audio)` line. Import `M2Radio sdio iw416 hci bt` and `Audio`, compile `bt_tone_test.cpp AudioOutputBluetooth.cpp ${M2_FW_SRC} ${M2_BT_FW_SRC}`, and carry these knobs from `m2_hci_probe`'s CMakeLists verbatim: `M2RADIO_IW416_FW`, `M2RADIO_IW416_BT_FW`, `M2_BT_UART_DNLD`, `M2_BT_ASSERT_CTS`, `M2_BT_WAKE_PULSE`, `M2_BT_FAST_BAUD`/`M2_BT_FAST_BAUD_RATE`, `M2_BT_TARGET_NAME`, `M2_BT_LEGACY_PIN`. (Copy that file, delete the `M2_BT_CONNECT`/`M2_BT_LOOPBACK`/`M2_CONTINUITY_PROBE`/`M2_BT_INJECT_UART_CFG` blocks this example does not use.)

- [ ] **Step 2: `bt_tone_test.cpp`** — the graph + bring-up + loop:

```cpp
#include <Arduino.h>
#include <Audio.h>
#include "AudioOutputBluetooth.h"
#include "Hci.h"
#include "HciIo.h"
#include "A2dpSource.h"
// ... (firmware download + identity + baud-switch preamble: copy the proven setup()
//      body from m2_hci_probe.cpp up to and including the 3 Mbaud switch and the
//      s_aclNum capture from Read_Buffer_Size.  Reuse HciTransport hciIo(Serial2),
//      Hci hci(hciIo), HciPump pump, BtFwLoader.)
static A2dpSource src(hci, hciIo);
static AudioSynthWaveformSine tone;
static AudioOutputBluetooth  btout;
static AudioConnection pc0(tone, 0, btout, 0);
static AudioConnection pc1(tone, 0, btout, 1);        // same tone to L and R
static uint32_t nowMs() { return millis(); }
static void idleMs()   { pump.service(); delay(1); }
static void btLog(void *, const char *s) { CONSOLE.println(s); }
static void onEvt(void *, uint8_t c, const uint8_t *p, uint8_t l) { src.onEvent(c, p, l); }
static void onAcl(void *, uint16_t h, const uint8_t *d, uint16_t l) { src.onAcl(h, d, l); }
void setup() {
    // ... preamble: download, identity, baud to 3 M, capture s_aclNum ...
    AudioMemory(24);
    tone.frequency(1000); tone.amplitude(0.5f);
    hci.onEvent(onEvt, nullptr); hci.onAcl(onAcl, nullptr);
    src.setLog(btLog, nullptr); src.setPin("1234");
#if defined(M2_BT_LEGACY_PIN)
    src.setLegacyPin(true);
#endif
#if defined(M2_BT_TARGET_NAME)
    A2dpSource::Result r = src.connect(M2_BT_TARGET_NAME, s_aclNum, nowMs, idleMs);
#else
    A2dpSource::Result r = src.connect(nullptr, s_aclNum, nowMs, idleMs);
#endif
    CONSOLE.print("a2dp="); CONSOLE.println(A2dpSource::resultName(r));
    if (r == A2dpSource::OK) { btout.begin(src); CONSOLE.println("streaming"); }
}
void loop() {
    src.l2().service(); src.avdtp().service(); btout.poll();
    static uint32_t n = 0;
    CONSOLE.print("hb streaming="); CONSOLE.print(src.started() ? 1 : 0);
    CONSOLE.print(" blocks="); CONSOLE.print(btout.blocks());
    CONSOLE.print(" packets="); CONSOLE.print(btout.packets());
    CONSOLE.print(" drops="); CONSOLE.print(btout.drops());
    CONSOLE.print(" hw="); CONSOLE.print(btout.queueHighWater());
    CONSOLE.print(" n="); CONSOLE.println(n++);
    delay(1000);
}
```
(`CONSOLE` is `Serial1` on the imxrt1176 core — copy the `CONSOLE` alias + the whole download/identity/baud preamble from `m2_hci_probe.cpp` so this compiles and behaves identically up to bring-up.)

- [ ] **Step 3: `run_qemu.sh`** — the card-ABSENT fallback gate (model on `m2_hci_probe/run_qemu.sh`). Build the default image (synthetic BT firmware, no `M2_BT_TARGET_NAME`), boot on `mimxrt1170-evk` with no second `-serial`, and assert the streaming path is VACUOUS with no peer: `a2dp=` reports a non-OK result (no inquiry hit), `streaming` never prints, and the heartbeat shows `streaming=0 packets=0`. Assert the image heartbeats (`^hb streaming=0`). This is the only gate that runs on stock QEMU.

- [ ] **Step 4: Build + run the card-absent gate**
```sh
cd examples/audio/bt_tone_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
./run_qemu.sh
```
Expected: `PASS` (streaming vacuous with no card; image heartbeats).

- [ ] **Step 5: Commit** (evkb)
```sh
cd /Users/nicholasnewdigate/Development/rt1170/evkb
git add examples/audio/bt_tone_test/AudioOutputBluetooth.h examples/audio/bt_tone_test/AudioOutputBluetooth.cpp examples/audio/bt_tone_test/bt_tone_test.cpp examples/audio/bt_tone_test/CMakeLists.txt examples/audio/bt_tone_test/run_qemu.sh
git commit -m "audio/bt_tone_test: AudioOutputBluetooth node + tone example + card-absent gate"
```

---

## Task 6: `hci_peer.py` media receiver + the `[media]` gate

**Files:**
- Modify: `examples/networking/m2_hci_probe/hci_peer.py` (add a `media` phase reusing the `avdtp` acceptor + a media-channel receiver)
- Create: `examples/audio/bt_tone_test/run_qemu_media.sh`
- Modify: `tools/gate-vacuity.test.sh` (a fixture for the new gate)

- [ ] **Step 1: Extend `hci_peer.py` with a `media` phase.** It reuses the existing `avdtp` acceptor (connect/pair/L2CAP/SDP/AVDTP to START) and then, on the media channel (the peer's accepted channel for AVDTP media, PSM 0x0019, the SECOND such channel), receives RTP packets and validates them. Add, in the same style as the existing `handle_avdtp`:
  - track `media = {"pkts":0,"frames":0,"lastseq":None,"seqgaps":0,"badsbc":0,"badrtp":0}`
  - when an ACL frame arrives on the media L2CAP channel: parse RTP: `pkt[0]==0x80`, `pkt[1]==96`; seq = `pkt[2:4]` big-endian; check `lastseq is None or seq==(lastseq+1)&0xFFFF` else `seqgaps+=1`; frame_count = `pkt[12] & 0x0F`; walk `frame_count` SBC frames from offset 13, each must start with sync `0x9C` and be 119 bytes (bitpool 53) else `badsbc+=1`; `pkts+=1`, `frames+=frame_count`.
  - print machine-readable `PEER-MEDIA pkts=<> frames=<> seqgaps=<> badsbc=<> badrtp=<>` periodically and at `phase_done`.
  - `phase_done` for `media` returns `peer.media["pkts"] > 0 and peer.media["seqgaps"]==0 and peer.media["badsbc"]==0 and peer.media["badrtp"]==0`.

- [ ] **Step 2: `run_qemu_media.sh`** — model on `m2_hci_probe/run_qemu_avdtp.sh` (peer over a `/tmp` unix socket + own build dir). Differences:
  - own build dir `build-media/` with `-DM2_BT_CONNECT is NOT used here; instead -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_LEGACY_PIN=OFF` (the fake peer pairs by SSP) plus synthetic firmware (no blobs) — i.e. the example's default CONNECT-equivalent build. (bt_tone_test always attempts connect in setup(); the target name is the only knob it needs.)
  - launch `python3 <path>/m2_hci_probe/hci_peer.py media "$SOCK"` (reuse the one peer script by path).
  - wait token `^PEER-MEDIA` on `$RES` (the LAST line the peer prints), and `^hb streaming=1` on `$OUT`.
  - assertions:
    ```sh
    grep -q '^a2dp=ok' "$OUT"                          || fail "[media] bring-up did not reach AVDTP START"
    grep -q '^streaming' "$OUT"                        || fail "[media] node never began streaming"
    grep -qE '^hb streaming=1 .* drops=0 ' "$OUT"      || fail "[media] drops on the source side"
    grep -q '^PEER-MEDIA ' "$RES"                      || fail "[media] peer received no media"
    awk '/^PEER-MEDIA/{p=$0} END{exit !(p ~ /pkts=[1-9]/ && p ~ /seqgaps=0/ && p ~ /badsbc=0/ && p ~ /badrtp=0/)}' "$RES" \
                                                       || fail "[media] RTP/SBC framing invalid or sequence gapped"
    # frames received == blocks produced (allow the last in-flight block): |diff| <= perPkt
    echo "PASS: AudioOutputBluetooth RTP-frames SBC to the media channel; sequence continuous, framing valid, source drops=0"
    ```

- [ ] **Step 3: Run → PASS, then demonstrate RED** (regression-gate discipline; record both in the script header):
  - (a) break the sequence: in `MediaPacketizer::drain`, stop incrementing `m_seq` → expect `[media] RTP/SBC framing invalid or sequence gapped` (seqgaps). Revert.
  - (b) break the frame count: write `0` for the payload-header frame count (`Rtp::header(pkt, m_seq, m_ts, 0)`) → the peer walks 0 frames, `frames` stays 0 while `pkts>0`; assert it fails (add `frames=[1-9]` to the awk if needed). Revert.
  Rebuild `build-media/` from clean source between demos.

- [ ] **Step 4: Vacuity fixture** — in `tools/gate-vacuity.test.sh`, add a case driving `run_qemu_media.sh` against a card-absent capture; it must fail `[media] bring-up did not reach AVDTP START`. Run `./tools/gate-vacuity.test.sh | tail -3` — all OK.

- [ ] **Step 5: Sweep count** — `./tools/run-all-qemu-gates.sh -l | tail -1` increases by exactly 2 (`audio/bt_tone_test` card-absent + `audio/bt_tone_test[media]`). Record the new number.

- [ ] **Step 6: Commit** (evkb)
```sh
cd /Users/nicholasnewdigate/Development/rt1170/evkb
git add examples/networking/m2_hci_probe/hci_peer.py examples/audio/bt_tone_test/run_qemu_media.sh tools/gate-vacuity.test.sh
git commit -m "audio/bt_tone_test: [media] gate -- fake acceptor counts+validates RTP/SBC media (sweep +2)"
```

---

## Task 7: push/pin M2Radio, fresh-user verify, close-out

**Files:**
- Modify: `evkb.cmake` (M2Radio pin)

- [ ] **Step 1: Push M2Radio** — confirm `git -C ~/Development/M2Radio log --oneline origin/master..HEAD` is exactly the Task 1/2/3 commits (Rtp, MediaPacketizer, A2dpSource — all under `bt/`); `cd ~/Development/M2Radio && git push origin master`.
- [ ] **Step 2: Bump the pin** — in `evkb.cmake` replace the M2Radio SHA with the pushed HEAD, append to the comment: `BT-3 phase 4 adds bt/Rtp + bt/MediaPacketizer + bt/A2dpSource (AudioOutputBluetooth's transport core); audio/bt_tone_test is the first consumer.`
- [ ] **Step 3: Fresh-user verify** — `cd examples/audio/bt_tone_test && cmake -B build-fresh -DEVKB_FORCE_FETCH=ON -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build-fresh` → produces `bt_tone_test.elf` (links Rtp/MediaPacketizer/A2dpSource from the pinned fetch). Delete `build-fresh/`.
- [ ] **Step 4: Full sweep + audit** — reconfigure `bt_tone_test/build` gate-clean; `./tools/run-all-qemu-gates.sh` → the previous total + 2, 0 SKIP (the one permitted red is `dualcore/cm4_audio_test`); `LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh` → `PASS` (Rtp/MediaPacketizer/A2dpSource are MIT clean-room; the new example gets a manifest walk); `./tools/gate-vacuity.test.sh` all OK; `~/Development/M2Radio/bt/test/run.sh` PASS.
- [ ] **Step 5: Update `CLAUDE.md`** — bump the gate-count paragraph and the target line by +2, naming `audio/bt_tone_test` and `[media]` and what each proves (card-absent vacuity; RTP/SBC framing + sequence continuity + source drops=0 against the fake acceptor). Add a measured-sweep entry.
- [ ] **Step 6: Commit** (evkb)
```sh
git add evkb.cmake CLAUDE.md
git commit -m "evkb.cmake: pin M2Radio for BT-3 phase 4 (Rtp/MediaPacketizer/A2dpSource); CLAUDE.md sweep +2"
```

---

## Task 8: silicon acceptance (bench — deferred to a hardware session)

**Not automatable; run on the EVKB + ESP32 sink.** Build the bench image and stream to the sink:
```sh
cd examples/audio/bt_tone_test
FW=~/Development/mcuxsdk-ws/mcuxsdk/components/conn_fwloader/fw_bin/inc/IW416
cmake -B build-bench -DM2RADIO_IW416_FW=$FW/sdIW416_wlan.bin.inc -DM2RADIO_IW416_BT_FW=$FW/uartIW416_bt.bin.inc \
  -DM2_BT_UART_DNLD=ON -DM2_BT_ASSERT_CTS=ON -DM2_BT_FAST_BAUD=ON -DM2_BT_FAST_BAUD_RATE=3000000 \
  -DM2_BT_TARGET_NAME=EVKB-SINK -DM2_BT_LEGACY_PIN=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build-bench
```
Bench flow (from the phase-0/2 lessons): flash `LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build-bench/bt_tone_test.elf --erase-all` VCOM-free; reset the ESP32 sink so it is discoverable; arm BOTH readers (EVKB VCOM at 115200; ESP32 at 115200 with **DTR/RTS de-asserted** so the reader does not reset it); press SW4.

**Acceptance:**
- **1 kHz tone audible on the ESP32 sink's DAC.**
- Sink log: `a2dp_audio: state=started`, `pkts` climbing, `pcm_bytes_per_s ≈ 176400`, `dropped=0`.
- EVKB heartbeat: `streaming=1`, `packets` climbing, **`drops=0`**, and the queue high-water reported.
- **packets sent = packets received over 60 s** (compare the EVKB `packets` to the sink's received count).
- If `drops>0`: this is the flow-control-at-rate finding — lower the encode bitpool (Task: change the `Sbc::Params` bitpool in `A2dpSource`), re-measure; approach C is the fallback (a phase-5 decision). Record the bitpool that achieves `drops=0`.
- Append the capture to `examples/networking/m2_hci_probe/transcript_hw_evkb.txt` (or a new `examples/audio/bt_tone_test/transcript_hw_evkb.txt`).

---

## Notes for the implementer
- **Two-gate rule:** every capability gets a QEMU gate (Task 6) AND silicon (Task 8). A green QEMU sweep is necessary, not sufficient.
- **Never weaken a gate or a test to make it pass.** If test 4 in Task 2 is off by one, fix the ring size (the NOTE), not the assertion.
- **Clean-room:** `Rtp`/`MediaPacketizer` are from RFC 3550 + A2DP v1.3 §4 only; the SBC encoder is reused. Do not consult libsbc/bluedroid.
- **Stage only named files;** the checkouts are shared with other sessions.
