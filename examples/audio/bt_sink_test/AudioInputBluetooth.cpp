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
    // The instrument's TALLIES and EXTREMES are LIFETIME, like m_over/m_under/m_pkts beside which they are
    // printed: begin() runs on EVERY stream start (bt_sink_test.cpp's onStreamCb), so resetting them here would
    // put `overev=0` next to `over=89` after a reconnect and make a heartbeat delta jump backwards.  (CLAUDE.md
    // records the same footgun from L2cap's l2frag, which does reset per attempt and needs a warning saying so.)
    // Only these two FLAGS reset, because a stale one produces a WRONG reading rather than a stale one:
    // m_haveRx would time the first packet of this stream against the last packet of the previous one -- an
    // interval of seconds, landing in the >120 ms bucket and pinning gapMaxUs at something that is not jitter.
    // m_inOverrun is cleared for the same structural reason, though it is NOT independently observable today:
    // the ring is emptied two lines above, so the new stream cannot overrun until RING-1 frames have landed and
    // each landing frame clears the latch itself.  Resetting it keeps that invariant local to begin() instead of
    // resting on the ring reset, and there is deliberately no host case pinning it -- there is nothing to pin.
    m_haveRx = false; m_inOverrun = false;
    servo_init(&m_servo, TARGET); m_applied = m_servo.trim_ppm; audioPllTrimPpm(m_servo.trim_ppm);
    // Arm the START pre-fill for THIS stream (NEW-42).  m_primeBlocks resets with it because it is a DURATION
    // and not a tally -- it measures one prime, and this is where a prime begins.  m_reprimes does NOT reset,
    // for the reason given above about the EVENT counters it will be printed beside.
    m_priming = m_prefill; m_primed = false; m_primeBlocks = 0;
    m_live = true;                                 // ... and last: everything update() reads is settled by here
}
// The trim HOLDS (servo_step is skipped while !m_live).  m_priming is cleared because priming() is an
// OBSERVABLE: with the stream gone, update() would do nothing with the flag either way, but a reader between
// end() and the next begin() must not be told a prime is running on a stream that no longer exists.
// m_primed and m_primeBlocks deliberately do NOT clear: they are the ENDED stream's reading, and the header
// says the duration stands until the next begin().  So `priming=0 primed=1 prime_ms=<the last stream's>` is
// the correct idle reading, not a leak.  It cannot be pinned by a case -- the next begin() clears both before
// any check could run -- so it is pinned here, in words.
void AudioInputBluetooth::end() { m_live = false; m_head = m_tail = 0; m_fragLen = 0; m_fragging = false; m_priming = false; }
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
    if (next == m_tail) { m_over++; if (!m_inOverrun) { m_inOverrun = true; m_overEv++; } return; }
    // decode() returns the frame LENGTH consumed (0 = refused: bad sync, CRC, truncated, unsupported).
    if (m_dec.decode(f, len, m_ring[head].l, m_ring[head].r) == 0) { m_bad++; return; }
    if (m_crcBlocks < 200) { m_crc = crc32Update(m_crc, (const uint8_t *)m_ring[head].l, sizeof m_ring[head].l); m_crcBlocks++; }
    uint32_t sum = 0;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { int32_t v = m_ring[head].l[i]; sum += (uint32_t)(v < 0 ? -v : v); }
    m_rmsAcc += sum / AUDIO_BLOCK_SAMPLES; m_rmsBlocks++;
    m_head = next; m_frames++;                     // publish AFTER the decode (SPSC)
    m_inOverrun = false;                           // a frame landed: the run of drops, if any, is over
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
    // The source's delivery cadence, measured on every ACCEPTED packet (a rejected header is not a delivery).
    uint32_t now = micros();
    if (m_haveRx) { uint32_t d = now - m_lastRxUs; if (d > m_gapMaxUs) m_gapMaxUs = d; m_gap[gapBucketOf(d)]++; }
    m_lastRxUs = now; m_haveRx = true;
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
    bool popped = false;
    if (run) {
        // START PRE-FILL (NEW-42, spec s2).  Until TARGET blocks are buffered this transmits silence and counts
        // NO underrun -- nothing is missing, the stream has not started yet.  A prime ends at the TOP of the
        // block that finds TARGET, and THAT BLOCK POPS: waiting one more period would add a silent block for
        // nothing, and popping here is what makes the first block out the first block decoded.
        // servo_recentre() states the completion postcondition -- a prime hands the servo a filter with no
        // history, so the 0 -> TARGET climb cannot drag the trim.  For the START prime it is a NO-OP (begin()'s
        // servo_init already centres the filter and a priming block steps no servo), and it earns its keep on
        // the MID-STREAM re-prime below, which begins from a filter wound up by the pre-drop fill: node_test
        // case 13 winds it >4 blocks off centre, and with this call deleted the filter is still 4.3 blocks off
        // after the resume instead of one EMA step from centre.
        // ** It also discards the LEARNED TRIM. **  trim_ppm is recomputed from the filter on every step, so
        // recentring the filter returns the trim to ~0 and the loop re-converges over ~120 s (MEASURED, closed
        // loop at -100 ppm source drift: -100 -> 0 -> overshoot -179 -> -100).  That is the right trade -- a
        // filter reading TARGET+5 against a ring holding TARGET-1 would drive a BOGUS trim for a full 72 s tau --
        // and it is why spec s5's trimlo/trimhi must be read beside reprimes rather than on their own.
        if (m_priming && fill() >= TARGET) { m_priming = false; m_primed = true; servo_recentre(&m_servo); }
        if (m_priming) {
            // primeBlocks() is the START prime's length and ONLY that: a mid-stream re-prime neither extends nor
            // re-times it, which is what `!m_primed` buys (m_primed latches at the first completed prime and is
            // cleared only by begin()).  Reason: spec s5's acceptance reads "ONE prime_ms ~ TARGET * 2.9 ms at
            // START" off a heartbeat sampled at the END of a 10-min window, and s5 also allows re-primes to
            // happen (`under <= reprimes + 2`) -- so a figure that grew or restarted with them could not be
            // checked against TARGET at all.  reprimes() counts the events; how LONG a re-prime took is the
            // source's absence, which the gap histogram measures on the arrival side where it is honest.
            // node_test case 13 pins it: delete `!m_primed` and primeBlocks() reads 68 there instead of 3.
            if (!m_primed) m_primeBlocks++;
        } else if (tail == m_head) {
            // DRY.  One underrun for the EVENT, then -- with the pre-fill on -- silence to TARGET, uncounted, and
            // the servo recentred when it completes.  A gap of G blocks otherwise leaves the ring G short until
            // the 72 s trim refills it; the bench measured that as the burst shape (spec s1).  `under` thereby
            // counts dropouts, not silent blocks, which is what makes `under <= reprimes + 2` a usable bound.
            m_under++;
            if (m_prefill) { m_reprimes++; m_priming = true; }
        } else {
            uint8_t f = fill();                    // pre-pop: the margin this block found, for fillMin/fillMax
            if (f < m_fillMin) m_fillMin = f;
            if (f > m_fillMax) m_fillMax = f;
            memcpy(l->data, m_ring[tail].l, sizeof l->data); memcpy(r->data, m_ring[tail].r, sizeof r->data); m_tail = (uint16_t)((tail + 1) % RING);
            popped = true;
        }
    }
    if (!popped) { memset(l->data, 0, sizeof l->data); memset(r->data, 0, sizeof r->data); }
    // The servo runs once per audio block -- this IS the block clock -- and only while the link is up and not
    // held: with the link down or the source suspended the trim is frozen where it was, so a resumed stream
    // starts from the rate it had learned.  (servo_step's own `hold` argument expresses the same freeze; the
    // node skips the call outright so a held stream costs nothing at all in the ISR.)  And NOT while PRIMING:
    // a priming ring is being filled deliberately, and integrating that ramp would trim the PITCH against a
    // fill the servo did not cause.
    if (run && !m_priming) {
        int32_t t = servo_step(&m_servo, fill(), 0);
        if (t < m_trimLo) m_trimLo = t;
        if (t > m_trimHi) m_trimHi = t;
        if (t != m_applied) { m_applied = t; audioPllTrimPpm(t); }
    }
    transmit(l, 0); transmit(r, 1); release(l); release(r);
}
