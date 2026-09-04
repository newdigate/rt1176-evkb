// AudioOutputBluetooth -- an AudioStream sink node that encodes its two inputs to
// SBC and streams them on an A2DP media channel via MediaPacketizer.  update() (the
// audio clock) only COPIES each stereo block into a PCM ring; poll() (the main loop)
// does the SBC encode and drains to L2CAP.  The encode is deliberately kept OUT of
// update(): when another sink (e.g. AudioOutputI2S) clocks the graph, update() runs in
// that sink's DMA ISR, and a flash-resident encode there overran the 2.9 ms block period
// and livelocked the main loop so no media ever shipped (silicon 2026-09-04, acid_box).
// Nothing in the ISR touches the transport.  MIT.
#pragma once
#include <Arduino.h>      // AudioStream.h needs F_CPU_ACTUAL/IRQ_NUMBER_t from this first
#include <string.h>       // memcpy for the PCM hand-off ring
#include "AudioStream.h"
#include "Sbc.h"
#include "MediaPacketizer.h"
#include "L2cap.h"

static_assert(AUDIO_BLOCK_SAMPLES == 128,
    "AudioOutputBluetooth assumes one Audio-library block == one SBC frame "
    "(128 samples/channel, A2DP v1.3 sec 12); AUDIO_BLOCK_SAMPLES has moved.");

class A2dpSource;
class AudioOutputBluetooth : public AudioStream {
public:
    AudioOutputBluetooth() : AudioStream(2, inputQueueArray) {}
    void begin(L2cap &l2, uint16_t mediaCid, uint16_t mediaMtu, const Sbc::Params &p);
    void begin(A2dpSource &src);                 // sugar: pulls l2/cid/mtu/params from src
    virtual void update(void);                   // audio clock (maybe an ISR): copy PCM into the ring
    void poll();                                 // main loop: encode ring -> pk.push -> l2.send
    // Self-clock (default): poll() calls AudioStream::update_all() to drive the graph,
    // for a graph with no hardware audio clock (bt_tone_test).  Set false when another
    // sink (e.g. AudioOutputI2S) already clocks the graph from its DMA ISR -- then poll()
    // does NOT clock the graph, it only encodes the buffered PCM and drains.  Either way
    // update() just copies PCM into the ring.  Call before begin().
    void setSelfClock(bool on) { m_selfClock = on; }
    bool     connected()      const { return m_l2 != nullptr && m_cid != 0; }
    uint32_t blocks()         const { return m_blocks; }
    uint32_t packets()        const { return m_pk.packets(); }
    uint32_t drops()          const { return m_pk.drops(); }
    uint32_t pcmDrops()       const { return m_pcmDrops; }   // PCM-ring overflow: loop too slow to encode
    uint8_t  queueHighWater() const { return m_pk.queueHighWater(); }
    uint16_t framesPerPacket() const { return m_pk.framesPerPacket(); }
private:
    static bool sendThunk(void *ctx, const uint8_t *pkt, uint16_t len);
    // This graph has no I2S/DMA output to drive AudioStream::update_all() the
    // way every other Audio example relies on -- AudioOutputBluetooth IS the
    // sink, so it must clock the graph itself. It does so from poll() (the main
    // loop), paced by micros() at the audio block period. * It does NOT use
    // IntervalTimer: on the imxrt1176 core the PIT period runs ~20x too fast
    // (measured on silicon 2026-09-03 -- blocks arrived at ~6890/s vs the 344/s
    // AUDIO_SAMPLE_RATE_EXACT/AUDIO_BLOCK_SAMPLES calls for), and an ISR-driven
    // graph at that rate ALSO starves the main loop so L2cap never returns ACL
    // credits and only one media packet ever ships. Main-loop pacing fixes both
    // -- and update_all() still pends IRQ_SOFTWARE, so the actual graph walk
    // runs at the right (software-ISR) priority, just triggered cooperatively.
    static bool s_setupDone;                 // AudioStream::update_setup() once, globally
    uint32_t m_usPerBlock = 0;               // audio block period in microseconds (~2902)
    uint32_t m_nextUpdate = 0;               // micros() deadline for the next block
    uint32_t m_flushUs = 0;                  // drain-flush deadline: bound the batching latency
    uint32_t m_lastDrainUs = 0;              // micros() of the last drain, for the flush timeout
    bool m_selfClock = true;                 // false = an external ISR clocks the graph; poll() drains only
    audio_block_t *inputQueueArray[2];
    Sbc m_sbc; MediaPacketizer m_pk;
    L2cap *m_l2 = nullptr; uint16_t m_cid = 0;
    uint32_t m_blocks = 0;
    // PCM hand-off ring: update() (the audio clock -- an external DMA ISR when another sink
    // clocks the graph) copies each stereo block in (cheap); poll() (the main loop) encodes
    // them to SBC (expensive) and drains.  Single-producer/single-consumer: the head index
    // is written only by update(), the tail only by poll(), so no lock is needed on a
    // Cortex-M (aligned 16-bit loads/stores are atomic).  Sized to ~93 ms so a slow UI frame
    // (acid_box's GC355 compositor) can stall the consumer without overflowing.
    static constexpr uint16_t PCM_RING = 32;
    struct PcmBlock { int16_t l[AUDIO_BLOCK_SAMPLES]; int16_t r[AUDIO_BLOCK_SAMPLES]; };
    PcmBlock m_pcm[PCM_RING];
    volatile uint16_t m_pcmHead = 0;         // producer (update)
    volatile uint16_t m_pcmTail = 0;         // consumer (poll)
    uint32_t m_pcmDrops = 0;                  // PCM-ring overflow count
};
