// Host tests for AudioInputBluetooth -- the A2DP sink's RTP/SBC receive path (NEW-41).
//
// WHY THIS EXISTS.  The QEMU gate (run_qemu.sh) drives this node through a REAL fake source and asserts the
// happy path end to end -- 150 well-formed packets, 750 frames, the level and the PCM golden.  What no gate in
// this tree can reach is the other half of the node: what it does with a MALFORMED packet (a source that is
// buggy, or an attacker on a paired link), what it does when the ring overflows, what it does when the source
// SUSPENDs, and what it does across begin()/end().  Those are pure functions of bytes in and counters out, so
// they belong here, where the packet can be built one byte at a time and the whole thing run under ASan/UBSan.
//
// WHAT IS REAL AND WHAT IS SHIMMED.  The code under test is the REAL AudioInputBluetooth.cpp and the REAL
// M2Radio bt/SbcDecoder.cpp + bt/Sbc.cpp -- every frame this file feeds the node was produced by the same
// encoder the bench and the gate use, never by a hand-written byte array.  Only AudioStream is shimmed
// (shim/AudioStream.h): a static block pool, and a transmit() that COPIES each block into a log this file
// asserts on.  __IMXRT1176__ is not defined, so audioPllTrimPpm() is the header's own no-op fallback and the
// servo's output is read through trimPpm() instead.
//
// THE LOAD-BEARING NEGATIVE is the 4-block refusal: AudioInputBluetooth::pushFrame checks
// `blocks * subbands == AUDIO_BLOCK_SAMPLES` because SbcDecoder writes blocks*8 samples per channel and reads
// the block count from the FRAME HEADER, not from the AVDTP configuration -- so a peer that negotiates 16
// blocks and then sends 4-block frames would leave 96 of the 128 samples in the ring slot STALE and we would
// transmit them as audio.  MUTATION-PROVEN: delete that check and case 9 fails by name (frames goes 0 -> 1).
// MIT.
#include "AudioInputBluetooth.h"
#include "AudioStream.h"
#include "Sbc.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <vector>

// Cases 8 and 9 hold the ring ABOVE target to wind the servo up; fill() saturates at RING - 1, so a
// configuration that cannot reach TARGET + 4 would feed for ever.  Measured: RING 32 / TARGET 30 hangs
// the suite with no output, which reads as a build that never ran rather than a configuration that is wrong.
static_assert(AudioInputBluetooth::TARGET + 4 <= AudioInputBluetooth::RING - 1,
    "node_test cases 8/9 need TARGET + 4 reachable within the ring");

static int g_fails = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---------------------------------------------------------------------------------------------------------
// A fresh node per case.  begin() resets the ring, the decoder and the servo but deliberately NOT the
// counters (they are the run's lifetime tally, which is what the heartbeat reports), so cases must not share
// one instance or every count would be read against the previous case's history.
// EVERY case here calls setPrefill(false) before begin(): they exercise the ring, the parser and the servo
// WITHOUT the START pre-fill (NEW-42), which will get its own cases in Task 3/4 of the plan.  With the pre-fill
// on, update() holds silence until TARGET blocks are buffered, and "feed one frame, update once, it comes out"
// would be false.
struct Node {
    AudioInputBluetooth *n;
    Node() : n(new AudioInputBluetooth()) { ShimAudio::reset(); }
    ~Node() { delete n; }
    AudioInputBluetooth *operator->() { return n; }
};

