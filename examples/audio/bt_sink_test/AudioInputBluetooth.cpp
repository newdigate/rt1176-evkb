#include "AudioInputBluetooth.h"
static uint32_t crc32Update(uint32_t c, const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); } return c; }
void AudioInputBluetooth::begin() {
    // m_live goes FALSE first, before anything else is touched.  update() runs from the SAI ISR and can land
    // between the head/tail reset and the rest of this function; with m_live still true from a previous stream
    // it would read a half-reset ring (and count an underrun on it).  Closing that window structurally costs
    // one store and removes the need to reason about the interleaving at all.
    m_live = false;
    m_hold = false;
    m_head = m_tail = 0; m_dec.reset(); m_haveSeq = false; m_fragLen = 0; m_fragging = false;
    servo_init(&m_servo, TARGET); m_applied = m_servo.trim_ppm; audioPllTrimPpm(m_servo.trim_ppm);
    m_live = true;                                 // ... and last: everything update() reads is settled by here
}
void AudioInputBluetooth::end() { m_live = false; m_head = m_tail = 0; m_fragLen = 0; m_fragging = false; }   // the trim HOLDS (servo_step is skipped while !m_live)
void AudioInputBluetooth::pushFrame(const uint8_t *f, uint16_t len) {
    // The decoder writes blocks*subbands samples per channel and reads the block count from the FRAME HEADER, not
    // from the AVDTP configuration -- a peer that sends 4-block frames after negotiating 16 would leave 96 of the
    // 128 samples in this slot STALE and we would transmit them as audio.  Refuse anything that is not exactly one
    // Audio-library block.  (SbcDecoder itself refuses 4-subband and SNR frames.)
    Sbc::Params p;
    if (!SbcDecoder::parseHeader(f, len, p)) { m_bad++; return; }
    if ((uint16_t)p.blocks * p.subbands != AUDIO_BLOCK_SAMPLES) { m_bad++; return; }
    uint16_t head = m_head, next = (uint16_t)((head + 1) % RING);
    // Ring full: DROP THE NEW FRAME.  Dropping the oldest instead would mean the producer writing m_tail, which
    // the ISR consumer owns -- that is a genuine race, not a style point, and it buys nothing: the ring size
    // already bounds latency, and the servo is what holds the fill near TARGET.  A standing m_over count is the
    // signal that the sink is consuming too slowly, which is the servo's job to correct.
    if (next == m_tail) { m_over++; return; }
    // decode() returns the frame LENGTH consumed (0 = refused: bad sync, CRC, truncated, unsupported).
    if (m_dec.decode(f, len, m_ring[head].l, m_ring[head].r) == 0) { m_bad++; return; }
    if (m_crcBlocks < 200) { m_crc = crc32Update(m_crc, (const uint8_t *)m_ring[head].l, sizeof m_ring[head].l); m_crcBlocks++; }
    uint32_t sum = 0;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { int32_t v = m_ring[head].l[i]; sum += (uint32_t)(v < 0 ? -v : v); }
    m_rmsAcc += sum / AUDIO_BLOCK_SAMPLES; m_rmsBlocks++;
    m_head = next; m_frames++;                     // publish AFTER the decode (SPSC)
}
void AudioInputBluetooth::onMedia(const uint8_t *p, uint16_t len) {
    if (!m_live || len < 13) return;                                                       // 12-byte RTP header + 1 media header
    if ((p[0] >> 6) != 2) { m_bad++; return; }                                             // RTP v2
    uint32_t hdr = 12u + 4u * (uint32_t)(p[0] & 0x0F);                                     // CSRC count
    if (p[0] & 0x10) {                                                                     // X: a header extension follows the CSRCs
        if (hdr + 4u > len) { m_bad++; return; }
        hdr += 4u + 4u * (uint32_t)((p[hdr + 2] << 8) | p[hdr + 3]);                        // length is in 32-bit words
    }
    if (hdr + 1u > len) { m_bad++; return; }                                               // no room for the SBC media header
    uint16_t seq = (uint16_t)((p[2] << 8) | p[3]);
    if (m_haveSeq && seq != (uint16_t)(m_lastSeq + 1)) m_seqGaps++;
    m_lastSeq = seq; m_haveSeq = true; m_pkts++;
    uint8_t mh = p[hdr]; bool F = (mh & 0x80) != 0, S = (mh & 0x40) != 0, L = (mh & 0x20) != 0; uint8_t n = (uint8_t)(mh & 0x0F);
    const uint8_t *q = p + hdr + 1; uint16_t rem = (uint16_t)(len - hdr - 1);
    if (F) {                                                                               // a fragmented frame: gather until L
        if (S) { m_fragLen = 0; m_fragging = true; }
        else if (!m_fragging) { m_bad++; return; }                                         // joined mid-frame: no START seen, drop it
        if ((uint32_t)m_fragLen + rem <= sizeof m_frag) { memcpy(m_frag + m_fragLen, q, rem); m_fragLen = (uint16_t)(m_fragLen + rem); }
        else { m_fragLen = 0; m_fragging = false; m_bad++; return; }
        if (L) { if (m_fragLen) pushFrame(m_frag, m_fragLen); m_fragLen = 0; m_fragging = false; }
        return;
    }
    m_fragging = false; m_fragLen = 0;                                                     // an unfragmented packet abandons any partial
    for (uint8_t i = 0; i < n && rem >= 4; i++) {                                          // n whole frames back to back
        Sbc::Params fp; if (!SbcDecoder::parseHeader(q, rem, fp)) { m_bad++; break; }
        uint16_t fl = Sbc::frameLength(fp);
        if (fl == 0 || fl > rem) { m_bad++; break; }                                       // fl==0 would not advance q: the loop must make progress
        pushFrame(q, fl); q += fl; rem = (uint16_t)(rem - fl);
    }
}
void AudioInputBluetooth::update(void) {
    audio_block_t *l = allocate(), *r = allocate();
    if (!l || !r) { if (l) release(l); if (r) release(r); return; }
    uint16_t tail = m_tail;
    // HELD (the source SUSPENDed) is silence that is NOT an underrun: nothing is missing, the source stopped on
    // purpose.  Counting it would bury the real underruns under 344 of these a second, and the ring is left
    // alone so a RESUME plays what is already in it.
    bool run = m_live && !m_hold;
    if (!run || tail == m_head) { memset(l->data, 0, sizeof l->data); memset(r->data, 0, sizeof r->data); if (run) m_under++; }
    else { memcpy(l->data, m_ring[tail].l, sizeof l->data); memcpy(r->data, m_ring[tail].r, sizeof r->data); m_tail = (uint16_t)((tail + 1) % RING); }
    // The servo runs once per audio block -- this IS the block clock -- and only while the link is up and not
    // held: with the link down or the source suspended the trim is frozen where it was, so a resumed stream
    // starts from the rate it had learned.  (servo_step's own `hold` argument expresses the same freeze; the
    // node skips the call outright so a held stream costs nothing at all in the ISR.)
    if (run) { int32_t t = servo_step(&m_servo, fill(), 0); if (t != m_applied) { m_applied = t; audioPllTrimPpm(t); } }
    transmit(l, 0); transmit(r, 1); release(l); release(r);
}
