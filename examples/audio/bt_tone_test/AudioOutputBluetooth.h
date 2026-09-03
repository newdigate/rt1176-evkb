// AudioOutputBluetooth -- an AudioStream sink node that encodes its two inputs to
// SBC and streams them on an A2DP media channel via MediaPacketizer.  update() runs
// in the audio ISR (encode + enqueue only); poll() runs in the main loop (drain to
// L2CAP).  Nothing in the ISR touches the transport.  MIT.
#pragma once
#include <Arduino.h>      // AudioStream.h needs F_CPU_ACTUAL/IRQ_NUMBER_t from this first
#include "AudioStream.h"
#include "Sbc.h"
#include "MediaPacketizer.h"
#include "L2cap.h"
#include <IntervalTimer.h>

static_assert(AUDIO_BLOCK_SAMPLES == 128,
    "AudioOutputBluetooth assumes one Audio-library block == one SBC frame "
    "(128 samples/channel, A2DP v1.3 sec 12); AUDIO_BLOCK_SAMPLES has moved.");

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
    // This graph has no I2S/DMA output to drive AudioStream::update_all() the
    // way every other Audio example relies on -- AudioOutputBluetooth IS the
    // sink, so it has to be its own clock, the same role AudioOutputI2S's DMA
    // ISR plays elsewhere. Started once, in begin(), at the audio block
    // period; never stopped (matches the lifetime of every other node's
    // implicit clock). IntervalTimer priority is fine for the QEMU gate as
    // committed -- may want tuning against other ISRs on silicon.
    static IntervalTimer s_timer;
    static bool s_clockRunning;
    static void audioClockISR();
    audio_block_t *inputQueueArray[2];
    Sbc m_sbc; MediaPacketizer m_pk;
    L2cap *m_l2 = nullptr; uint16_t m_cid = 0;
    uint32_t m_blocks = 0;
};
