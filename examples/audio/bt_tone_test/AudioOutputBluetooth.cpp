#include "AudioOutputBluetooth.h"
#include "A2dpSource.h"

bool AudioOutputBluetooth::s_setupDone = false;

void AudioOutputBluetooth::begin(L2cap &l2, uint16_t cid, uint16_t mtu, const Sbc::Params &p) {
    // ★ L2cap::send() DROPS any payload larger than its Tx buffer
    // (L2cap::MAX_PAYLOAD): basic mode does one ACL packet per SDU, no
    // fragmentation.  The AVDTP-negotiated media MTU (src.mediaMtu()) can exceed
    // that, so an oversize media packet would be silently dropped and media would
    // stall (measured on silicon 2026-09-03 against MAX_PAYLOAD=700: packets froze
    // at ~17, cred idle, NCP frozen -- NOT a flow-control problem).  Cap the
    // packetiser at the L2CAP send limit so it batches only frames that fit.
    if (mtu > L2cap::MAX_PAYLOAD) mtu = L2cap::MAX_PAYLOAD;
    m_l2 = &l2; m_cid = cid; m_sbc.begin(p); m_pk.begin(mtu); m_blocks = 0;
    // begin() is only reached after a2dp=ok (bt_tone_test.cpp gates it on
    // A2dpSource::connect() succeeding), so the card-absent path never calls
    // this: the graph stays idle and run_qemu.sh's vacuity assertions still hold.
    // A BT sink is a SOFTWARE output -- no I2S/DMA drives the graph -- so poll()
    // clocks it from the main loop via micros() (see the header for why NOT
    // IntervalTimer). update_setup() attaches IRQ_SOFTWARE once; the actual graph
    // walk runs there when poll() calls update_all().
    if (!s_setupDone) { AudioStream::update_setup(); s_setupDone = true; }
    m_usPerBlock = (uint32_t)(1.0e6f * (float)AUDIO_BLOCK_SAMPLES / AUDIO_SAMPLE_RATE_EXACT + 0.5f);  // ~2902
    m_nextUpdate = micros();
    // Batch to full packets: hold frames until framesPerPacket() are ready, so each ACL
    // packet carries several SBC frames and uses ONE credit -- ~perPkt-fold less credit
    // churn than one-frame packets, which on silicon ran the 7-credit pool down to 0 and
    // made brief link stalls drop (2026-09-04).  The flush deadline bounds the added
    // latency to the time it takes to produce a full packet (~perPkt block periods).
    m_flushUs = (uint32_t)m_pk.framesPerPacket() * m_usPerBlock;
    m_lastDrainUs = micros();
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
void AudioOutputBluetooth::poll() {
    if (!m_l2) return;
    // Clock the graph at the audio block rate: at most one block per call (poll()
    // runs far more often than 344 Hz).  update_all() pends IRQ_SOFTWARE, whose
    // handler walks the graph -> our update() pushes one SBC frame.  If we fell
    // more than a few blocks behind (a loop stall), resync to now rather than
    // bursting and overflowing the ring.
    uint32_t now = micros();
    if ((int32_t)(now - m_nextUpdate) >= 0) {
        m_nextUpdate += m_usPerBlock;
        if ((int32_t)(now - m_nextUpdate) > (int32_t)(4 * m_usPerBlock)) m_nextUpdate = now + m_usPerBlock;
        AudioStream::update_all();
    }
    // Drain when a full packet's worth of frames is ready (so packets are fuller and use
    // fewer ACL credits), or when the flush deadline passes (bounds latency and empties a
    // backlog).  drain() itself still batches up to framesPerPacket and, once triggered,
    // sends every full packet it can -- so a credit-stall backlog drains fast on recovery.
    if (m_pk.pending() >= m_pk.framesPerPacket() || (int32_t)(now - (m_lastDrainUs + m_flushUs)) >= 0) {
        m_pk.drain(sendThunk, this);
        m_lastDrainUs = now;
    }
}
