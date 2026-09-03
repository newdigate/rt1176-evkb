#include "AudioOutputBluetooth.h"
#include "A2dpSource.h"

IntervalTimer AudioOutputBluetooth::s_timer;
bool AudioOutputBluetooth::s_clockRunning = false;

// The block-rate tick a hardware output node's DMA ISR would otherwise
// provide. update_all() just pends IRQ_SOFTWARE; software_isr() (attached by
// AudioStream::update_setup(), called below) does the actual walk over every
// active node, this one included.
void AudioOutputBluetooth::audioClockISR() { AudioStream::update_all(); }

void AudioOutputBluetooth::begin(L2cap &l2, uint16_t cid, uint16_t mtu, const Sbc::Params &p) {
    m_l2 = &l2; m_cid = cid; m_sbc.begin(p); m_pk.begin(mtu); m_blocks = 0;
    // Started once, on the first successful bring-up. ★ begin() is only ever
    // reached after a2dp=ok (bt_tone_test.cpp gates it on A2dpSource::connect()
    // succeeding), so the card-absent path never calls this: the graph stays
    // idle and run_qemu.sh's vacuity assertions (streaming never claimed,
    // blocks/packets/drops all 0) still hold with no card to answer.
    if (!s_clockRunning) {
        AudioStream::update_setup();     // attach + enable IRQ_SOFTWARE (never done otherwise --
                                          // this graph has no I2S device to do it for us)
        float usPerBlock = 1.0e6f * (float)AUDIO_BLOCK_SAMPLES / AUDIO_SAMPLE_RATE_EXACT;
        s_timer.begin(audioClockISR, usPerBlock);   // ~2902 us at 44.1k/128
        s_clockRunning = true;
    }
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
    if (l) release(l);
    if (r) release(r);
}
bool AudioOutputBluetooth::sendThunk(void *ctx, const uint8_t *pkt, uint16_t len) {
    AudioOutputBluetooth *o = (AudioOutputBluetooth *)ctx;
    return o->m_l2->send(o->m_cid, pkt, len);      // L2cap::send returns false when out of credit/txq
}
void AudioOutputBluetooth::poll() { if (m_l2) m_pk.drain(sendThunk, this); }
