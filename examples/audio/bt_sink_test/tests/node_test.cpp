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
// Case 11's fill walk is a fixed 9-block climb, so it carries a RING dependency the header's own bounds do not:
// RING 8 / TARGET 1 satisfies both static_asserts in the header and would fail case 11 with a bare
// `fillMax() == 9`, which names a ring size nowhere and reads as a broken instrument.
static_assert(AudioInputBluetooth::RING >= 10, "case 11's fill walk needs 9 blocks in the ring");
// Case 13 winds the servo up harder than cases 8/9 do -- it needs the FILTER more than 4 blocks off centre, and
// the servo samples fill AFTER the pop, so a ring held at TARGET + 6 converges the EMA to TARGET + 5.  Same
// hazard as the assert above: at TARGET + 6 > RING - 1 the `while (fill() < TARGET + 6)` feed loop never exits
// and the suite hangs with no output, which reads as a build that never ran.
static_assert(AudioInputBluetooth::TARGET + 6 <= AudioInputBluetooth::RING - 1,
    "node_test case 13 needs TARGET + 6 reachable within the ring");

static int g_fails = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---------------------------------------------------------------------------------------------------------
// A fresh node per case.  begin() resets the ring, the decoder and the servo but deliberately NOT the
// counters (they are the run's lifetime tally, which is what the heartbeat reports), so cases must not share
// one instance or every count would be read against the previous case's history.
// EVERY case here except 12 calls setPrefill(false) before begin(): they exercise the ring, the parser and the
// servo WITHOUT the START pre-fill (NEW-42), which is case 12's own subject.  With the pre-fill on, update()
// holds silence until TARGET blocks are buffered, and "feed one frame, update once, it comes out" would be
// false.  Case 12 turns it ON explicitly rather than relying on the build's PREFILL default, so that it runs
// in BOTH CMake arms -- the control arm compiles with BT_SINK_PREFILL=0.
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
    {   // 11. THE INSTRUMENT (NEW-42).  Inter-packet intervals land in the bucket their length names and the max
        //     is the MAX; the ISR-side fill/trim extremes are the extremes of a scripted sequence in BOTH
        //     directions; a RUN of dropped frames is ONE overrun event; a RECONNECT keeps every tally and
        //     restarts only the interval; and an AVDTP SUSPEND is not a delivery gap.  micros() is the shim's,
        //     so every interval is exact.
        Node nd; nd->setPrefill(false); nd->begin();
        std::vector<uint8_t> f = encodeFrame(8000);
        uint32_t t = 1000000; shimSetMicros(t); feed(nd, onePacket(1, f));
        for (uint8_t i = 0; i < AudioInputBluetooth::GAP_BUCKETS; i++) CHECK(nd->gapBucket(i) == 0);   // no predecessor yet
        // Bucket tops are INCLUSIVE, and ALL FOUR are now exercised exactly ON the top: 30000, 50000, 80000 and
        // 120000 are the tops of buckets 0, 1, 2 and 3.  RED with any one of those four compares turned into a
        // `<` -- that entry alone moves up a bucket.  80000 was MISSING until this review: MEASURED, mutating
        // only the third top left the entire suite green, so one of the four edges was decorative.
        // The TRAILING 15000 is what makes gapMaxUs a MAX rather than a last-writer-wins store.  With the
        // sequence ending at its own largest interval, `m_gapMaxUs = d` is indistinguishable from
        // `if (d > m_gapMaxUs) m_gapMaxUs = d` -- MEASURED: that mutant left the entire suite green, while it
        // makes `gapmax_ms` report the MOST RECENT gap, and gapmax_ms is the single number the bench and Task 7's
        // gate read as "the worst gap".
        const uint32_t gaps_us[] = { 10000, 30000, 25000, 45000, 50000, 70000, 80000, 100000, 120000, 130000, 15000 };
        uint16_t seq = 2;
        for (size_t i = 0; i < sizeof gaps_us / sizeof gaps_us[0]; i++) {
            nd->update(); t += gaps_us[i]; shimSetMicros(t); feed(nd, onePacket(seq++, f));
        }
        CHECK(nd->gapBucket(0) == 4);                                             // 10, 30 (on the top), 25, 15
        CHECK(nd->gapBucket(1) == 2);                                             // 45, 50 (on the top)
        CHECK(nd->gapBucket(2) == 2);                                             // 70, 80 (on the top)
        CHECK(nd->gapBucket(3) == 2);                                             // 100, 120 (on the top)
        CHECK(nd->gapBucket(4) == 1);                                             // 130, past the last top
        CHECK(nd->gapMaxUs() == 130000);                                          // ... and NOT the 15 ms that came after it
        // Give this node a non-zero OVERRUN tally too, so the survival check below is not merely "0 stayed 0".
        // micros() is left FROZEN across these feeds, so each is a 0 us interval in bucket 0 and neither the max
        // nor the >120 ms bucket moves.  The loop is bounded rather than `while (overEvents() == 0)`: a broken
        // ring must fail this case, not hang it.
        for (int i = 0; i < AudioInputBluetooth::RING + 2 && nd->overEvents() == 0; i++) feed(nd, onePacket(seq++, f));
        // A RECONNECT: begin() runs again.  The two FLAGS must reset -- the first packet of the new stream must
        // not be timed against the last packet of the old one -- while every TALLY and EXTREME must SURVIVE,
        // because they are printed beside m_over/m_pkts, which begin() has never reset.  All four survivors
        // checked here are NON-ZERO, so this is a real survival check and not "0 stayed 0".
        // RED with `m_haveRx = false` removed from begin(): the 5 s cross-stream interval lands in gbig, so
        // gapBucket(4) reads b4Before + 1 and gapMaxUs reads 5000000 instead of 130000.
        // RED with the tallies reset again in begin(): overEvents() reads 0 here, and gapMaxUs() 0.
        uint32_t gapMaxBefore = nd->gapMaxUs(), evBefore = nd->overEvents();
        uint32_t b0Before = nd->gapBucket(0), b4Before = nd->gapBucket(4);
        CHECK(evBefore == 1); CHECK(gapMaxBefore == 130000); CHECK(b4Before == 1); CHECK(b0Before > 4);
        nd->begin();
        CHECK(nd->overEvents() == evBefore);                                      // tallies SURVIVE a reconnect
        CHECK(nd->gapMaxUs() == gapMaxBefore);
        CHECK(nd->gapBucket(0) == b0Before);
        CHECK(nd->gapBucket(4) == b4Before);
        t += 5000000; shimSetMicros(t); feed(nd, onePacket(500, f));              // 5 s later: a new stream's first packet
        CHECK(nd->gapBucket(4) == b4Before);                                      // ... and it is NOT an interval
        CHECK(nd->gapBucket(0) == b0Before);
        t += 20000; shimSetMicros(t); feed(nd, onePacket(501, f));
        CHECK(nd->gapMaxUs() == gapMaxBefore);                                    // 20 ms did not raise the max
        CHECK(nd->gapBucket(4) == b4Before);
        CHECK(nd->gapBucket(0) == b0Before + 1);                                  // ... and the NEXT interval IS measured
        // Fill extremes are sampled by update() at the instant it POPS (pre-pop fill) and only then: a sample
        // taken on a dry block would pin fillMin at 0 forever and say nothing about margin.  The ring is walked
        // up to 9 and down to 0; the only extremes consistent with that walk are max 9 / min 1.
        // RED against extremes latched at their init values: fillMin() reads RING, fillMax() 0.
        Node n2; n2->setPrefill(false); n2->begin();
        uint16_t s2 = 1;
        for (int i = 0; i < 9; i++) feed(n2, onePacket(s2++, f));
        n2->update();                                                             // samples 9, pops -> 8
        for (int i = 0; i < 7; i++) n2->update();                                // samples 8..2, pops -> 1
        CHECK(n2->fillMax() == 9); CHECK(n2->fillMin() == 2);
        n2->update();                                                             // samples 1, pops -> 0
        CHECK(n2->fillMin() == 1);
        CHECK(n2->trimLo() <= 0 && n2->trimHi() >= 0 && n2->trimLo() <= n2->trimHi());
        // ... and the extremes must BRACKET the trim the servo actually reached, in BOTH directions.  The
        // ordering invariant above is satisfied by the INIT values on their own -- MEASURED: with the directional
        // checks below absent, deleting BOTH tracking lines from update() leaves every other check green.  Each
        // line needs its own arm, because the scripted sequence only ever drove the trim ONE way: the drain above
        // parks it BELOW zero (the servo samples fill AFTER the pop, so an emptied ring reads -12 at TARGET 16
        // and -4 at TARGET 8) and pins trimLo, while trimHi stayed at its init value of 0 and `if (t > m_trimHi)`
        // was UNPINNED -- MEASURED: deleting that line ALONE left the whole suite green.
        CHECK(n2->trimLo() < 0);                                                  // the DRAIN pins trimLo: RED with its tracking line removed
        CHECK(n2->trimLo() <= n2->trimPpm() && n2->trimHi() >= n2->trimPpm());
        // The WIND-UP is what pins trimHi: hold the ring ABOVE target until the servo's output goes positive,
        // the same idiom cases 8 and 9 use (and what the file-scope TARGET + 4 <= RING - 1 static_assert exists
        // to keep reachable).  Bounded, so a servo that cannot wind up fails this case rather than hanging it.
        for (int i = 0; i < 3000 && n2->trimPpm() <= 0; i++) {
            while (n2->fill() < AudioInputBluetooth::TARGET + 4) feed(n2, onePacket(s2++, f));
            n2->update();
        }
        CHECK(n2->trimPpm() > 0);                                                 // a standing fill above TARGET speeds us up
        CHECK(n2->trimHi() > 0);                                                  // the WIND-UP pins trimHi: RED with its tracking line removed
        CHECK(n2->trimLo() < 0);                                                  // ... and the low end is still the drain's
        CHECK(n2->trimLo() <= n2->trimPpm() && n2->trimHi() >= n2->trimPpm());
        // Overrun EVENTS: a full ring dropping eight in a row is over=8 overev=1; free one slot, land one, drop
        // one more -> overev=2.  RED with the run latch removed: overEvents() == 8.
        Node n3; n3->setPrefill(false); n3->begin();
        uint16_t s3 = 1;
        for (int i = 0; i < AudioInputBluetooth::RING - 1; i++) feed(n3, onePacket(s3++, f));
        CHECK(n3->overruns() == 0 && n3->overEvents() == 0);
        for (int i = 0; i < 8; i++) feed(n3, onePacket(s3++, f));
        CHECK(n3->overruns() == 8); CHECK(n3->overEvents() == 1);
        n3->update(); feed(n3, onePacket(s3++, f)); CHECK(n3->overruns() == 8);   // one slot freed, one frame landed
        feed(n3, onePacket(s3++, f));
        CHECK(n3->overruns() == 9); CHECK(n3->overEvents() == 2);
        // An AVDTP SUSPEND is NOT a delivery gap.  This file already applies that principle to the underrun
        // counter (case 9: "a suspended source is not an underrun" -- the source stopped on purpose); the gap
        // histogram is the same claim about the same silence, and it matters more, because spec s3 makes this
        // distribution the thing that SIZES TARGET and the iPhone bench run exercises pause/resume.  MEASURED
        // before hold() reset the timestamp: gapmax_ms=23 streaming became gapmax_ms=30023 across one 30 s
        // SUSPEND/RESUME.  RED with the `m_haveRx = false` removed from hold(): gapBucket(4) reads 1 and
        // gapMaxUs 30020000.
        Node n4; n4->setPrefill(false); n4->begin();
        uint32_t t4 = 2000000; shimSetMicros(t4); feed(n4, onePacket(1, f));
        t4 += 20000; shimSetMicros(t4); feed(n4, onePacket(2, f));
        CHECK(n4->gapBucket(0) == 1); CHECK(n4->gapMaxUs() == 20000);
        n4->hold(true);
        t4 += 30000000; shimSetMicros(t4);                                        // 30 s of SUSPEND, exactly the bench's
        n4->hold(false);
        feed(n4, onePacket(3, f));
        CHECK(n4->gapBucket(4) == 0);                                             // the pause is not a gap ...
        CHECK(n4->gapMaxUs() == 20000);
        CHECK(n4->pkts() == 3);                                                   // ... and the packet itself is still a packet
        t4 += 20000; shimSetMicros(t4); feed(n4, onePacket(4, f));
        CHECK(n4->gapBucket(0) == 2);                                             // the interval AFTER the resume IS measured
        CHECK(n4->gapMaxUs() == 20000);
    }
    {   // 12. PRE-FILL AT START (NEW-42, spec s2).  begin() used to start popping from an EMPTY ring -- the SAI
        //     ISR walks the graph from the moment the stream is configured, so update() runs long before the
        //     source's first packet can have landed, and the iPhone bench counted ~27 underruns in the first
        //     second because of it.  With the pre-fill on, update() transmits silence and counts NO underrun
        //     until TARGET blocks are buffered; the block that FINDS TARGET completes the prime AND pops, so the
        //     first block OUT is the first block DECODED and nothing that was buffered is discarded.
        //     DEMONSTRATED RED, each by name: the pre-fill never armed (`m_priming = false` in begin()) fails
        //     priming() and reads underruns()=8 below; a prime that DISCARDS its buffer (`m_head = m_tail = 0`
        //     at completion) fails meanAbs(first) > 3000; a prime whose completing block DEFERS its pop fails
        //     that and fill() == TARGET - 1.  ** The obvious `m_tail = m_head;` mutant is NOT one of them: the
        //     local `tail` is captured at the top of update(), so the pop branch reads m_ring[tail] and writes
        //     m_tail = tail + 1 over it, and the whole suite stays green.  Measured, not assumed. **
        Node nd; nd->setPrefill(true); nd->begin();
        CHECK(nd->priming()); CHECK(!nd->primed()); CHECK(nd->primeBlocks() == 0);
        // (a) THE DRY START -- the defect itself.  Eight blocks (23 ms) of graph walk before any packet arrives.
        for (int i = 0; i < 8; i++) {
            nd->update();
            CHECK(nd->priming());
            const int16_t *tx = lastTx(0); CHECK(tx && allZero(tx));
        }
        CHECK(nd->underruns() == 0);                   // RED with the pre-fill removed: 8
        CHECK(nd->primeBlocks() == 8);                 // RED with the m_primeBlocks++ removed: 0
        // The servo must not step while priming either: a priming ring is being filled deliberately, and
        // integrating that 0 -> TARGET ramp would trim the PITCH against a fill the servo did not cause.
        // Asserted HERE and not after the prime, because the recentre at completion HIDES it -- MEASURED with
        // `!m_priming` dropped from the servo's condition: this reads -14 (default arm) / -7 (control arm) and
        // is the ONLY failing check in either build, while the post-completion trimPpm() below stays 0.
        CHECK(nd->trimPpm() == 0);
        // (b) A SUSPEND DURING THE PRIME.  hold() takes precedence over priming exactly as it does over the
        //     underrun counter (case 9): a held source is not filling the ring, so those blocks are not prime
        //     time either and primeBlocks() must not inflate across a pause.
        //     ** WHAT THIS PINS, precisely: that a held block is not prime time (mutant: the priming branch
        //     hoisted out of `if (run)` -> primeBlocks() reads 13), that hold(true) does not ABANDON the prime,
        //     and that a held-and-priming block counts no underrun.  It does NOT pin the `if (run)` gate on the
        //     prime-COMPLETION check: the ring is EMPTY here, so that branch is never reached while held, and
        //     hoisting the completion out of `if (run)` leaves the whole suite green (measured).  Pinning it
        //     needs TARGET frames fed while held, which would disturb (c)'s loud-first construction; the
        //     behavioural delta is only WHEN servo_recentre() fires, so it is recorded rather than tested.
        nd->hold(true);
        for (int i = 0; i < 5; i++) nd->update();
        CHECK(nd->priming());                                                      // ... the prime is not abandoned
        CHECK(nd->primeBlocks() == 8);                                             // RED with the priming branch outside `if (run)`: 13
        CHECK(nd->underruns() == 0);
        nd->hold(false);
        // (c) FILLING.  The first frame in is LOUD and every later one silent, so the block that eventually
        //     comes out names which one it was -- and while the ring is part full the node must still transmit
        //     silence, not the audio it is holding back.
        std::vector<uint8_t> loud = encodeFrame(16000), quiet = encodeFrame(0);
        uint16_t seq = 1;
        feed(nd, onePacket(seq++, loud));                                          // the FIRST decoded block is loud
        for (int i = 1; i < AudioInputBluetooth::TARGET; i++) {                    // TARGET-1 silent periods, a packet each
            nd->update();
            CHECK(nd->priming());                                                  // the prime does not end early
            const int16_t *tx = lastTx(0); CHECK(tx && allZero(tx));               // ... and pops nothing while it runs
            feed(nd, onePacket(seq++, quiet));
        }
        CHECK(nd->fill() == AudioInputBluetooth::TARGET);
        const uint32_t primeLen = (uint32_t)(8 + AudioInputBluetooth::TARGET - 1);
        CHECK(nd->primeBlocks() == primeLen);
        ShimAudio::reset();
        nd->update();                                                              // finds TARGET: prime complete, block popped
        CHECK(!nd->priming()); CHECK(nd->primed());
        CHECK(nd->fill() == AudioInputBluetooth::TARGET - 1);                      // the completing block POPPED
        // A stream's FIRST frame decodes quiet by construction (the filterbank ramps in; case 1 measured 4299 for
        // a 16384 sine), so the bar is 3000: well above the quiet frame's 0, well below steady state.
        const int16_t *first = lastTx(0); CHECK(first && ShimAudio::meanAbs(first) > 3000);
        CHECK(nd->underruns() == 0);
        CHECK(nd->primeBlocks() == primeLen);                                      // latched: the completing block is not one of them
        // NOT reddenable on its own, and the comment must not pretend otherwise: with the recentre present this
        // is 0 whether or not the prime stepped the servo, and with the recentre DELETED it is still 0 because
        // nothing wound the filter -- only the double mutation moves it.  The load-bearing assertion is the
        // mid-prime trimPpm() above.  Kept as a postcondition of the pair, stated as one.
        CHECK(nd->trimPpm() == 0);
        // (d) ... and once PRIMED the underrun counter WORKS AGAIN.  A prime that never ended would silence it
        //     for the rest of the run -- the one shape of instrument failure that reads as good news.  There is
        //     exactly ONE dry block here, so these two checks read the same before and after Task 4's re-prime
        //     (which that block now also arms, and which (e)'s begin() then clears); case 13 is where the
        //     re-prime itself is pinned, including that a SECOND silent block would not be a second underrun.
        for (int i = 0; i < AudioInputBluetooth::TARGET; i++) nd->update();        // TARGET-1 pops, then one dry block
        CHECK(nd->fill() == 0);
        CHECK(nd->underruns() == 1);
        // (e) A RECONNECT re-primes.  begin() runs on EVERY stream start (bt_sink_test.cpp's onStreamCb) and the
        //     second stream starts from an empty ring exactly as the first did.  This is what pins begin()'s
        //     m_primed and m_primeBlocks resets: on a FRESH node both already read their initial values, so
        //     deleting either line leaves every check above green.
        nd->begin();
        CHECK(nd->priming()); CHECK(!nd->primed()); CHECK(nd->primeBlocks() == 0);
        CHECK(nd->fill() == 0);
        CHECK(nd->underruns() == 1);                                               // the tally is LIFETIME, like m_over
        nd->update();
        CHECK(nd->underruns() == 1);                                               // ... and the new stream's dry start adds none
        CHECK(nd->primeBlocks() == 1);
        // (f) end() -- the stream was lost or closed.  A dead stream is not priming, and priming() is an
        //     observable: leaving it set would report a prime running on a stream that no longer exists.
        nd->end();
        CHECK(!nd->priming());
        CHECK(ShimAudio::outstanding() == 0);
    }
    {   // 13. RE-PRIME ON A DRY RING (NEW-42, spec s2).  Mid-stream the source stops and the ring runs dry.  Exactly
        //     ONE underrun is counted, reprimes() goes 1, and the node holds silence -- uncounted, servo frozen --
        //     until TARGET blocks are back, then pops.  The filter is RECENTRED on the way back so the excursion
        //     does not drag the trim: the servo is wound far off-centre first, or "recentred" would be satisfied
        //     by a filter that never moved.  Without this a 45 ms gap left the ring 15 blocks short for the
        //     ~72 s it takes the trim to refill it (spec s1, fact 3).
        //     RED against: an underrun per silent block (underruns() climbs past 1); a resume below TARGET (pops
        //     at TARGET-1); the recentre removed -- MEASURED, the filter is still 4.3 blocks ABOVE target (4.7
        //     in the control arm), where a recentred one sits 0.003 BELOW it; the prime's
        //     `if (!m_primed)` guard removed (primeBlocks() stops being the START figure -- see below).
        Node nd; nd->setPrefill(true); nd->begin();
        std::vector<uint8_t> f = encodeFrame(12000);
        uint16_t seq = 1;
        // Spend three blocks in the START prime BEFORE any packet arrives, so primeBlocks() is NON-ZERO when the
        // re-prime later has to leave it alone.  Without this the wind-up loop's first update() would find the
        // ring already at TARGET + 6 and complete the prime in zero blocks, and every primeBlocks() check below
        // would be "0 stayed 0" -- satisfied by an implementation that counts nothing at all.
        for (int i = 0; i < 3; i++) nd->update();
        const uint32_t primeAtStart = nd->primeBlocks();
        CHECK(primeAtStart == 3);
        for (int i = 0; i < 1500; i++) { while (nd->fill() < AudioInputBluetooth::TARGET + 6) feed(nd, onePacket(seq++, f)); nd->update(); }
        CHECK(nd->primed() && !nd->priming());
        CHECK(nd->underruns() == 0 && nd->reprimes() == 0);
        CHECK(nd->servo().filt_x65536 - nd->servo().target_x65536 > 4 * 65536);   // wound up: >4 blocks above target
        while (nd->fill() > 0) nd->update();                                        // the source stops: drain to dry
        CHECK(nd->underruns() == 0);
        ShimAudio::reset();
        nd->update();                                                               // the dry block
        CHECK(nd->underruns() == 1); CHECK(nd->reprimes() == 1); CHECK(nd->priming());
        int32_t filtAtDry = nd->servo().filt_x65536;
        for (int i = 0; i < 50; i++) nd->update();                                  // silent, uncounted, servo frozen
        CHECK(nd->underruns() == 1);
        CHECK(nd->servo().filt_x65536 == filtAtDry);
        bool silent = true; for (int i = 0; i < ShimAudio::logCount(); i++) if (!allZero(ShimAudio::logData(i))) silent = false;
        CHECK(silent);
        for (int i = 1; i < AudioInputBluetooth::TARGET; i++) { feed(nd, onePacket(seq++, f)); nd->update(); CHECK(nd->priming()); }   // TARGET-1 is not enough
        CHECK(nd->fill() == AudioInputBluetooth::TARGET - 1);
        feed(nd, onePacket(seq++, f));                                              // TARGET
        ShimAudio::reset();
        nd->update();                                                               // resumes: pops
        CHECK(!nd->priming()); CHECK(nd->reprimes() == 1); CHECK(nd->underruns() == 1);
        CHECK(nd->fill() == AudioInputBluetooth::TARGET - 1);
        const int16_t *tx = lastTx(0); CHECK(tx && !allZero(tx));
        int32_t d = nd->servo().target_x65536 - nd->servo().filt_x65536;            // recentred, then ONE EMA step at TARGET-1
        CHECK(d >= 0 && d <= 65536 / 344 + 1);                                      // RED with the recentre removed: -4.26 blocks
        // ** THE primeBlocks QUESTION, ANSWERED: a mid-stream re-prime NEITHER EXTENDS NOR RE-TIMES primeBlocks().
        //    prime_ms is the START prime's length and nothing else. **  Three reasons, in order of weight.
        //    (1) Spec s5's acceptance is "ONE prime_ms ~ TARGET * 2.9 ms at START", read off a heartbeat sampled
        //        at the END of a 10-min window.  Extend it and the number is START plus every re-prime since;
        //        re-time it and the START figure is gone the first time the ring runs dry.  Under either the
        //        criterion cannot be checked at all -- and s5 also allows reprimes to be non-zero
        //        (`under <= reprimes + 2`), so "there will not be any" is not an available defence.
        //    (2) The header types it as a PER-STREAM DURATION, not a tally.  "Total blocks ever spent priming"
        //        is a tally wearing a duration's name, and the LIFETIME/per-stream split is already the one
        //        thing about this instrument a reader has to hold in their head.
        //    (3) reprimes() already counts the events, and a re-prime's LENGTH carries nothing new: it is
        //        however long the SOURCE took to deliver TARGET blocks, which is exactly what the gap histogram
        //        (gapmax_ms / gbig) is built to measure, on the arrival side where it can be measured honestly.
        //    What that costs, recorded rather than fixed by overloading this counter: with `under` now counting
        //    EVENTS, the total SILENCE inserted is in no counter.  reprimes() * TARGET bounds it from below,
        //    and spec s5 does not ask for it.
        //    RED with the `if (!m_primed)` guard removed from update()'s priming branch: the 50 silent blocks
        //    plus the TARGET-1 refill blocks land here too -- MEASURED 68 against 3 in the default arm, 60
        //    against 3 in the control arm.  (The dry block ITSELF is not among them: it takes the dry branch,
        //    not the priming one, because m_priming is still false at the top of that update().)
        CHECK(nd->primeBlocks() == primeAtStart);
        CHECK(ShimAudio::outstanding() == 0);
    }
    {   // 13b. With the pre-fill OFF (the bench's CONTROL arm) a dry ring is the NEW-41 behaviour exactly: one
        //      underrun per silent block, no re-prime, the servo integrating fill=0 -- the A/B's control must be
        //      the old firmware, not a third thing.  DEMONSTRATED RED against the `if (m_prefill)` guard removed
        //      from update()'s dry branch (a re-prime whatever the build): ALL FOUR checks below fail by name --
        //      underruns() 1 not 10, reprimes() 1 not 0, priming() set, and the trim frozen at trimFull.  Nothing
        //      else in either arm sees that mutation (4 failures, all here): no other case takes more than ONE
        //      dry block with the pre-fill off, and the FIRST dry block books its underrun either way -- it is
        //      the SECOND that separates the two behaviours, which is why this case takes ten.
        Node nd; nd->setPrefill(false); nd->begin();
        std::vector<uint8_t> f = encodeFrame(12000);
        for (int i = 1; i <= 4; i++) feed(nd, onePacket((uint16_t)i, f));
        for (int i = 0; i < 4; i++) nd->update();
        int32_t trimFull = nd->trimPpm();
        for (int i = 0; i < 10; i++) nd->update();
        CHECK(nd->underruns() == 10); CHECK(nd->reprimes() == 0); CHECK(!nd->priming());
        // STRICTLY less, not `<=`: the claim is that the servo KEPT INTEGRATING fill=0, and a servo frozen on a
        // dry ring (which is what the re-prime does when m_prefill is on) satisfies `<=` exactly.  Measured on
        // this construction: trimFull is -6 and the ten dry blocks take it to -24 in the default arm, -3 -> -12
        // in the control arm, so the margin is not marginal.
        CHECK(nd->trimPpm() < trimFull);
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
