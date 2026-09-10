// AudioInputBluetooth -- an AudioStream SOURCE node fed by A2DP media (NEW-41): onMedia() (from the L2cap RX path,
// main context) parses RTP + the SBC payload header and decodes frames into a PCM ring; update() (the SAI DMA ISR's
// graph walk) pops one block per period and transmits it, outputting silence + counting on underrun; the PLL servo
// (servo.h) runs once per block on the ring's fill and trims the audio PLL so the SAI consumes at the source's rate.
// The decode is OUT of the ISR (the 2026-09-04 livelock lesson: a flash-resident SBC encode inside the SAI ISR
// overran the 2.9 ms block period and livelocked the main loop); the ring is SPSC -- onMedia owns m_head, update
// owns m_tail, and NEITHER writes the other's index.  Unlike the sibling AudioOutputBluetooth this node does NOT
// self-clock: the SAI DMA ISR walks the graph, so update() is already called at exactly the audio rate.  MIT.
#pragma once
#include <Arduino.h>                               // AudioStream.h needs F_CPU_ACTUAL/IRQ_NUMBER_t from this first
#include <string.h>
#include "AudioStream.h"
#include "SbcDecoder.h"
#include "servo.h"
#if defined(__IMXRT1176__)
#include "I2S.h"                                   // audioPllTrimPpm()
#else
// The example is rt1176-only; this keeps the node HOST-COMPILABLE (tests/node_test.cpp) and buildable on any
// other board, where there is no trimmable audio PLL to talk to.
static inline int32_t audioPllTrimPpm(int32_t) { return 0; }
#endif
static_assert(AUDIO_BLOCK_SAMPLES == 128,
    "AudioInputBluetooth assumes one SBC frame (16 blocks x 8 subbands) == one Audio-library block; "
    "AUDIO_BLOCK_SAMPLES has moved.");
