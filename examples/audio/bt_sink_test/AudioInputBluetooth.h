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
#define BT_SINK_RING 32
#endif
#ifndef BT_SINK_TARGET
#define BT_SINK_TARGET 16
#endif
#ifndef BT_SINK_PREFILL
#define BT_SINK_PREFILL 1
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
    void hold(bool on) { m_hold = on; }
    bool held() const { return m_hold; }
    void onMedia(const uint8_t *rtp, uint16_t len);   // A2dpSink's media callback target (main context)
    virtual void update(void);                    // the SAI ISR: pop a block (or silence) + servo
    uint32_t pkts() const { return m_pkts; } uint32_t frames() const { return m_frames; } uint32_t seqGaps() const { return m_seqGaps; }
    uint32_t underruns() const { return m_under; } uint32_t overruns() const { return m_over; } uint32_t badFrames() const { return m_bad; }
    uint8_t  fill() const { uint16_t h = m_head, t = m_tail; return (uint8_t)((h + RING - t) % RING); }
    int32_t  trimPpm() const { return m_servo.trim_ppm; }
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
    static constexpr uint16_t RING = BT_SINK_RING, TARGET = BT_SINK_TARGET;
    static constexpr bool PREFILL = (BT_SINK_PREFILL) != 0;
    static_assert(RING >= 4 && RING <= 256 && TARGET >= 1 && TARGET <= RING - 2,
        "AudioInputBluetooth: TARGET must leave two slots below RING (one is the SPSC sentinel), and RING "
        "must fit fill()'s uint8_t return.  The one-packet-of-headroom claim is TARGET's, not this bound's: "
        "it is asserted in node_test case 14, because the bench's CONTROL arm (16/8) deliberately breaks it.");
    // Whether begin() pre-fills to TARGET before the first pop, and whether a dry ring re-primes (Task 3/4 of the
    // NEW-42 plan).  Defaults to the compile-time PREFILL; the sketch never calls this, the host tests do, so that
    // the ring/parser cases run without the pre-fill and the pre-fill cases run with it, in BOTH CMake arms.
    void setPrefill(bool on) { m_prefill = on; }
private:
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
    // Not volatile, unlike m_live/m_hold: it is written from main context BEFORE begin() publishes m_live,
    // never while update() is running, so no ISR can observe it change.  Nothing reads it yet -- begin() and
    // update() take it up in Task 3/4 of the NEW-42 plan.
    bool m_prefill = PREFILL;
    volatile int32_t m_applied = 0;                // last ppm handed to the PLL, so update() only writes on a change
    uint16_t m_lastSeq = 0; bool m_haveSeq = false;
    uint8_t m_frag[1100]; uint16_t m_fragLen = 0; bool m_fragging = false;  // a fragmented SBC frame being reassembled across packets
    // m_under is written by update() (the SAI ISR) and read by loop(); m_over by onMedia() (main context) and
    // read by loop().  Both are volatile so the compiler cannot cache either across the heartbeat's read -- a
    // counter that never appears to move is the one shape of instrument failure that reads as good news.
    volatile uint32_t m_under = 0, m_over = 0;
    uint32_t m_pkts = 0, m_frames = 0, m_seqGaps = 0, m_bad = 0, m_rmsAcc = 0, m_rmsBlocks = 0, m_crc = 0xFFFFFFFFu, m_crcBlocks = 0;
    void pushFrame(const uint8_t *f, uint16_t len);
};
