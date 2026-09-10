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
// Case 15(a) drives a fixed 33-block spell and asserts the re-prime ENGAGED inside it -- that assertion is what
// stops (a) from being equally green on a build with no re-prime at all.  A threshold at or above 33 would
// leave the spell entirely sub-threshold and (a) would fail for a configuration reason with nothing naming it.
static_assert(AudioInputBluetooth::REPRIME_AFTER < 33,
    "node_test case 15(a) needs its 33-block spell to CROSS the re-prime threshold");

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
        //     exactly ONE dry block here, and under the THRESHOLD re-prime (spec s9) that block arms nothing:
        //     it books its underrun and the node stays out of priming, which is the whole point of the
        //     threshold.  So the underrun check is unchanged in VALUE and no longer for the old reason -- it
        //     read 1 before because a re-prime absorbed the silence after the first block, and it reads 1 now
        //     because there IS only one silent block.  The reprimes() check is what makes that distinction
        //     checkable rather than prose (RED at BT_SINK_REPRIME_AFTER=0: 1); case 16 owns the boundary and
        //     case 13 the re-prime's own mechanics.
        for (int i = 0; i < AudioInputBluetooth::TARGET; i++) nd->update();        // TARGET-1 pops, then one dry block
        CHECK(nd->fill() == 0);
        CHECK(nd->underruns() == 1);
        CHECK(nd->reprimes() == 0);                                                // one dry block is below the threshold
        CHECK(!nd->priming());
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
    {   // 13. RE-PRIME ON A DRY RING (NEW-42, spec s2 + s9).  Mid-stream the source stops and the ring runs dry.
        //     Past the THRESHOLD -- REPRIME_AFTER consecutive dry blocks, each booking its own underrun -- the
        //     next dry block rebuffers: reprimes() goes 1 and the node holds silence, UNCOUNTED and with the
        //     servo frozen, until TARGET blocks are back, then pops.  The filter is RECENTRED on the way back so
        //     the excursion does not drag the trim: the servo is wound far off-centre first, or "recentred"
        //     would be satisfied by a filter that never moved.  Without the re-prime at all a 45 ms gap left the
        //     ring 15 blocks short for the ~72 s it takes the trim to refill it (spec s1, fact 3).
        //     ** What the threshold changed here, and what it did NOT. **  The old case drove ONE dry block and
        //     asserted `underruns() == 1` throughout; every claim it was making -- silence uncounted while
        //     PRIMING, the servo frozen while priming, resume only at TARGET, the recentre, primeBlocks()
        //     untouched -- is still true and still asserted, just reached REPRIME_AFTER + 1 blocks later.  The
        //     one claim that is gone is "exactly ONE underrun for a dropout", which spec s9.3 retired
        //     deliberately: below the threshold `under` counts BLOCKS.  It is replaced, not dropped -- the
        //     checks below pin that a crossing spell costs exactly REPRIME_AFTER + 1 and that the priming
        //     remainder costs nothing, which is the same claim about the priming window that `== 1` used to make.
        //     RED against: an underrun per silent block WHILE PRIMING (underruns() climbs past REPRIME_AFTER+1
        //     over the 50 silent blocks); a resume below TARGET (pops at TARGET-1); the recentre removed --
        //     MEASURED below; the prime's `if (!m_primed)` guard removed (primeBlocks() stops being the START
        //     figure -- see below).  The boundary itself (exactly REPRIME_AFTER does NOT fire) is case 16's.
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
        // BOUNDED, and that is not tidiness: this is the only loop here whose termination depends on the node
        // continuing to POP, so an overshoot regression (the completion test weakened to `== TARGET`, which the
        // `>=` exists to prevent) HANGS the suite instead of failing -- and stdout is block-buffered to the pipe,
        // so the two checks above it never reach the terminal either.  A build that never ran, exactly the shape
        // this file's static_asserts exist to stop.
        for (int i = 0; i < AudioInputBluetooth::RING && nd->fill() > 0; i++) nd->update();   // the source stops: drain to dry
        CHECK(nd->fill() == 0);                    // a node that stopped popping fails HERE, by name, not by hanging
        CHECK(nd->underruns() == 0);
        ShimAudio::reset();
        // The sub-threshold stretch: REPRIME_AFTER dry blocks, one underrun EACH and no rebuffer.  Note the
        // servo DOES step through these (they are not priming blocks), which is what winds the filter down
        // before the recentre check at the end of the case -- and is why that check's RED value moved.
        const uint32_t R13 = AudioInputBluetooth::REPRIME_AFTER;
        for (uint32_t i = 0; i < R13; i++) nd->update();
        CHECK(nd->underruns() == R13); CHECK(nd->reprimes() == 0); CHECK(!nd->priming());
        nd->update();                                                               // the block that CROSSES it
        CHECK(nd->underruns() == R13 + 1); CHECK(nd->reprimes() == 1); CHECK(nd->priming());
        int32_t filtAtDry = nd->servo().filt_x65536;
        for (int i = 0; i < 50; i++) nd->update();                                  // silent, uncounted, servo frozen
        CHECK(nd->underruns() == R13 + 1);
        CHECK(nd->servo().filt_x65536 == filtAtDry);
        bool silent = true; for (int i = 0; i < ShimAudio::logCount(); i++) if (!allZero(ShimAudio::logData(i))) silent = false;
        CHECK(silent);
        for (int i = 1; i < AudioInputBluetooth::TARGET; i++) { feed(nd, onePacket(seq++, f)); nd->update(); CHECK(nd->priming()); }   // TARGET-1 is not enough
        CHECK(nd->fill() == AudioInputBluetooth::TARGET - 1);
        feed(nd, onePacket(seq++, f));                                              // TARGET
        ShimAudio::reset();
        nd->update();                                                               // resumes: pops
        CHECK(!nd->priming()); CHECK(nd->reprimes() == 1); CHECK(nd->underruns() == R13 + 1);
        CHECK(nd->fill() == AudioInputBluetooth::TARGET - 1);
        const int16_t *tx = lastTx(0); CHECK(tx && !allZero(tx));
        int32_t d = nd->servo().target_x65536 - nd->servo().filt_x65536;            // recentred, then ONE EMA step at TARGET-1
        // RED with the recentre removed: the filter is left 3.34 blocks ABOVE target (4.08 in the control
        // arm), where a recentred one sits 0.003 BELOW it.  RE-MEASURED for the threshold -- it read 4.26 /
        // 4.7 before, and it moved because the REPRIME_AFTER sub-threshold blocks are NOT priming blocks, so
        // the servo steps through them at fill = 0 and winds the filter part of the way back on its own.
        CHECK(d >= 0 && d <= 65536 / 344 + 1);
        // ** THE primeBlocks QUESTION, ANSWERED: a mid-stream re-prime NEITHER EXTENDS NOR RE-TIMES primeBlocks().
        //    prime_ms is the START prime's length and nothing else. **  Three reasons, in order of weight.
        //    (1) Spec s5's acceptance is "ONE prime_ms ~ TARGET * 2.9 ms at START", read off a heartbeat sampled
        //        at the END of a 10-min window.  Extend it and the number is START plus every re-prime since;
        //        re-time it and the START figure is gone the first time the ring runs dry.  Under either the
        //        criterion cannot be checked at all -- and s5 expects reprimes to be non-zero (its bound was
        //        `under <= reprimes + 2`; spec s9.3 restated that once the threshold made `under` a block
        //        count again), so "there will not be any" is not an available defence.
        //    (2) The header types it as a PER-STREAM DURATION, not a tally.  "Total blocks ever spent priming"
        //        is a tally wearing a duration's name, and the LIFETIME/per-stream split is already the one
        //        thing about this instrument a reader has to hold in their head.
        //    (3) reprimes() already counts the events, and a re-prime's LENGTH carries nothing new: it is
        //        however long the SOURCE took to deliver TARGET blocks, which is exactly what the gap histogram
        //        (gapmax_ms / gbig) is built to measure, on the arrival side where it can be measured honestly.
        //    What that costs, recorded rather than fixed by overloading this counter: the total SILENCE
        //    inserted is in no counter -- `under` counts only the dry blocks OUTSIDE the priming window, and
        //    dryTotal() bounds the remainder from above without separating it.  reprimes() * TARGET is an UPPER BOUND rather
        //    than an estimate, and over-reads 8x for the bursting source spec s1 describes (measured in a closed
        //    loop: 2 silent blocks at a 49 ms gap against the 16 it predicts) -- and spec s5 does not ask for it.
        //    RED with the `if (!m_primed)` guard removed from update()'s priming branch: the 50 silent blocks
        //    plus the TARGET-1 refill blocks land here too -- MEASURED 68 against 3 in the default arm, 60
        //    against 3 in the control arm.  (The REPRIME_AFTER + 1 dry blocks are
        //    not among them: they take the dry branch, not the priming one, because m_priming is still false
        //    at the top of each -- which is also why those two measured figures did not move when the
        //    threshold landed.)
        CHECK(nd->primeBlocks() == primeAtStart);
        CHECK(ShimAudio::outstanding() == 0);
    }
    {   // 13b. With the pre-fill OFF (the bench's CONTROL arm) a dry ring is the NEW-41 behaviour exactly: one
        //      underrun per silent block, no re-prime, the servo integrating fill=0 -- the A/B's control must be
        //      the old firmware, not a third thing.
        //      ** ITS RED DEMONSTRATION IS STALE AND IS RECORDED AS SUCH RATHER THAN QUIETLY DROPPED. **  Before
        //      the threshold, removing the `if (m_prefill)` guard from update()'s dry branch failed all four
        //      checks here by name (underruns 1 not 10, reprimes 1, priming set, the trim frozen) and reddened
        //      nothing else in either arm.  RE-RUN after the threshold landed: it reddens NOTHING here --
        //      MEASURED, 3 failures and all three in case 16(f).  The reason is the threshold's own coupling:
        //      with the pre-fill off nothing latches m_primed, so the dry counter never leaves 0, and
        //      `m_dryRun > REPRIME_AFTER` blocks the re-prime whether or not the m_prefill term is there.
        //      This case still pins the BEHAVIOUR (it fails outright if the dry branch re-primes at all -- the
        //      whole condition replaced by `true` fails all four); what it no longer pins is that ONE term, and
        //      case 16(f) builds the construction that does.  Ten dry blocks rather than one because the FIRST
        //      dry block books its underrun either way -- it is the SECOND that separates the two behaviours.
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
    {   // 15. THE DRY-SPELL INSTRUMENT (NEW-42, spec s9).  The threshold re-prime's N -- rebuffer only after
        //     the ring has been dry for more than N consecutive blocks -- has to be MEASURED, the way TARGET 16
        //     was, and ** the re-prime TRUNCATES the quantity to measure **: the ring goes dry for one block and
        //     then enters priming, so a counter that stops at "dry" reads 1 for ever.  What is counted instead
        //     is consecutive update()s on an EMPTY RING whatever branch they take, so a spell is recovered at
        //     its true length THROUGH the re-prime's own window.  (a) is that claim and is the reason this case
        //     exists; the rest pin the edges, the exclusions and the lifetime.
        //     EVERY sub-case turns the pre-fill ON explicitly, like cases 12 and 13, so it runs in BOTH CMake
        //     arms -- and with BT_SINK_PREFILL=0 there is no completed START prime and the instrument is silent
        //     by construction, which is the very configuration (a) has to run under to mean anything.
        std::vector<uint8_t> f = encodeFrame(11000);
        const int T = AudioInputBluetooth::TARGET;
        // Refill to TARGET (which completes any prime) and pop back to empty: leaves the node PRIMED, dry, and
        // with no spell in progress -- the state each spell below starts from.  Neither loop can run away: the
        // feed loop adds one block per packet against TARGET <= RING - 2, and the pop loop is BOUNDED for case
        // 13's reason -- a node that stopped popping must FAIL this case by name, not hang the suite with the
        // checks above it still sitting in stdout's pipe buffer.
        auto rearm = [&](Node &nd, uint16_t &seq) {
            while (nd->fill() < T) feed(nd, onePacket(seq++, f));
            for (int i = 0; i <= T && nd->fill() > 0; i++) nd->update();   // never a dry block: fill is tested first
        };
        // k consecutive EMPTY blocks, then one arriving frame to END the spell.  With the pre-fill on the FIRST
        // of those blocks arms the re-prime and every later one takes the PRIMING arm, which is exactly what an
        // instrument sitting in the dry branch would fail to see.
        auto spell = [&](Node &nd, uint16_t &seq, int k) {
            for (int i = 0; i < k; i++) nd->update();
            feed(nd, onePacket(seq++, f));
            nd->update();                                                  // a block is there: the spell ENDS here
        };
        // (a) THE LOAD-BEARING CASE: 33 dry blocks read as 33, not as 1.  The re-prime is asserted to have
        //     ENGAGED first -- without that this sub-case would be equally green on a build with no re-prime at
        //     all, where nothing truncates anything and the claim is empty.
        //     DEMONSTRATED RED against the obvious implementation -- count in the dry branch, close the spell
        //     on the pop -- and RE-MEASURED after the threshold landed, because the threshold moved the
        //     numbers: 33 dry blocks now read `dryMax=0 dryTotal=17 d4=0 dbig=0` (they read `dryTotal=1`
        //     before it).  Still `dryMax=0`: with the pre-fill on the block that ENDS the spell is a priming
        //     block, so a dry-branch instrument never closes the spell at all and it reaches no bucket.
        //     ** The threshold has PARTLY neutralised the truncation this instrument was built against, and
        //     only for the spells that do not matter: ** in (b) the 4/8/16-block spells are entirely
        //     sub-threshold, so a dry-branch counter now sees all three exactly (d4=d8=d16=1, where it used
        //     to read 1 block for each), and it is the 32-block one -- the length N is read off -- that is
        //     still truncated, to 17.  (b)'s `dryMax == 32` is what catches it.
        Node na; na->setPrefill(true); na->begin();
        uint16_t sa = 1;
        rearm(na, sa);
        CHECK(na->fill() == 0);
        CHECK(na->dryTotal() == 0); CHECK(na->dryMax() == 0);              // popped blocks are not dry blocks
        CHECK(na->reprimes() == 0);
        spell(na, sa, 33);
        const uint32_t R15 = AudioInputBluetooth::REPRIME_AFTER;
        CHECK(na->reprimes() == 1);                                        // the re-prime engaged: 33 - (R15+1) of the 33 primed
        CHECK(na->underruns() == R15 + 1);                                 // ... after R15 sub-threshold ticks; the rest are primed
        CHECK(na->dryMax() == 33);                                         // RED in the dry branch: 1
        CHECK(na->dryTotal() == 33);                                       // RED in the dry branch: 1
        CHECK(na->dryBucket(4) == 1);                                      // > 32
        CHECK(na->dryBucket(0) == 0);                                      // RED in the dry branch: 1
        // (b) BUCKET TOPS, all four exercised exactly ON the inclusive top (the convention gapBucketOf's edges
        //     are pinned by in case 11): turn any one `<=` into `<` and that spell alone moves up a bucket, so
        //     two counters change.  The order is 4, 8, 32, 16 DELIBERATELY -- the longest spell is not the last
        //     one, so `m_dryMax = m_dryRun` unconditional (a last-writer, which is the defect the gap histogram
        //     actually had) reads 16 here instead of 32.
        Node nb; nb->setPrefill(true); nb->begin();
        uint16_t sb = 1;
        rearm(nb, sb); spell(nb, sb, 4);
        rearm(nb, sb); spell(nb, sb, 8);
        rearm(nb, sb); spell(nb, sb, 32);
        rearm(nb, sb); spell(nb, sb, 16);
        CHECK(nb->dryBucket(0) == 1);                                      // 4, on the top of <=4
        CHECK(nb->dryBucket(1) == 1);                                      // 8, on the top of <=8
        CHECK(nb->dryBucket(2) == 1);                                      // 16, on the top of <=16
        CHECK(nb->dryBucket(3) == 1);                                      // 32, on the top of <=32
        CHECK(nb->dryBucket(4) == 0);
        CHECK(nb->dryTotal() == 4 + 8 + 32 + 16);                          // every empty block, and only those
        CHECK(nb->dryMax() == 32);                                         // RED as a last-writer: 16
        CHECK(nb->dryBucket(AudioInputBluetooth::DRY_BUCKETS) == 0);       // the bounds guard: ASan reads m_dry[5] without it
        // (c) A SUSPEND RECORDS NOTHING, and the spell it interrupts is DROPPED rather than bucketed.  Same
        //     call this file already makes for the gap histogram (case 11: a 30 s pause is not a 30 s gap), and
        //     it matters more here: a pause's dry TAIL runs until the SUSPEND signalling arrives and would land
        //     in the long buckets -- which is the tail N is read off.  The blocks stay in dryTotal, so they are
        //     dropped from the population rather than from the instrument.
        //     RED with the counting hoisted out of `if (run)`: dryTotal() reads 16.  RED with the !run arm
        //     RECORDING the spell instead of dropping it: dryMax() reads 6 and d8 reads 1.
        Node nc; nc->setPrefill(true); nc->begin();
        uint16_t sc = 1;
        rearm(nc, sc);
        for (int i = 0; i < 6; i++) nc->update();                          // six dry blocks: a spell IN PROGRESS
        CHECK(nc->dryTotal() == 6);
        CHECK(nc->dryMax() == 0);                                          // not recorded yet -- it has not ended
        CHECK(nc->dryBucket(1) == 0);
        nc->hold(true);
        for (int i = 0; i < 10; i++) nc->update();                         // the SUSPEND: silence that is not a dropout
        CHECK(nc->dryTotal() == 6);                                        // RED outside `if (run)`: 16
        nc->hold(false);
        feed(nc, onePacket(sc++, f)); nc->update();                        // a block arrives: nothing left to record
        CHECK(nc->dryMax() == 0);                                          // RED if the held spell were recorded: 6
        for (uint8_t i = 0; i < AudioInputBluetooth::DRY_BUCKETS; i++) CHECK(nc->dryBucket(i) == 0);
        CHECK(nc->dryTotal() == 6);                                        // ... and the blocks are still in the total
        // (d) THE START PRIME IS EXCLUDED -- it is a ~30-block empty stretch by construction (87/89 ms measured
        //     on the bench) and would be the biggest thing in the histogram while saying nothing about the
        //     source -- and the instrument WORKS the moment that prime completes.  The second half is not
        //     decoration: an instrument silenced for the whole run reads exactly like a source that never
        //     gapped, which is the one failure shape that looks like good news.
        //     RED with the m_primed guard removed: dryTotal() reads 10 here, then 15 below, the ten prime
        //     blocks land in d16, and dryMax() reads 10.
        Node nd2; nd2->setPrefill(true); nd2->begin();
        uint16_t sd = 1;
        for (int i = 0; i < 10; i++) nd2->update();
        CHECK(nd2->priming() && !nd2->primed());
        CHECK(nd2->primeBlocks() == 10);                                   // they ARE empty blocks ...
        CHECK(nd2->dryTotal() == 0);                                       // ... and none of them is counted: RED 10
        CHECK(nd2->dryMax() == 0);
        rearm(nd2, sd);
        spell(nd2, sd, 5);
        CHECK(nd2->dryTotal() == 5);                                       // RED with the guard removed: 15
        CHECK(nd2->dryBucket(1) == 1);                                     // 5 -> the <=8 bucket
        CHECK(nd2->dryBucket(2) == 0);                                     // RED with the guard removed: the prime lands here
        CHECK(nd2->dryMax() == 5);                                         // RED with the guard removed: 10
        // (e) LIFETIME.  begin() runs on EVERY stream start (bt_sink_test.cpp's onStreamCb) and these tallies
        //     are printed beside m_over/m_under, which it has never reset -- so all three must survive it, and
        //     the survivors checked here are NON-ZERO rather than "0 stayed 0".  The one thing that must NOT
        //     survive is the spell in PROGRESS: it belonged to the stream that ended and this ring starts empty,
        //     so a 3-block spell on the new stream must read 3 and not 6 + 3.
        //     RED with begin()'s `m_dryRun = 0` removed: the stranded 6 is recorded by the first block of the
        //     next stream, so d8 reads 2 and dryMax 6.
        rearm(nd2, sd);
        for (int i = 0; i < 6; i++) nd2->update();                         // a spell in progress across the reconnect
        CHECK(nd2->dryTotal() == 11);
        nd2->begin();
        CHECK(nd2->dryMax() == 5);                                         // RED if the tallies reset in begin(): 0
        CHECK(nd2->dryTotal() == 11);                                      // RED if the tallies reset in begin(): 0
        CHECK(nd2->dryBucket(1) == 1);                                     // RED if the tallies reset in begin(): 0
        rearm(nd2, sd);
        spell(nd2, sd, 3);
        CHECK(nd2->dryBucket(0) == 1);                                     // the new stream's 3
        CHECK(nd2->dryBucket(1) == 1);                                     // RED without begin()'s m_dryRun reset: 2
        CHECK(nd2->dryMax() == 5);                                         // RED without it: 6 -- and a max, not the last 3
        CHECK(nd2->dryTotal() == 11 + 3);
        CHECK(ShimAudio::outstanding() == 0);
    }
    {   // 16. THE THRESHOLD RE-PRIME (NEW-42, spec s9).  A ring that runs dry rebuffers only after MORE than
        //     REPRIME_AFTER consecutive dry blocks; below that it ticks one underrun per block and lets the
        //     SOURCE's catch-up burst refill it.  ** Why: RUN 5 failed spec s5's `over = 0` because a re-prime
        //     refills to TARGET from FRESH packets and the backlog the phone buffered during the gap then lands
        //     on top -- TARGET + backlog > RING - 1, surplus dropped (boot 2: fillmax 25 -> 31, overev 0 -> 3,
        //     reprimes 1 -> 2 in one window).  RUN 6 then measured the spells the threshold has to clear:
        //     three in 26.3 min, longest 9 blocks. **  Every sub-case turns the pre-fill ON explicitly, like
        //     cases 12/13/15, so it runs in BOTH CMake arms -- and every one of them is written against
        //     REPRIME_AFTER rather than 16, which makes them honest in any configuration and BLIND to the
        //     configuration itself.  (g) is where the value is pinned, and says what that blindness measures.
        const uint32_t R = AudioInputBluetooth::REPRIME_AFTER;
        const int T = AudioInputBluetooth::TARGET;
        std::vector<uint8_t> f = encodeFrame(11000);
        // Case 15's `rearm`, and bounded for its reason: a node that stopped popping must FAIL by name rather
        // than hang the suite with the checks above it still in stdout's pipe buffer.  Leaves the node PRIMED,
        // dry, and with no spell in progress -- the state a mid-stream dropout starts from.
        auto rearm = [&](Node &nd, uint16_t &seq) {
            while (nd->fill() < T) feed(nd, onePacket(seq++, f));
            for (int i = 0; i <= T && nd->fill() > 0; i++) nd->update();
        };
        // (a) THE BOUNDARY IS INCLUSIVE: exactly REPRIME_AFTER dry blocks do NOT rebuffer.  "More than N", so
        //     N itself is below it.  Each of those blocks books its own underrun -- `under` is a BLOCK count
        //     below the threshold (spec s9.3), and that is the trade this increment accepted with open eyes.
        //     RED against `>` weakened to `>=` -- 17 checks across cases 13, 15(a) and 16, TWO of them here.
        Node na; na->setPrefill(true); na->begin();
        uint16_t sa = 1;
        rearm(na, sa);
        CHECK(na->fill() == 0);
        CHECK(na->reprimes() == 0); CHECK(na->underruns() == 0);
        for (uint32_t i = 0; i < R; i++) na->update();
        CHECK(na->reprimes() == 0);                                        // RED with `>=`: 1
        CHECK(!na->priming());                                             // RED with `>=`: set
        // ** NOT reddened by `>=`, and the comment must not pretend it is -- MEASURED. **  The arming block
        // books its own tick either way, so an off-by-one moves WHICH block arms and not how many ticked: the
        // count reads R under both.  The two checks above are what see the boundary; this one is here because
        // it is the block-counting claim (spec s9.3), and it goes RED against a re-prime on the first dry
        // block (the whole guard forced true): 1, not R.
        CHECK(na->underruns() == R);
        // (b) ONE MORE CROSSES IT, and reprimes() increments EXACTLY ONCE however long the spell then runs.
        //     The 40 further blocks are the "exactly once" half: an implementation that armed the re-prime
        //     without latching m_priming would re-enter this branch and count 41.
        //     ** This sub-case is also what pins the COUPLING: the trigger reads the dry instrument's
        //     in-progress spell length (m_dryRun), so gating that counting off -- the obvious tidy-up now the
        //     histogram has done its job -- disables the re-prime silently.  RED with the counter's
        //     `if (m_primed)` forced false: reprimes stays 0 and priming() is clear. **
        na->update();
        CHECK(na->reprimes() == 1); CHECK(na->priming());
        CHECK(na->underruns() == R + 1);                                   // the arming block books its BLOCK tick ...
        for (int i = 0; i < 40; i++) na->update();
        CHECK(na->reprimes() == 1);                                        // ... exactly one re-prime
        CHECK(na->underruns() == R + 1);                                   // ... and the priming remainder books nothing
        CHECK(na->priming());
        // THE ACCOUNTING, stated as one number: a crossing spell costs exactly REPRIME_AFTER + 1 underruns,
        // however long it runs.  RED with an extra `m_under++` in the re-prime (the double count this
        // increment had to choose against): R + 2.
        while (na->fill() < T) feed(na, onePacket(sa++, f));
        na->update();                                                      // finds TARGET: prime complete, pops
        CHECK(!na->priming());
        CHECK(na->underruns() == R + 1);
        CHECK(na->reprimes() == 1);
        CHECK(na->dryTotal() == R + 41);                                   // R + the arming block + 40 primed
        CHECK(na->underruns() <= na->dryTotal());                          // spec s9.3's restated bound, on real numbers
        // (c) ** THE LOAD-BEARING CASE, and the whole point of the change: below the threshold the ring
        //     RESUMES ON THE VERY NEXT PACKET, with no wait to TARGET. **  One packet is ONE block, far below
        //     TARGET, and it must be played immediately -- that is what stops the ring from being refilled to
        //     TARGET out of fresh packets and then overrun by the backlog behind them.
        //     RED with `>` weakened to `>=`: the node is priming by then, so it transmits SILENCE and holds
        //     the block -- allZero(tx) fails and fill() reads 1.  Same shape against a re-prime on every dry
        //     block (the whole guard forced true): MEASURED, both of those checks fail there too.
        Node nb; nb->setPrefill(true); nb->begin();
        uint16_t sb = 1;
        rearm(nb, sb);
        for (uint32_t i = 0; i < R; i++) nb->update();
        CHECK(nb->underruns() == R); CHECK(!nb->priming());
        feed(nb, onePacket(sb++, f));                                      // ONE block arrives -- 1, not TARGET
        ShimAudio::reset();
        nb->update();
        const int16_t *tx = lastTx(0);
        CHECK(tx && !allZero(tx));                                         // it PLAYED, on the next block
        CHECK(nb->fill() == 0);                                            // ... popped, not accumulated toward TARGET
        CHECK(nb->underruns() == R);                                       // ... and that block is no underrun
        CHECK(nb->reprimes() == 0); CHECK(!nb->priming());
        // (d) THE START PRE-FILL IS UNTOUCHED by the threshold -- it primes to TARGET from block one, counting
        //     nothing, however many empty blocks that takes.  R + 4 of them is past the threshold and must
        //     still not book an underrun or a re-prime: the START prime is not a dropout, and a threshold
        //     leaking into it would put NEW-41's ~27 start-up underruns straight back.
        //     Not reddened by the threshold mutations -- it is the CONTROL for them, and it goes RED against a
        //     threshold applied at START (underruns R+4, priming clear).
        Node nc; nc->setPrefill(true); nc->begin();
        uint16_t sc = 1;
        for (uint32_t i = 0; i < R + 4; i++) { nc->update(); CHECK(nc->priming()); }
        CHECK(nc->underruns() == 0); CHECK(nc->reprimes() == 0);
        CHECK(nc->primeBlocks() == R + 4);
        while (nc->fill() < T) feed(nc, onePacket(sc++, f));
        ShimAudio::reset();
        nc->update();
        CHECK(!nc->priming() && nc->primed());
        CHECK(nc->fill() == (uint8_t)(T - 1));                             // still primes to TARGET, and that block pops
        const int16_t *tc = lastTx(0); CHECK(tc && !allZero(tc));
        CHECK(nc->underruns() == 0); CHECK(nc->reprimes() == 0);
        // (e) hold() STILL TAKES PRECEDENCE, and it takes precedence over the THRESHOLD too: R + 10 held blocks
        //     are past it and must arm nothing, because an AVDTP SUSPEND is not a dropout (the same call this
        //     file makes for `under` in case 9 and for the gap histogram in case 11).  Then the RESUME onto an
        //     empty ring is an ordinary spell and crosses the threshold like any other -- recorded here rather
        //     than assumed, because it is a behaviour a reader would expect the SUSPEND to have shortened.
        //     RED with `run = m_live && !m_hold` weakened to `run = m_live`: all three of the first checks
        //     fail -- the held blocks take the dry path, so underruns reads R+1 rather than 0, reprimes 1 and
        //     priming set (18 checks fail suite-wide, case 9's SUSPEND case and case 15(c) among them).
        Node nd3; nd3->setPrefill(true); nd3->begin();
        uint16_t sd = 1;
        rearm(nd3, sd);
        nd3->hold(true);
        for (uint32_t i = 0; i < R + 10; i++) nd3->update();
        CHECK(nd3->underruns() == 0); CHECK(nd3->reprimes() == 0); CHECK(!nd3->priming());
        nd3->hold(false);
        for (uint32_t i = 0; i < R; i++) nd3->update();
        CHECK(nd3->underruns() == R); CHECK(nd3->reprimes() == 0);
        nd3->update();
        CHECK(nd3->reprimes() == 1); CHECK(nd3->priming());
        // (f) THE `m_prefill` GUARD ON THE DRY BRANCH, which the threshold made redundant in both SHIPPED
        //     configurations and which is kept deliberately.  With the pre-fill off nothing latches m_primed,
        //     so the dry counter never leaves 0 and the threshold term alone already blocks the re-prime --
        //     MEASURED: deleting `m_prefill &&` reddens nothing in either arm, case 13b included, which is
        //     exactly the "a check stopped working and the suite stayed green" shape.  This is the one
        //     construction in which the two terms disagree: prime first (so m_primed latches and the counter
        //     runs), THEN turn the pre-fill off mid-stream.  Only the guard stops the rebuffer here.
        //     It is kept, rather than deleted as dead, because it states the intent the coupling only implies
        //     -- a BT_SINK_PREFILL=0 build must be NEW-41 exactly (case 13b) -- and because spec s9.2a already
        //     contemplates replacing m_primed with the arm-independent "after the first successful pop", under
        //     which the control arm WOULD latch and this guard becomes the only thing holding it.
        //     RED with `m_prefill &&` removed: reprimes 1, priming set, underruns R + 1.
        Node ne; ne->setPrefill(true); ne->begin();
        uint16_t se = 1;
        rearm(ne, se);
        CHECK(ne->primed());                                               // the counter is running ...
        ne->setPrefill(false);                                             // ... and only the guard is left
        for (uint32_t i = 0; i < R + 5; i++) ne->update();
        CHECK(ne->underruns() == R + 5);                                   // every dry block counts, NEW-41 style
        CHECK(ne->reprimes() == 0); CHECK(!ne->priming());
        // (g) THE DEFAULT VALUE, pinned the way case 14 pins RING/TARGET -- and it has to be pinned SEPARATELY,
        //     because ** every check above is parameterised on REPRIME_AFTER and therefore cannot see the
        //     threshold being reverted. **  MEASURED, not feared: with BT_SINK_REPRIME_AFTER mutated to 0 (the
        //     RUN 5 always-re-prime behaviour) this whole case, case 13 and case 15(a) stay GREEN -- every
        //     `R`-relative expectation simply follows the knob -- and the ONLY failures in the suite are case
        //     12(d)'s two, which hard-code "one dry block does not rebuffer".  A suite that adapts to the
        //     configuration is honest about the MECHANISM and silent about the VALUE, so the value is stated.
        //     16 is RUN 6's measurement (iPhone, 26.3 min: three dry spells, longest 9 blocks, `drymax=9
        //     drytot=11 d4=2 d16=1`), not a choice -- see AudioInputBluetooth::REPRIME_AFTER for why the ~25
        //     that was INFERRED beforehand was wrong by 3x.  The `> 9` states the property; the `== 16` is the
        //     value that property was satisfied with.
        //     Not asserted in the CONTROL arm: with BT_SINK_PREFILL=0 nothing ever re-primes, so the threshold
        //     is moot there and run.sh leaves it at its default rather than saying anything about it.
#if !defined(NODE_TEST_CONTROL_ARM)
        CHECK(AudioInputBluetooth::REPRIME_AFTER > 9);                     // clear above RUN 6's longest spell
        CHECK(AudioInputBluetooth::REPRIME_AFTER == 16);                   // ... and that is the value it chose
#endif
        CHECK(ShimAudio::outstanding() == 0);
    }
    printf("node_test: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