#ifndef BT_SINK_RING
#define BT_SINK_RING 40
#endif
#ifndef BT_SINK_TARGET
#define BT_SINK_TARGET 16
#endif
#ifndef BT_SINK_PREFILL
#define BT_SINK_PREFILL 1
#endif
#ifndef BT_SINK_REPRIME_AFTER
#define BT_SINK_REPRIME_AFTER 16
#endif
class AudioInputBluetooth : public AudioStream {
public:
    AudioInputBluetooth() : AudioStream(0, nullptr) {}
    void begin();                                 // stream started: reset ring/decoder, recentre the servo, start counting
    void end();                                   // stream lost/closed: clear the ring, hold the trim
    // AVDTP SUSPEND: the source stopped sending but the stream is still configured, so m_live stays true and the
    // ring simply runs dry.  Left alone the servo would then integrate fill=0 all the way to its -200 ppm clamp
    // and update() would count an underrun 344 times a second, for a link that is behaving exactly as it should.
    // While held the node outputs silence, steps no servo and counts no underrun; the trim stays where the live
    // stream left it, so a RESUME starts from the rate it had learned.  The sketch calls this every loop pass
    // with A2dpSink::suspended().
    // A RESUME also starts a FRESH gap interval, for the same reason the file already gives for the underrun
    // counter: a suspended source is not a delivery failure, so the silence it left behind is not jitter.  The
    // node otherwise times the first post-RESUME packet against the last pre-SUSPEND one and books the whole
    // pause as one delivery gap -- MEASURED on this instrument before the reset went in: `gapmax_ms=23` while
    // streaming became `gapmax_ms=30023` across a 30 s SUSPEND/RESUME.  The iPhone bench run exercises
    // pause/resume and spec s3 makes this distribution the thing that SIZES TARGET, so a pause left in it would
    // corrupt the one number the whole task exists to produce.  Only the true->false EDGE resets: this is called
    // every loop pass (above), so an unconditional reset would restart the interval on every pass and the
    // histogram would measure nothing at all.
    void hold(bool on) { if (m_hold && !on) m_haveRx = false; m_hold = on; }
    bool held() const { return m_hold; }
    void onMedia(const uint8_t *rtp, uint16_t len);   // A2dpSink's media callback target (main context)
    virtual void update(void);                    // the SAI ISR: pop a block (or silence) + servo
    uint32_t pkts() const { return m_pkts; } uint32_t frames() const { return m_frames; } uint32_t seqGaps() const { return m_seqGaps; }
    uint32_t underruns() const { return m_under; } uint32_t overruns() const { return m_over; } uint32_t badFrames() const { return m_bad; }
    uint8_t  fill() const { uint16_t h = m_head, t = m_tail; return (uint8_t)((h + RING - t) % RING); }
    int32_t  trimPpm() const { return m_servo.trim_ppm; }
    // The servo's INTERNAL state, for the HOST TESTS and nothing else -- the firmware reads trimPpm() and only
    // trimPpm().  node_test case 13 needs it because the mid-stream re-prime's servo_recentre() is invisible
    // from the trim: a filter left wound up from the pre-drop fill still produces a perfectly plausible ppm,
    // so "recentred" can only be asserted against filt_x65536 itself.  Nothing outside update() may WRITE
    // m_servo (see the member's own note); this hands out a const reference so nothing can.
    const sink_servo_t &servo() const { return m_servo; }
    // Mean |L| per decoded block, summed: rmsAcc()/rmsBlocks() is the mean absolute left-channel sample, the
    // gate's audio-clock-referenced measure of "is there really audio in here".  * The per-block MEAN is
    // accumulated, not the raw per-sample sum: a raw sum of |v| <= 32768 over 128 samples/block overflows a
    // uint32 after ~1024 blocks (3 s of streaming), which a gate running longer than that would read as garbage.
    // The quotient the consumer computes is unchanged; the headroom is 128x larger (~380 s).
    uint32_t rmsAcc() const { return m_rmsAcc; } uint32_t rmsBlocks() const { return m_rmsBlocks; }
    uint32_t crc() const { return m_crc; }        // running CRC32 of the first 200 decoded left-channel blocks (a golden for the gate)
    // NEW-42: ring depth and target in blocks, and whether START pre-fills the ring, all overridable from CMake
    // (BT_SINK_RING / BT_SINK_TARGET / BT_SINK_PREFILL) so the bench builds its CONTROL arm (16 / 8 / off -- the
    // NEW-41 behaviour) and its CHANGE arm from ONE source.  Sizing (spec s2): a source delivers EIGHT blocks per
    // packet, so the ring needs one packet of headroom above the operating point, the largest gap it should ride
    // out below it (~16 blocks = 46 ms), and the servo's standing offset (+-2.5 blocks at +-100 ppm).  32 / 16
    // gives 16 above and 16 below; the old 16 / 8 held exactly two packets, and the servo's offset parked the
    // mean at the ceiling (iPhone bench 2026-09-09, run 3).  The gate always builds the defaults.
    // ** RING 32 -> 40 (bench RUN 8, 2026-09-10); TARGET stays 16, so latency and the DelayReport do not
    // move. **  RUN 7 met `over = 0` across 38.8 min with the threshold re-prime, but boot 1 reached
    // fillmax=30 against a ceiling of 31 on its one 80-120 ms gap: a ONE-BLOCK margin.  The overflow
    // mechanism survives the threshold, it is merely no longer AMPLIFIED -- the post-gap peak is the
    // source's `backlog` rather than `TARGET + backlog`.  40 gives that backlog 23 blocks of headroom above
    // the operating point instead of 15, for 8 KB of .bss and no added latency.
    static constexpr uint16_t RING = BT_SINK_RING, TARGET = BT_SINK_TARGET;
    static constexpr bool PREFILL = (BT_SINK_PREFILL) != 0;
    static_assert(RING >= 4 && RING <= 255 && TARGET >= 1 && TARGET <= RING - 2,
        "AudioInputBluetooth: TARGET must leave two slots below RING (one is the SPSC sentinel), and RING "
        "must fit fill()'s uint8_t return AND stay <= 255 so (uint8_t)RING remains an OUT-OF-BAND sentinel "
        "for m_fillMin -- at RING == 256 it truncates to 0, the low-water mark can never be lowered, and the "
        "instrument prints a permanent false fillmin=0 while looking perfectly healthy.  The "
        "one-packet-of-headroom claim is TARGET's, not this bound's: it is asserted in node_test case 14, "
        "because the bench's CONTROL arm (16/8) deliberately breaks it.");
    // THE THRESHOLD (NEW-42, spec s9), overridable as BT_SINK_REPRIME_AFTER like the three above: a ring that
    // runs dry mid-stream re-primes only after it has been dry for MORE than REPRIME_AFTER consecutive blocks.
    // Below that it simply ticks 2.9 ms underruns and the SOURCE's catch-up burst refills it, so the re-prime's
    // `TARGET` term never enters the sum.  ** That sum is what RUN 5 failed spec s5's `over = 0` on: a re-prime
    // refills the ring to TARGET from FRESH packets, and then the backlog the phone buffered during the gap
    // lands on top -- TARGET + backlog > RING - 1, surplus DROPPED.  Boot 2 went fillmax 25 -> 31, overev
    // 0 -> 3, reprimes 1 -> 2 inside one heartbeat window. **  The re-prime becomes what it should always have
    // been: the safety net for a source that does NOT catch up and would otherwise leave the ring permanently
    // short (spec s1, fact 3), rather than a response to ordinary jitter.  Not a bigger RING, which out-runs
    // the mechanism instead of removing it.
    // ** 16 IS MEASURED, NOT CHOSEN. **  RUN 6 (iPhone, 26.3 min, the dry-spell instrument below, same
    // behaviour as RUN 5 so the two compare): THREE dry spells -- two of <= 4 blocks and one of <= 16 -- the
    // LONGEST 9 blocks, eleven dry blocks in total (`drymax=9 drytot=11 d4=2 d8=0 d16=1 d32=0 dbig=0`).  16 is
    // clear above every spell that phone produced and still fires for a genuine stall.  ** The INFERENCE this
    // replaced said ~25 and was wrong by ~3x: ** spec s9.1 reasoned that the servo holds fill at ~TARGET, so a
    // G-block gap strands the ring for about G - 16 -- but the same heartbeat reads `fillmax=27`, so the
    // operating point PLUS the 8-block packet sawtooth keeps fill well ABOVE TARGET much of the time and a
    // 22-block gap is simply absorbed.  The ring empties only when a gap lands on a trough, which is rare and
    // shallow.  RUN 6 also held `over=0` for 26.3 min WITH three re-primes, so the threshold removes a
    // MECHANISM, not a certainty.
    // 16 blocks is 46.4 ms; it comes out TARGET-sized by arithmetic, not because it was tied to TARGET.
    static constexpr uint16_t REPRIME_AFTER = BT_SINK_REPRIME_AFTER;
    // Whether begin() pre-fills the ring to TARGET before the first pop, AND whether a ring that runs dry
    // mid-stream re-primes rather than counting every silent block.  One knob for both, deliberately: the
    // bench's CONTROL arm must be the NEW-41 firmware exactly, not a third thing (node_test case 13b).
    // Defaults to the compile-time PREFILL; the sketch never calls this, the host tests do, so that the
    // ring/parser cases run without the pre-fill and cases 12/13 run with it, in BOTH CMake arms.
    void setPrefill(bool on) { m_prefill = on; }
    // --- the NEW-42 instrument: cumulative for the RUN, never per-heartbeat -----------------------------------
    // The committed transcript samples every 30th heartbeat, so a per-window maximum would be invisible in 29 of
    // 30 windows -- and an extreme that is never printed reads exactly like one that never happened.
    // LIFETIME, not per-stream: these tallies and extremes are printed beside m_over/m_under/m_pkts, which
    // begin() has never reset, and begin() runs on EVERY stream start (bt_sink_test.cpp's onStreamCb).  Resetting
    // them there would put `overev=0` next to `over=89` after a reconnect and make a 30 s heartbeat delta -- which
    // is exactly how spec s1 read the bench's run 3 -- jump backwards with nothing saying why.  (CLAUDE.md records
    // the same footgun from L2cap's l2frag, which DOES reset per attempt and needs a standing warning saying so.)
    static constexpr uint8_t GAP_BUCKETS = 5;      // <=30 / <=50 / <=80 / <=120 / >120 ms, against a 23.2 ms nominal period
    uint32_t gapMaxUs() const { return m_gapMaxUs; }                       // longest interval between two accepted RTP packets
    // The 0 for i >= GAP_BUCKETS is a BOUNDS GUARD, not a reading: there is no such bucket to report.
    uint32_t gapBucket(uint8_t i) const { return i < GAP_BUCKETS ? m_gap[i] : 0; }
    uint8_t  fillMin() const { return m_fillMin; }                         // ring fill at the instant of a pop: the true low-side margin
    uint8_t  fillMax() const { return m_fillMax; }                         // ... and the high side; RING (min) / 0 (max) until the first pop
    int32_t  trimLo() const { return m_trimLo; }                           // the servo's output range: "is the trim settled" as a number
    int32_t  trimHi() const { return m_trimHi; }
    uint32_t overEvents() const { return m_overEv; }                       // RUNS of consecutive dropped frames (a burst of 8 is one event)
    // --- START pre-fill and mid-stream re-prime (NEW-42, spec s2) ------------------------------------------
    // While PRIMING, update() transmits silence, counts no underrun and steps no servo; the block that finds
    // TARGET blocks in the ring ends the prime, recentres the servo's filter and pops straight away, so the
    // first block OUT is the first block DECODED.  begin() popping from an empty ring is what produced the
    // ~27 start-up underruns of the iPhone bench (spec s1) -- the SAI ISR walks the graph from the moment the
    // stream is configured, long before the source's first packet can have landed.
    // A ring that runs DRY MID-STREAM re-primes the same way, and that is the other half of the fix: a gap of
    // G blocks does not merely cost G blocks of audio, it leaves the ring G blocks SHORT, and only the servo's
    // 72 s closed-loop trim puts the margin back (spec s1, fact 3 -- the bench's 15-36 underruns trailing each
    // overrun burst).  ** But only past REPRIME_AFTER consecutive dry blocks (spec s9): below the threshold the
    // ring ticks underruns and the source's own catch-up burst puts the margin back. **
    // ** WHAT `under` COUNTS, AND IT CHANGED WITH THE THRESHOLD (spec s9.3): BLOCKS, uniformly. **  Every dry
    // block that was played as silence books exactly one underrun -- INCLUDING the block that arms a re-prime --
    // and the re-prime itself books none of its own.  So a spell of L <= REPRIME_AFTER blocks costs L, a spell
    // that crosses costs exactly REPRIME_AFTER + 1 however long it then runs, and the priming remainder costs
    // nothing.  The alternative -- a per-block tick below the threshold PLUS an event tick at the re-prime --
    // would count the arming block twice and leave `under` meaning two different things on either side of one
    // number, which is the counter-that-means-two-things shape this file already refuses elsewhere.
    // Consequence, and it is spec s9.3's not this file's to soften: `under` no longer counts DROPOUTS, so spec
    // s5's `under <= reprimes + 2` stops being the right bound and is restated there as `over = 0` plus
    // `under <= dryTotal`.  The CONTROL arm (BT_SINK_PREFILL=0) counted blocks all along -- that is 13b's whole
    // subject -- so the two arms now agree on what the number MEANS and still differ in what produces it.
    // An `under` of 3 on this build is STILL not comparable with an `under` of 3 on NEW-41's: the threshold
    // suppresses the trailing 15-36 that each overrun burst used to leave behind.
    // primeBlocks() is the START prime's length in blocks (x 2.9 ms), and ONLY that.  Unlike the tallies above
    // it is PER-STREAM, because it is a DURATION and not a count: begin() restarts it and the value stands from
    // the moment that prime completes until the next begin().  A mid-stream re-prime neither EXTENDS nor
    // RE-TIMES it -- spec s5 reads "ONE prime_ms ~ TARGET * 2.9 ms at START" off a heartbeat sampled at the END
    // of a long window, and s5 expects re-primes to occur, so a figure that grew or restarted with them could
    // not be checked against TARGET at all.  How long a re-prime took is the SOURCE's absence, which the gap
    // histogram measures on the arrival side; reprimes() counts the events.  (Accepted gap, NARROWED by the
    // threshold: the total silence inserted is still in no counter, but dryTotal() below now bounds it from
    // above -- every silent block, priming or not, is one of those.  reprimes() * TARGET is an UPPER BOUND far
    // more often than an estimate, and for the source shape spec s1 describes it over-reads badly: a source that
    // buffers through the gap and bursts its backlog -- which is exactly the "35-45 ms gap then a catch-up burst"
    // -- refills past TARGET inside ONE onMedia(), so the re-prime costs a SINGLE silent block.  MEASURED in a
    // closed-loop model at a 49 ms gap: 2 silent blocks against the 16 this product predicts, an 8x over-read.
    // What the re-prime provably does NOT do is ADD silence: against a build without it the total emitted is
    // identical for a non-catch-up source at four gap lengths, and LOWER for a bursting one.)
    // reprimes() counts mid-stream ring rebuilds and is LIFETIME for the same reason m_overEv is: it counts
    // EVENTS printed beside m_over/m_under, and a reconnect must not make that column jump backwards.  Nothing
    // prints it yet (Task 6), so there is no reading here to misread.
    // ** A PRIME THAT NEVER REACHES TARGET NEVER ENDS, and that is the one signal this pre-fill takes away. **
    // A source that delivers fewer than TARGET blocks and then stalls without SUSPEND or CLOSE -- an RF dropout
    // right at stream start -- leaves the node silent with `under` FROZEN and the servo at its begin() trim: a
    // heartbeat indistinguishable from a healthy idle stream.  Before the pre-fill the same condition drove
    // `under` at 344/s, unmistakably.  priming() stays 1 and primeBlocks() keeps climbing, and those are the
    // only witnesses until Task 6 prints them.  Bounded in practice by BtLink's link supervision reaching end().
    // The MID-STREAM re-prime inherits that hazard and takes one more witness away, since primeBlocks() is the
    // START figure and does not climb for it -- but it also arrives with a better one that START cannot have:
    // `pkts`/`frames` on the existing bt_sink line STOP MOVING, which at stream start is indistinguishable from
    // "the source has not begun yet" and mid-stream is unambiguous.  A stalled source is read there, not here.
    bool     priming() const { return m_priming; }
    bool     primed() const { return m_primed; }
    uint32_t primeBlocks() const { return m_primeBlocks; }
    uint32_t reprimes() const { return m_reprimes; }
    // --- the DRY-SPELL instrument (NEW-42, spec s9): what N was sized against, and now what ARMS it ---------
    // ** THIS IS NO LONGER ONLY AN INSTRUMENT.  update()'s dry branch triggers the re-prime off the SAME
    // in-progress spell length this counts (m_dryRun), so deleting the counting -- the obvious tidy-up once N
    // has been measured and the histogram has done its job -- SILENTLY DISABLES THE RE-PRIME. **  It fails
    // safe (the node degenerates to NEW-41's per-block underruns and the source refills the ring itself, so
    // audio still plays) and it fails INVISIBLY, which is why node_test case 16(b) pins it: with the counting
    // gated off, "a spell of REPRIME_AFTER + 1 re-primes" goes RED by name.
    // It was built to MEASURE N, and that rationale still stands beside the new one: N had to be measured the
    // way TARGET 16 was, and RUN 6 did it (three spells in 26.3 min, longest 9 blocks -- see REPRIME_AFTER).
    // ** And the re-prime TRUNCATES the quantity to be measured, which is why the counting sits HERE and not
    // in the dry branch: ** before the threshold the ring went dry for exactly ONE block and then entered
    // priming, so any counter that stopped at "dry" read 1 every time, for ever, whatever the source did.
    // What is counted here instead is consecutive update() calls on an EMPTY RING, irrespective of priming
    // state -- during a re-prime the ring genuinely is empty until packets arrive, so counting through the
    // priming window recovers the natural dry spell exactly.
    // ★ The threshold has since made that truncation PARTIAL rather than total, and only for the spells that
    // do not matter: a dry-branch counter would now see every SUB-threshold spell exactly, and still truncate
    // the long ones at REPRIME_AFTER + 1 -- which is precisely the tail N is read off.  MEASURED, node_test
    // case 15(a)/(b).  So the placement is still right, for a narrower reason than it was written for.
    // TWO POPULATIONS, and reading one for the other is the way to misread this line:
    //   * dryTotal() counts EVERY empty block, so spec s9.3's restated bound (`under <= dryTotal`) can be
    //     checked whatever else happened;
    //   * the BUCKETS and dryMax() describe the same smaller population -- spells that ENDED because a block
    //     finally arrived, which is the only ending whose LENGTH is a measurement of the source's delivery.
    //     A spell cut short by an AVDTP SUSPEND or by the stream ending is DROPPED from both (update()'s own
    //     note says why); its blocks are still in dryTotal, so the difference is where dropped spells show up
    //     rather than nowhere.  A spell IN PROGRESS is in neither yet -- dryMax() lags a running spell.
    // Cumulative for the RUN, like every other tally on the bt_jit line: begin() resets only the in-progress
    // spell (working state), for the LIFETIME reason spelled out above m_gap/m_overEv.
    static constexpr uint8_t DRY_BUCKETS = 5;      // <=4 / <=8 / <=16 / <=32 / >32 consecutive empty blocks
    uint32_t dryMax() const { return m_dryMax; }                           // the longest RECORDED spell, in blocks
    uint32_t dryTotal() const { return m_dryTotal; }                       // every empty block, recorded spell or not
    // The 0 for i >= DRY_BUCKETS is a BOUNDS GUARD, not a reading, exactly as gapBucket()'s is.
    uint32_t dryBucket(uint8_t i) const { return i < DRY_BUCKETS ? m_dry[i] : 0; }
private:
    // PRIVATE: one caller (onMedia).  The bucket edges are pinned END TO END through gapBucket() in node_test
    // case 11, by feeding intervals that sit exactly on each inclusive top -- asserting this function against
    // itself would pin nothing that the counters do not already pin, and would freeze an implementation detail.
    static uint8_t gapBucketOf(uint32_t us) { return us <= 30000u ? 0 : us <= 50000u ? 1 : us <= 80000u ? 2 : us <= 120000u ? 3 : 4; }
    // Dry-spell LENGTHS in blocks, tops INCLUSIVE like gapBucketOf's, and pinned the same way: end to end
    // through dryBucket() in node_test case 15, on the boundary value of each of the four tops.  The edges
    // bracket the inference this instrument exists to replace -- spec s9.1 puts every dry spell this phone
    // produced at <= 25 blocks, so <=16 / <=32 straddle it and >32 is "the inference was wrong".
    static uint8_t dryBucketOf(uint32_t n) { return n <= 4u ? 0 : n <= 8u ? 1 : n <= 16u ? 2 : n <= 32u ? 3 : 4; }
    struct Blk { int16_t l[AUDIO_BLOCK_SAMPLES]; int16_t r[AUDIO_BLOCK_SAMPLES]; };
    Blk m_ring[RING]; volatile uint16_t m_head = 0, m_tail = 0;
    SbcDecoder m_dec;
    // m_servo is written ONLY by update() (the SAI ISR) after begin() has initialised it; loop() reads
    // m_servo.trim_ppm through trimPpm() for the heartbeat.  That read is a single aligned int32 of a value the
    // ISR only ever replaces wholesale, so a torn read is not expressible -- it can be one block stale, which a
    // once-a-second log line does not care about.  Nothing outside update() may WRITE it.
    sink_servo_t m_servo;
    volatile bool m_live = false;
    volatile bool m_hold = false;                  // AVDTP SUSPEND: run silent, freeze the servo, count nothing
    // Not volatile, unlike m_live/m_hold, even though update() (the SAI ISR) now reads it too -- the dry branch
    // asks it whether to re-prime.  The justification is about the WRITE side and is unchanged by that: in
    // FIRMWARE this member is written exactly once, by the initialiser below, and never again -- the sketch
    // does not call setPrefill() at all -- so the ISR reads a value that cannot change under it.  setPrefill()
    // exists for the host tests, which call it from main context BEFORE begin() publishes m_live.  A cached
    // read is therefore always the current value, which is the only thing volatile would buy here.
    bool m_prefill = PREFILL;
    // Instrument state.  m_gap*/m_lastRx*/m_overEv are written by onMedia() (main context) and read by loop() --
    // the same context m_over is written from, and volatile for the same reason given at m_under/m_over below.
    // The fill/trim extremes are written by update() (the SAI ISR) and read by loop(); they are 8/32-bit aligned
    // and volatile for that same reason.  What makes the ISR's READ-MODIFY-WRITE of them safe is NOT the store
    // width: it is that no other context writes them while the ISR can run -- begin()'s writes are fenced behind
    // m_live = false, and loop() only ever reads.  Say so rather than reasoning from width, because the width
    // argument would read as permission the day someone adds a "clear the instrument" call from loop(), and the
    // ISR's RMW would silently swallow it.
    // m_lastRxUs/m_haveRx/m_inOverrun are the instrument's own working state, read only by their writer.
    uint32_t m_lastRxUs = 0; bool m_haveRx = false;
    volatile uint32_t m_gapMaxUs = 0, m_gap[GAP_BUCKETS] = {};
    volatile uint8_t m_fillMin = (uint8_t)RING, m_fillMax = 0;
    volatile int32_t m_trimLo = 0, m_trimHi = 0;
    volatile uint32_t m_overEv = 0; bool m_inOverrun = false;
    // The pre-fill's state.  All FOUR are written by update() (the SAI ISR) and read from main context -- the
    // host tests today, loop()'s heartbeat once Task 6 prints them -- so they are volatile for the reason the
    // block above gives.  begin() writes the first three and end() writes m_priming, and those writes are safe
    // for that block's OTHER reason and no other: both fence themselves behind m_live = false, so the ISR cannot
    // be inside the branch that touches them.  m_reprimes is the exception in the other direction: only the ISR
    // ever writes it (the re-prime), because it is a LIFETIME event count and begin() must not reset it.
    volatile bool m_priming = false, m_primed = false;
    volatile uint32_t m_primeBlocks = 0, m_reprimes = 0;
    // The dry-spell instrument.  Written by update() (the SAI ISR) and read from main context, volatile for
    // the reason the instrument block above gives; safe under the ISR's read-modify-write for that block's
    // OTHER reason and no other -- no context outside update() writes the TALLIES.  m_dryRun is the spell in
    // PROGRESS: working state, not a tally, so it is the one thing begin() resets -- and it is volatile for
    // THAT, not because anything reads it (nothing outside update() does).  It has two writers in two
    // contexts, the ISR and begin(), which is the same shape as m_priming/m_primeBlocks and the file's line
    // for volatile; m_haveRx and m_inOverrun look like the same pattern and are not, since both of their
    // writers are main context.
    // uint32 cannot wrap here: m_dryRun climbs at the 344 Hz block rate, so 2^32 blocks is 145 days of an
    // unbroken dry spell -- and the wrap would cost one mis-bucketed spell, not a corrupt counter, so it is
    // stated rather than guarded with an ISR compare that could never fire.
    volatile uint32_t m_dryMax = 0, m_dryTotal = 0, m_dry[DRY_BUCKETS] = {}, m_dryRun = 0;
    volatile int32_t m_applied = 0;                // last ppm handed to the PLL, so update() only writes on a change
    uint16_t m_lastSeq = 0; bool m_haveSeq = false;
    uint8_t m_frag[1100]; uint16_t m_fragLen = 0; bool m_fragging = false;  // a fragmented SBC frame being reassembled across packets
    // m_under is written by update() (the SAI ISR) and read by loop(); m_over by onMedia() (main context) and
    // read by loop().  Both are volatile so the compiler cannot cache either across the heartbeat's read -- a
    // counter that never appears to move is the one shape of instrument failure that reads as good news.  Every
    // instrument counter above carries volatile for exactly this reason and no other; they are not shared with
    // any context that would need more than that.
    volatile uint32_t m_under = 0, m_over = 0;
    uint32_t m_pkts = 0, m_frames = 0, m_seqGaps = 0, m_bad = 0, m_rmsAcc = 0, m_rmsBlocks = 0, m_crc = 0xFFFFFFFFu, m_crcBlocks = 0;
    void pushFrame(const uint8_t *f, uint16_t len);
};
