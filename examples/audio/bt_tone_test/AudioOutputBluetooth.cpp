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
    if (l) release(l);
    if (r) release(r);
}
bool AudioOutputBluetooth::sendThunk(void *ctx, const uint8_t *pkt, uint16_t len) {
    AudioOutputBluetooth *o = (AudioOutputBluetooth *)ctx;
    return o->m_l2->send(o->m_cid, pkt, len);      // L2cap::send returns false when out of credit/txq
}
void AudioOutputBluetooth::poll() { if (m_l2) m_pk.drain(sendThunk, this); }