// --- real SBC frames, from the real encoder ---------------------------------------------------------------
// 44.1 kHz, joint stereo, 8 subbands, LOUDNESS, bitpool 53 -- the exact configuration the sink advertises and
// the gate's peer negotiates.  `blocks` is what goes in the frame HEADER.
static std::vector<uint8_t> encodeFrame(int16_t amp, int blocks = 16, int cycles = 3) {
    Sbc enc; Sbc::Params p = { Sbc::RATE_44100, Sbc::JOINT_STEREO, (uint8_t)blocks, 8, Sbc::LOUDNESS, 53 };
    enc.begin(p);
    int16_t l[AUDIO_BLOCK_SAMPLES], r[AUDIO_BLOCK_SAMPLES];
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        double v = amp * sin(2.0 * M_PI * cycles * i / AUDIO_BLOCK_SAMPLES);
        l[i] = r[i] = (int16_t)lrint(v);
    }
    std::vector<uint8_t> out(512);
    uint16_t n = enc.encode(l, r, out.data());
    // The encoder's analysis loop is fixed at 16 blocks, so a `blocks`-in-the-header frame carries a 16-block
    // BODY.  That is exactly the shape the refusal case needs -- a header the parser believes and a length the
    // parser derives from it -- and it is what a peer that lies about its block count looks like on the wire.
    out.resize(blocks == 16 ? n : Sbc::frameLength(p));
    return out;
}

// --- packet builders ---------------------------------------------------------------------------------------
// RTP: V2, no padding, no extension, no CSRC, PT 96 (A2DP's dynamic payload type), then the 1-byte SBC media
// header (A2DP v1.3 s4.3.4: F | S | L | number-of-frames).
static std::vector<uint8_t> rtp(uint16_t seq, uint8_t mediaHdr, const uint8_t *payload, size_t n) {
    std::vector<uint8_t> p;
    p.push_back(0x80); p.push_back(0x60);
    p.push_back((uint8_t)(seq >> 8)); p.push_back((uint8_t)seq);
    for (int i = 0; i < 4; i++) p.push_back(0);              // timestamp
    for (int i = 0; i < 4; i++) p.push_back(0);              // SSRC
    p.push_back(mediaHdr);
    p.insert(p.end(), payload, payload + n);
    return p;
}
static std::vector<uint8_t> onePacket(uint16_t seq, const std::vector<uint8_t> &frame) {
    return rtp(seq, 0x01, frame.data(), frame.size());
}
static void feed(Node &nd, const std::vector<uint8_t> &pkt) { nd->onMedia(pkt.data(), (uint16_t)pkt.size()); }

// The last block transmitted on channel `ch` (0 = left).
static const int16_t *lastTx(uint8_t ch) {
    for (int i = ShimAudio::logCount(); i-- > 0;) if (ShimAudio::logChan(i) == ch) return ShimAudio::logData(i);
    return nullptr;
}
static bool allZero(const int16_t *d) { for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) if (d[i]) return false; return true; }

