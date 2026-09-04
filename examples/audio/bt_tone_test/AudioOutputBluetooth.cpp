#include "AudioOutputBluetooth.h"
#include "A2dpSource.h"

bool AudioOutputBluetooth::s_setupDone = false;

void AudioOutputBluetooth::begin(L2cap &l2, uint16_t cid, uint16_t mtu, const Sbc::Params &p) {
    // * L2cap::send() DROPS any payload larger than its Tx buffer
    // (L2cap::MAX_PAYLOAD): basic mode does one ACL packet per SDU, no
    // fragmentation.  The AVDTP-negotiated media MTU (src.mediaMtu()) can exceed
    // that, so an oversize media packet would be silently dropped and media would
    // stall (measured on silicon 2026-09-03 against MAX_PAYLOAD=700: packets froze
    // at ~17, cred idle, NCP frozen -- NOT a flow-control problem).  Cap the
    // packetiser at the L2CAP send limit so it batches only frames that fit.
    if (mtu > L2cap::MAX_PAYLOAD) mtu = L2cap::MAX_PAYLOAD;
    m_l2 = &l2; m_cid = cid; m_sbc.begin(p); m_pk.begin(mtu); m_blocks = 0;
    m_pcmHead = m_pcmTail = 0; m_pcmDrops = 0;
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
    // Runs in whatever context clocks the graph: poll()'s cooperative update_all() when
    // self-clocked (bt_tone_test), or another sink's DMA ISR when not (acid_box's
    // AudioOutputI2S).  Keep it CHEAP -- copy the stereo block into the PCM ring and
    // return.  The SBC encode is done by poll() in the main loop; doing it here overran
    // the block period in the SAI ISR and livelocked the loop (silicon 2026-09-04).
    audio_block_t *l = receiveReadOnly(0), *r = receiveReadOnly(1);
    // Not streaming yet (begin() not called): drop the input.  When an EXTERNAL clock
    // drives the graph, update() runs from power-on -- buffering into a ring that is never
    // drained (m_l2 null) would wrap the head past the tail forever, and even the memcpy is
    // wasted before there is a channel to send on.
    if (!m_l2) { if (l) release(l); if (r) release(r); return; }
    uint16_t head = m_pcmHead;
    uint16_t next = head + 1; if (next >= PCM_RING) next = 0;
    if (next == m_pcmTail) {
        // Ring full: the main-loop encoder fell behind (a long UI frame).  Drop this block
        // and count it rather than overwrite one the consumer has not read.  This is the
        // graceful-degradation signal that the CM7 is over budget -- distinct from m_pk
        // drops (a send/credit stall downstream of the encode).
        m_pcmDrops++;
    } else {
        static const int16_t silence[AUDIO_BLOCK_SAMPLES] = {0};
        const int16_t *L = l ? l->data : silence;
        const int16_t *R = r ? r->data : silence;
        memcpy(m_pcm[head].l, L, sizeof(m_pcm[head].l));
        memcpy(m_pcm[head].r, R, sizeof(m_pcm[head].r));
        m_pcmHead = next;                              // publish AFTER the copy (SPSC)
    }
    if (l) release(l);
    if (r) release(r);
}
bool AudioOutputBluetooth::sendThunk(void *ctx, const uint8_t *pkt, uint16_t len) {
    AudioOutputBluetooth *o = (AudioOutputBluetooth *)ctx;
    const bool ok = o->m_l2->send(o->m_cid, pkt, len);   // L2cap::send returns false when out of credit/txq
    if (ok) o->m_txBytes += 9u + len;                    // + the ACL header L2cap::service() prepends
    return ok;
}
void AudioOutputBluetooth::poll() {
    if (!m_l2) return;
    uint32_t now = micros();
    if (m_selfClock) {
        // No hardware audio clock (bt_tone_test): drive the graph here, paced by micros().
        // update_all() pends IRQ_SOFTWARE, whose handler walks the graph -> our update()
        // copies one block into the PCM ring.  Resync to now after a stall rather than
        // bursting several update_all()s and overrunning the ring.
        if ((int32_t)(now - m_nextUpdate) >= 0) {
            m_nextUpdate += m_usPerBlock;
            if ((int32_t)(now - m_nextUpdate) > (int32_t)(4 * m_usPerBlock)) m_nextUpdate = now + m_usPerBlock;
            AudioStream::update_all();
        }
    }
    // else: an external sink's ISR (AudioOutputI2S) already called update_all() this period,
    // so update() has copied its block(s) into the PCM ring; poll() encodes and drains below.
    //
    // Encode every PCM block buffered since the last poll.  This is the expensive step
    // (SBC), deliberately in the main loop: encoding in the audio ISR overran the block
    // period and livelocked the loop when the graph was externally clocked (silicon
    // 2026-09-04, acid_box).  A snapshot of head bounds the loop so it cannot spin against
    // a producer that keeps filling the ring during a long encode.
    uint16_t head = m_pcmHead;
    if (m_pcmTail != head) {
        const uint32_t t0 = micros();
        while (m_pcmTail != head) {
            uint16_t tail = m_pcmTail;
            uint8_t frame[128];
            uint16_t n = m_sbc.encode(m_pcm[tail].l, m_pcm[tail].r, frame);
            m_pk.push(frame, n); m_blocks++;
            uint16_t next = tail + 1; if (next >= PCM_RING) next = 0;
            m_pcmTail = next;                              // release the slot AFTER the encode (SPSC)
        }
        m_encodeUs += micros() - t0;
    }
    // Drain when a full packet's worth of frames is ready (so packets are fuller and use
    // fewer ACL credits), or when the flush deadline passes (bounds latency and empties a
    // backlog).  drain() itself still batches up to framesPerPacket and, once triggered,
    // sends every full packet it can -- so a credit-stall backlog drains fast on recovery.
    if (m_pk.pending() >= m_pk.framesPerPacket() || (int32_t)(now - (m_lastDrainUs + m_flushUs)) >= 0) {
        const uint32_t t0 = micros();
        m_pk.drain(sendThunk, this);
        m_drainUs += micros() - t0;
        m_lastDrainUs = now;
    }
}
