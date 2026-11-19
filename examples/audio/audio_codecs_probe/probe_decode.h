/* probe_decode.h - decode a whole clip held in memory to interleaved 16-bit
 * PCM with audio-codecs.  Shared by the firmware probe and clips/host_decode,
 * so the board and the host run the same feeding loop.
 * Copyright (c) 2026 Nicholas Newdigate.  SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "audio_codecs/wav/wav_decoder.h"
#include "audio_codecs/aiff/aiff_decoder.h"
#include "audio_codecs/flac/flac_decoder.h"
#include "audio_codecs/mp3/mp3_decoder.h"

struct ProbeResult { uint32_t rate; uint8_t channels; uint8_t bits; uint32_t frames; int error; };

static inline uint32_t probe_fnv(const int16_t *p, size_t samples)
{
    uint32_t h = 2166136261u;
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < samples * 2; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

/* How many frames the container says it holds (0 = it does not say). */
static inline uint64_t probe_total_frames(const audio_codecs::wav::WavDecoder &d)   { return d.get_total_samples(); }
static inline uint64_t probe_total_frames(const audio_codecs::aiff::AiffDecoder &d) { return d.get_total_frames(); }
static inline uint64_t probe_total_frames(const audio_codecs::flac::FlacDecoder &d) { return d.get_total_samples(); }

/* WAV, AIFF and FLAC share a shape: a stream header, then data taken in
 * pieces; each call says how many bytes it consumed.
 *
 * The decoders read whatever they are handed as audio, so the loop must stop
 * at the length the header gives: tag chunks often FOLLOW the audio (an 'ID3 '
 * chunk with artwork in an AIFF, a 'LIST' chunk in a WAV) and would otherwise
 * be played as noise.  A header that gives no length (a streamed WAV with a
 * data size of 0) is read to the end of the data. */
template <typename Dec>
static ProbeResult probe_decode_container(Dec &dec, const uint8_t *data, size_t size,
                                          int16_t *out, size_t max_samples, size_t piece)
{
    ProbeResult r = {0, 0, 0, 0, 0};
    audio_codecs::AudioConfig cfg;
    dec.init(cfg);
    size_t used = 0;
    if (!dec.parse_stream_header(data, size, used)) { r.error = -100; return r; }
    r.rate = dec.get_sample_rate(); r.channels = dec.get_channels(); r.bits = dec.get_bit_depth();
    if (r.channels == 0 || r.channels > 2) { r.error = -101; return r; }
    const uint64_t total = probe_total_frames(dec);
    if (total != 0 && total * r.channels < max_samples) max_samples = (size_t)(total * r.channels);
    size_t pos = used, written = 0;
    while (pos < size && written < max_samples) {
        const size_t give = (size - pos < piece) ? size - pos : piece;
        const int n = dec.decode_frame_i16(data + pos, give, out + written, max_samples - written);
        const size_t adv = dec.get_last_frame_bytes();
        if (n <= 0 || adv == 0) { if (n < 0) r.error = n; break; }
        written += (size_t)n; pos += adv;
    }
    r.frames = (uint32_t)(written / r.channels);
    return r;
}

/* An MPEG audio frame header at p?  Returns the frame length in bytes for
 * MPEG-1 Layer III (the only kind the decoder gets right), -1 for a well-formed
 * MPEG-2 / 2.5 Layer III header (22.05, 24, 16, 11.025, 12, 8 kHz: recognised
 * so that such a file can be refused), 0 for anything else. */
static inline int probe_mp3_header(const uint8_t *p, uint32_t *rate)
{
    static const uint16_t kbps[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const uint32_t hz[4] = {44100, 48000, 32000, 0};
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;
    const int version = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3;      /* version 3 = MPEG-1; layer 1 = Layer III */
    const int br = p[2] >> 4, sr = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
    if (layer != 1 || version == 1 || br == 0 || br == 15 || sr == 3) return 0;
    if (version != 3) return -1;
    *rate = hz[sr];
    return (int)(144000u * kbps[br] / hz[sr]) + pad;
}

/* Where the audio starts: the first MPEG-1 Layer III header that is FOLLOWED,
 * one frame later, by another at the same sample rate.  One header alone is
 * four bytes that random data produces by chance every few kilobytes; the
 * decoder would take its sample rate and channel count from it.  Returns the
 * offset, -1 if there is none within `limit` bytes, -2 if what is there is
 * MPEG-2 / 2.5. */
static inline long probe_mp3_first_frame(const uint8_t *data, size_t size, size_t from, size_t limit)
{
    bool saw_mpeg2 = false;
    const size_t end = (size < from + limit) ? size : from + limit;
    for (size_t i = from; i + 4 <= end; i++) {
        if (data[i] != 0xFF) continue;
        uint32_t r1 = 0, r2 = 0;
        const int len = probe_mp3_header(data + i, &r1);
        if (len == -1) {
            /* 0xFF 0xF3 / 0xE3 with a second one 1 frame on would be an MPEG-2 file; a lone one is noise */
            if (i + 4 < size && (data[i + 1] & 0xE6) == 0xE2) saw_mpeg2 = saw_mpeg2 || (i == from);
            continue;
        }
        if (len <= 4) continue;
        if (i + (size_t)len + 4 > size) return (long)i;                 /* the last frame of the file */
        if (probe_mp3_header(data + i + len, &r2) > 4 && r2 == r1) return (long)i;
    }
    return saw_mpeg2 ? -2 : -1;
}

static inline ProbeResult probe_decode_mp3(audio_codecs::mp3::Mp3Decoder &dec, const uint8_t *data, size_t size,
                                           int16_t *out, size_t max_samples)
{
    ProbeResult r = {0, 0, 16, 0, 0};
    audio_codecs::AudioConfig cfg;
    dec.init(cfg);
    size_t pos = 0, written = 0;
    if (size >= 10 && memcmp(data, "ID3", 3) == 0)          /* ID3v2: a 28-bit syncsafe size after the 10-byte header */
        pos = 10 + (((size_t)data[6] & 0x7F) << 21 | ((size_t)data[7] & 0x7F) << 14 | ((size_t)data[8] & 0x7F) << 7 | ((size_t)data[9] & 0x7F));
    if (pos >= size) { r.error = -103; return r; }
    const long first = probe_mp3_first_frame(data, size, pos, 262144);
    if (first < 0) { r.error = first == -2 ? -104 : -103; return r; }   /* -104: MPEG-2 / 2.5, not supported */
    pos = (size_t)first;
    int guard = 0;
    while (pos + 4 < size && written < max_samples) {
        const int n = dec.decode_frame_i16(data + pos, size - pos, out + written, max_samples - written);
        const size_t adv = dec.get_last_sync_offset() + dec.get_last_frame_bytes();
        if (n == -1 || n == -2 || adv == 0) break;          /* no further frame */
        pos += adv;
        if (n > 0) {
            written += (size_t)n;
            if (r.channels == 0) { uint32_t kbps; dec.get_frame_info(r.rate, r.channels, kbps); }
        } else if (++guard > 100000) { r.error = -102; break; }   /* e.g. -3: bit reservoir not primed yet */
    }
    if (r.channels) r.frames = (uint32_t)(written / r.channels);
    else r.error = -103;
    return r;
}