int main() {
    {   // 1. ONE real frame, end to end.  The packet is accepted, the frame decodes into the ring, and one
        //    update() moves it out and transmits it on BOTH channels.  The level tie is the point: the mean
        //    |L| of the block the node TRANSMITTED must be exactly the rmsAcc()/rmsBlocks() it reported, or
        //    the heartbeat's `rms=` is measuring something other than the audio that came out.
        Node nd; nd->setPrefill(false); nd->begin();
        std::vector<uint8_t> f = encodeFrame(16384);
        CHECK(f.size() == 119);                              // 44.1/joint/16/8/53: 4 + 8 + (8 + 16*53 + 7)/8
        feed(nd, onePacket(1, f));
        CHECK(nd->pkts() == 1); CHECK(nd->frames() == 1); CHECK(nd->badFrames() == 0);
        CHECK(nd->fill() == 1);
        CHECK(nd->rmsBlocks() == 1);
        int32_t reported = (int32_t)(nd->rmsAcc() / nd->rmsBlocks());
        // The FIRST frame of a stream is quiet by construction, at both ends: the encoder's analysis window and
        // the decoder's synthesis history both start zero-filled, so the block ramps in.  Measured 4299 for a
        // 16384 sine here against 7744 in steady state (case 4 drains a full ring and sees the latter).  The band
        // is wide because the exact figure is the filterbank's business, not this test's -- what is asserted
        // exactly is the TIE below: whatever came out is what `rms=` reported.
        CHECK(reported > 3000 && reported < 6000);
        nd->update();
        CHECK(nd->fill() == 0);
        CHECK(ShimAudio::logCount() == 2);                   // left and right
        CHECK(ShimAudio::logChan(0) == 0 && ShimAudio::logChan(1) == 1);
        CHECK(ShimAudio::meanAbs(ShimAudio::logData(0)) == reported);
        CHECK(!allZero(ShimAudio::logData(1)));              // joint stereo: the right channel is real too
        CHECK(nd->underruns() == 0);
        CHECK(ShimAudio::outstanding() == 0);                // both blocks released
    }
    {   // 2. FIVE frames in one packet -- the shape a real source sends (the packetiser batches to fill the
        //    L2CAP MTU), and the loop that walks them is where an off-by-one costs a frame silently.
        Node nd; nd->setPrefill(false); nd->begin();
        std::vector<uint8_t> f = encodeFrame(12000), pl;
        for (int i = 0; i < 5; i++) pl.insert(pl.end(), f.begin(), f.end());
        std::vector<uint8_t> pkt = rtp(1, 0x05, pl.data(), pl.size());
        feed(nd, pkt);
        CHECK(nd->pkts() == 1); CHECK(nd->frames() == 5); CHECK(nd->badFrames() == 0); CHECK(nd->fill() == 5);
        for (int i = 0; i < 5; i++) nd->update();
        CHECK(nd->fill() == 0); CHECK(nd->underruns() == 0); CHECK(ShimAudio::logCount() == 10);
    }
    {   // 3. A SEQUENCE GAP is counted once and does not cost the packet: the frame in it is still decoded.
        //    (A gap means the air dropped a packet; refusing the next one would turn one loss into two.)
        Node nd; nd->setPrefill(false); nd->begin();
        std::vector<uint8_t> f = encodeFrame(9000);
        feed(nd, onePacket(100, f)); CHECK(nd->seqGaps() == 0);      // the first packet has nothing to follow
        feed(nd, onePacket(101, f)); CHECK(nd->seqGaps() == 0);
        feed(nd, onePacket(103, f)); CHECK(nd->seqGaps() == 1);      // 102 never arrived
        feed(nd, onePacket(104, f)); CHECK(nd->seqGaps() == 1);
        CHECK(nd->frames() == 4);
        CHECK(nd->pkts() == 4);
    }
    {   // 4. RING OVERRUN drops the NEW frame, never the oldest.  Dropping the oldest would mean the producer
        //    (onMedia, main context) writing m_tail, which the ISR consumer owns -- a real race.  The witness
        //    is which block comes out LAST: the frame that overruns is SILENCE and the RING - 1 before it are loud, so a
        //    drop-oldest ring would end the drain on silence.  fill saturates at RING - 1 (one slot is the SPSC sentinel).
        Node nd; nd->setPrefill(false); nd->begin();
        std::vector<uint8_t> loud = encodeFrame(16000), quiet = encodeFrame(0);
        for (int i = 0; i < AudioInputBluetooth::RING - 1; i++) feed(nd, onePacket((uint16_t)(i + 1), loud));
        CHECK(nd->fill() == AudioInputBluetooth::RING - 1); CHECK(nd->frames() == AudioInputBluetooth::RING - 1); CHECK(nd->overruns() == 0);
        feed(nd, onePacket(AudioInputBluetooth::RING, quiet));                  // the ring is full: this one is dropped
        CHECK(nd->overruns() == 1);
        CHECK(nd->fill() == AudioInputBluetooth::RING - 1);                     // unchanged
        CHECK(nd->frames() == AudioInputBluetooth::RING - 1);                   // it never became a frame
        for (int i = 0; i < AudioInputBluetooth::RING - 1; i++) nd->update();
        CHECK(nd->fill() == 0); CHECK(nd->underruns() == 0);
        const int16_t *last = lastTx(0); CHECK(last != nullptr);
        CHECK(last && ShimAudio::meanAbs(last) > 5000);              // the last block out is LOUD, not the dropped silence
    }
    {   // 5. A frame FRAGMENTED across three packets is reassembled and decoded exactly once.  F|S starts it,
        //    F alone continues, F|L ends it -- and nothing reaches the ring until the last one.
        Node nd; nd->setPrefill(false); nd->begin();
        std::vector<uint8_t> f = encodeFrame(14000);
        size_t a = 40, b = 40, c = f.size() - a - b;
        feed(nd, rtp(1, (uint8_t)(0x80 | 0x40 | 3), f.data(), a));           // F|S, 3 fragments to come
        CHECK(nd->frames() == 0); CHECK(nd->fill() == 0);
        feed(nd, rtp(2, (uint8_t)(0x80 | 2), f.data() + a, b));              // F, continuation
        CHECK(nd->frames() == 0);
        feed(nd, rtp(3, (uint8_t)(0x80 | 0x20 | 1), f.data() + a + b, c));   // F|L, the last one
        CHECK(nd->frames() == 1); CHECK(nd->fill() == 1); CHECK(nd->badFrames() == 0);
        CHECK(nd->pkts() == 3); CHECK(nd->seqGaps() == 0);
        nd->update();
        const int16_t *tx = lastTx(0); CHECK(tx != nullptr);
        // Real audio, not silence and not poison -- and a FIRST frame, so the ramp-in of case 1 applies (a
        // 14000 sine measures 3674 here against 6618 in steady state).
        CHECK(tx && ShimAudio::meanAbs(tx) > 2500);
        CHECK(tx && ShimAudio::meanAbs(tx) == (int32_t)(nd->rmsAcc() / nd->rmsBlocks()));
    }
    {   // 6. Joining MID-FRAGMENT -- a continuation with no START seen -- is refused and counted, and does not
        //    poison the next whole frame.  (We tuned in after the source began a fragmented frame; the bytes we
        //    hold are the tail of something we never saw the head of.)
        Node nd; nd->setPrefill(false); nd->begin();
        std::vector<uint8_t> f = encodeFrame(11000);
        feed(nd, rtp(1, 0x80, f.data() + 40, f.size() - 40));                // F, no S: mid-frame
        CHECK(nd->frames() == 0); CHECK(nd->badFrames() == 1); CHECK(nd->fill() == 0);
        feed(nd, onePacket(2, f));                                            // ... and the node still works
        CHECK(nd->frames() == 1); CHECK(nd->badFrames() == 1);
    }
    {   // 7. SEVEN HOSTILE PACKETS.  Each is a header a buggy or malicious source could put on the wire, and
        //    each one names a specific read this parser must NOT make past p+len.  Nothing may decode; the
        //    node must still be usable afterwards.  Run under ASan/UBSan (tests/run.sh adds the flags when the
        //    compiler is clang), which is what turns "frames==0" into "and it did not read out of bounds".
        Node nd; nd->setPrefill(false); nd->begin();
        std::vector<uint8_t> f = encodeFrame(10000);
        std::vector<uint8_t> h;
        // (a) CSRC count 15 in a 13-byte packet: the CSRC list alone claims 60 bytes past the header.
        h.assign(13, 0); h[0] = 0x8F; h[1] = 0x60; feed(nd, h);
        // (b) X bit with a 0xFFFF-word extension: the declared header runs 256 kB past a 20-byte packet.
        h.assign(20, 0); h[0] = 0x90; h[1] = 0x60; h[14] = 0xFF; h[15] = 0xFF; feed(nd, h);
        // (c) RTP version 0 -- not RTP at all.
        h.assign(32, 0); h[0] = 0x00; h[1] = 0x60; feed(nd, h);
        // (d) media header claiming 15 frames, with 8 bytes of zeroes behind it.
        h.assign(21, 0); h[0] = 0x80; h[1] = 0x60; h[12] = 0x0F; feed(nd, h);
        // (e) a REAL SBC header whose declared frame length runs past the packet (truncated frame).
        h = rtp(9, 0x01, f.data(), 20);  feed(nd, h);
        // (f) a runt: shorter than an RTP header plus one media-header byte.
        h.assign(12, 0); h[0] = 0x80; h[1] = 0x60; feed(nd, h);
        // (g) empty.
        nd->onMedia(nullptr, 0);
        CHECK(nd->frames() == 0);
        CHECK(nd->fill() == 0);
        CHECK(nd->rmsBlocks() == 0);
        nd->update();
        CHECK(nd->underruns() == 1);                                          // nothing to play: that IS an underrun
        const int16_t *tx = lastTx(0); CHECK(tx && allZero(tx));              // ... and silence came out, not poison
        feed(nd, onePacket(1, f));                                            // the node survived all seven
        CHECK(nd->frames() == 1);
    }
    {   // 8. end() -- the stream was lost or closed.  It must output silence, it must NOT count that silence as
        //    an underrun (nothing is missing: there is no stream), and it must HOLD the trim, so a stream that
        //    comes back starts from the rate this one had learned.  The trim is driven somewhere non-zero
        //    first, or "unchanged" would be satisfied by a servo that never moved.
        Node nd; nd->setPrefill(false); nd->begin();
        std::vector<uint8_t> f = encodeFrame(13000);
        uint16_t seq = 1;
        for (int i = 0; i < 1500; i++) {                                      // hold the ring ABOVE target: the servo winds up
            while (nd->fill() < AudioInputBluetooth::TARGET + 4) feed(nd, onePacket(seq++, f));
            nd->update();
        }
        int32_t trim = nd->trimPpm();
        CHECK(trim > 0);                                                      // a standing fill above TARGET speeds us up
        uint32_t under = nd->underruns();
        ShimAudio::reset();
        nd->end();
        CHECK(nd->fill() == 0);                                               // the ring is cleared
        for (int i = 0; i < 100; i++) nd->update();
        CHECK(nd->underruns() == under);                                      // not one of those hundred was an underrun
        CHECK(nd->trimPpm() == trim);                                         // and the trim is HELD
        CHECK(ShimAudio::logCount() == 200);
        bool silent = true;
        for (int i = 0; i < ShimAudio::logCount(); i++) if (!allZero(ShimAudio::logData(i))) silent = false;
        CHECK(silent);
        CHECK(ShimAudio::outstanding() == 0);
    }
    {   // 9. hold() -- the source SUSPENDed.  Same three properties as end() (silence, no underrun, trim
        //    frozen) but the stream is still LIVE: the ring is left alone, so a RESUME plays what is in it.
        //    Without this the servo would integrate fill=0 down to its -200 ppm clamp and `under` would climb
        //    at 344 Hz on a link doing exactly what it was told.
        Node nd; nd->setPrefill(false); nd->begin();
        std::vector<uint8_t> f = encodeFrame(15000);
        uint16_t seq = 1;
        for (int i = 0; i < 800; i++) { while (nd->fill() < AudioInputBluetooth::TARGET + 4) feed(nd, onePacket(seq++, f)); nd->update(); }
        int32_t trim = nd->trimPpm(); uint32_t under = nd->underruns(); uint8_t fill = nd->fill();
        CHECK(trim != 0);
        ShimAudio::reset();
        nd->hold(true);
        CHECK(nd->held());
        for (int i = 0; i < 400; i++) nd->update();                           // longer than the servo's 344-block tau
        CHECK(nd->underruns() == under);                                      // a suspended source is not an underrun
        CHECK(nd->trimPpm() == trim);                                         // ... and the servo did not step
        CHECK(nd->fill() == fill);                                            // the ring is untouched: RESUME plays it
        bool silent = true;
        for (int i = 0; i < ShimAudio::logCount(); i++) if (!allZero(ShimAudio::logData(i))) silent = false;
        CHECK(silent);
        nd->hold(false);                                                      // RESUME: the ring drains again
        ShimAudio::reset();
        nd->update();
        const int16_t *tx = lastTx(0); CHECK(tx && !allZero(tx));
        CHECK(nd->fill() == (uint8_t)(fill - 1));
    }
    {   // 10. THE LOAD-BEARING NEGATIVE.  A frame whose HEADER says 4 blocks is refused, because SbcDecoder
        //     writes blocks*8 samples per channel and takes the count from that header -- 32 of the slot's 128
        //     samples would be written and the other 96 left STALE from whatever the ring held before, and we
        //     would transmit them as audio.  Note what does NOT catch it: the CRC covers the header bytes, the
        //     join byte and the scale factors, all of which are identical to the 16-block frame's, so the
        //     decoder accepts it happily.  Only the blocks*subbands test in pushFrame refuses it.
        //     MUTATION-PROVEN: with that test deleted, frames() reads 1 and badFrames() 0.
        Node nd; nd->setPrefill(false); nd->begin();
        std::vector<uint8_t> ok = encodeFrame(12000), four = encodeFrame(12000, 4);
        feed(nd, onePacket(1, ok));
        CHECK(nd->frames() == 1); CHECK(nd->badFrames() == 0);
        Sbc::Params p;
        CHECK(SbcDecoder::parseHeader(four.data(), (uint16_t)four.size(), p));   // it IS a well-formed SBC header
        CHECK(p.blocks == 4 && p.subbands == 8);                                  // ... claiming 4 blocks
        feed(nd, onePacket(2, four));
        CHECK(nd->badFrames() == 1);                                              // RED with the check removed: 0
        CHECK(nd->frames() == 1);                                                 // RED with the check removed: 2
        CHECK(nd->fill() == 1);
    }
    {   // 14. HEADROOM (NEW-42).  A real source delivers EIGHT frames per RTP packet (iPhone bench 2026-09-09:
        //     frames/pkts = 7.97), so three back-to-back packets are 24 blocks.  The DEFAULT ring must swallow
        //     them from empty without a single drop; the bench's CONTROL arm (RING 16, built by run.sh with
        //     -DNODE_TEST_CONTROL_ARM) must drop exactly 24 - (RING - 1) = 9, which is the arithmetic that made
        //     the old ring overrun.  The expectation is computed from RING so the case is honest in both builds.
        //     RED at RING 16 in the default build: overruns() == 9, not 0.
        Node nd; nd->setPrefill(false); nd->begin();
        std::vector<uint8_t> f = encodeFrame(9000), pl;
        for (int i = 0; i < 8; i++) pl.insert(pl.end(), f.begin(), f.end());
        for (uint16_t s = 1; s <= 3; s++) feed(nd, rtp(s, 0x08, pl.data(), pl.size()));
        const uint32_t usable = AudioInputBluetooth::RING - 1;                   // one slot is the SPSC full/empty sentinel
        const uint32_t expect = usable >= 24 ? 0 : 24 - usable;
        CHECK(nd->overruns() == expect);
        CHECK(nd->frames() == 24 - expect);
#if !defined(NODE_TEST_CONTROL_ARM)
        CHECK(nd->overruns() == 0);                                              // the DEFAULT configuration must not drop
        // Spec s2/s5: at the OPERATING POINT an arriving 8-block packet must still find room, and one slot is
        // the SPSC sentinel.  `RING >= 32 && TARGET >= 16` does not say that -- 32/24 satisfies it with SEVEN
        // free slots (MEASURED: all 85 checks green), and 24 was the plan's own TARGET fallback until this
        // check refuted it.  It cannot be a header static_assert: the CONTROL arm is 8 + 8 > 15 by design.
        CHECK(AudioInputBluetooth::RING - 1 - AudioInputBluetooth::TARGET >= 8); // one packet of burst above
        CHECK(AudioInputBluetooth::TARGET >= 8);                                 // one packet of gap absorption below
#else
        CHECK(AudioInputBluetooth::RING == 16 && AudioInputBluetooth::TARGET == 8 && !AudioInputBluetooth::PREFILL);
#endif
    }
    printf("node_test: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
