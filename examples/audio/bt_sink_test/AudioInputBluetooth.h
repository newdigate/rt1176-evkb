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
// The example is rt1176-only; this keeps the node HOST-COMPILABLE (tests/node_compile.sh) and buildable on any
// other board, where there is no trimmable audio PLL to talk to.
static inline int32_t audioPllTrimPpm(int32_t) { return 0; }
#endif
static_assert(AUDIO_BLOCK_SAMPLES == 128,
    "AudioInputBluetooth assumes one SBC frame (16 blocks x 8 subbands) == one Audio-library block; "
    "AUDIO_BLOCK_SAMPLES has moved.");
class AudioInputBluetooth : public AudioStream {
public:
    AudioInputBluetooth() : AudioStream(0, nullptr) {}
    void begin();                                 // stream started: reset ring/decoder, recentre the servo, start counting
    void end();                                   // stream lost/closed: clear the ring, hold the trim
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
    static constexpr uint16_t RING = 16, TARGET = 8;
private:
    struct Blk { int16_t l[AUDIO_BLOCK_SAMPLES]; int16_t r[AUDIO_BLOCK_SAMPLES]; };
    Blk m_ring[RING]; volatile uint16_t m_head = 0, m_tail = 0;
    SbcDecoder m_dec; sink_servo_t m_servo; bool m_live = false;
    int32_t m_applied = 0;                         // last ppm handed to the PLL, so update() only writes on a change
    uint16_t m_lastSeq = 0; bool m_haveSeq = false;
    uint8_t m_frag[1100]; uint16_t m_fragLen = 0; bool m_fragging = false;  // a fragmented SBC frame being reassembled across packets
    uint32_t m_pkts = 0, m_frames = 0, m_seqGaps = 0, m_under = 0, m_over = 0, m_bad = 0, m_rmsAcc = 0, m_rmsBlocks = 0, m_crc = 0xFFFFFFFFu, m_crcBlocks = 0;
    void pushFrame(const uint8_t *f, uint16_t len);
};
