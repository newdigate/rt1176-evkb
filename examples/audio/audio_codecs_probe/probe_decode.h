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

/* WAV, AIFF and FLAC share a shape: a stream header, then data taken in
 * pieces; each call says how many bytes it consumed. */
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

static inline ProbeResult probe_decode_mp3(audio_codecs::mp3::Mp3Decoder &dec, const uint8_t *data, size_t size,
                                           int16_t *out, size_t max_samples)
{
    ProbeResult r = {0, 0, 16, 0, 0};
    audio_codecs::AudioConfig cfg;
    dec.init(cfg);
    size_t pos = 0, written = 0;
    if (size >= 10 && memcmp(data, "ID3", 3) == 0)          /* ID3v2: a 28-bit syncsafe size after the 10-byte header */
        pos = 10 + (((size_t)data[6] & 0x7F) << 21 | ((size_t)data[7] & 0x7F) << 14 | ((size_t)data[8] & 0x7F) << 7 | ((size_t)data[9] & 0x7F));
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
