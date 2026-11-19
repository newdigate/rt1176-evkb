/* track_source.cpp - see track_source.h.
 * Copyright (c) 2026 Nicholas Newdigate.  SPDX-License-Identifier: MIT */
#include "track_source.h"
#include "audio_codecs/wav/wav_decoder.h"
#include "audio_codecs/aiff/aiff_decoder.h"
#include "audio_codecs/flac/flac_decoder.h"
#include "audio_codecs/mp3/mp3_decoder.h"
#include <new>
#include <memory>
#include <string.h>

using audio_codecs::preview::SeekableReader;
namespace ac = audio_codecs;

/* ---- fast memory shared by whichever adapter is open -------------------- */
static const uint32_t WIN = 32768;                  /* holds a whole FLAC frame: 16-bit to block size 8192, 24-bit to 4096 */
static uint8_t s_win[WIN];
alignas(16) static uint8_t s_arena[sizeof(ac::mp3::Mp3Decoder) > sizeof(ac::flac::FlacDecoder)
                                       ? sizeof(ac::mp3::Mp3Decoder) : sizeof(ac::flac::FlacDecoder)];

/* A sliding window over [start, limit) of the file. */
struct Stream {
    SeekableReader *f = nullptr;
    uint64_t base = 0, limit = 0;                   /* file offset of s_win[0]; nothing at or past limit is read */
    uint32_t len = 0, rd = 0;

    void start(SeekableReader *file, uint64_t off, uint64_t lim) { f = file; base = off; limit = lim; len = rd = 0; }
    uint64_t pos() const { return base + rd; }
    uint32_t avail() const { return len - rd; }
    const uint8_t *ptr() const { return s_win + rd; }
    bool at_end() const { return base + len >= limit; }
    bool full() const { return rd == 0 && len == WIN; }
    /* Slide what is unread to the front and read as much more as fits. */
    uint32_t fill()
    {
        if (rd) { memmove(s_win, s_win + rd, len - rd); base += rd; len -= rd; rd = 0; }
        while (len < WIN && base + len < limit) {
            uint64_t want = limit - (base + len);
            if (want > WIN - len) want = WIN - len;
            if (f->position() != base + len && !f->seek(base + len)) { limit = base + len; break; }
            const size_t got = f->read(s_win + len, (size_t)want);
            if (got == 0) { limit = base + len; break; }        /* shorter than it said: that is the end */
            len += (uint32_t)got;
        }
        return len;
    }
    void skip(uint64_t n)
    {
        if (n <= avail()) rd += (uint32_t)n;
        else { base = pos() + n; len = rd = 0; }
    }
};
static Stream s_stream;

static bool read_at(SeekableReader &f, uint64_t off, uint8_t *buf, size_t n)
{
    if (off + n > f.size()) return false;
    if (f.position() != off && !f.seek(off)) return false;
    return f.read(buf, n) == n;
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put_be32(uint8_t *p, uint32_t v) { p[3] = (uint8_t)v; p[2] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v >> 16); p[0] = (uint8_t)(v >> 24); }
/* Size of an ID3v2 tag at `off`, header included; 0 if there is none. */
static uint64_t id3v2_size(SeekableReader &f, uint64_t off)
{
    uint8_t h[10];
    if (!read_at(f, off, h, 10) || memcmp(h, "ID3", 3) != 0) return 0;
    return 10u + (((uint32_t)h[6] & 0x7F) << 21 | ((uint32_t)h[7] & 0x7F) << 14 | ((uint32_t)h[8] & 0x7F) << 7 | ((uint32_t)h[9] & 0x7F))
               + ((h[5] & 0x10) ? 10u : 0u);                    /* a footer, when flagged */
}

/* ---- WAV and AIFF: PCM in a chunked container --------------------------- *
 * The chunks are walked with seeks and the decoder is shown a small header
 * made of just the format chunk and an (empty) data chunk, so nothing depends
 * on how much else the file carries or where. */
template <typename Dec>
class PcmSource : public TrackSource {
public:
    explicit PcmSource(bool aiff) : aiff_(aiff) {}
    SrcStatus open(SeekableReader &f) override
    {
        close();
        info_ = TrackInfo();
        produced_ = 0;
        uint8_t mini[160]; size_t mlen = 0;
        uint64_t data_off = 0, data_bytes = 0;
        uint8_t bytes_per_sample_hint = 0;
        if (!(aiff_ ? walk_aiff(f, mini, mlen, data_off, data_bytes, bytes_per_sample_hint) : walk_wav(f, mini, mlen, data_off, data_bytes))) return SrcStatus::Error;
        dec_ = new (s_arena) Dec();
        ac::AudioConfig cfg;
        dec_->init(cfg);
        size_t used = 0;
        if (!dec_->parse_stream_header(mini, mlen, used)) return fail();
        info_.sample_rate = dec_->get_sample_rate();
        info_.channels = dec_->get_channels();
        info_.bit_depth = dec_->get_bit_depth();
        if (info_.sample_rate == 0 || info_.channels < 1 || info_.channels > 2 || info_.bit_depth == 0) return fail();
        const uint32_t bps = bytes_per_sample_hint ? bytes_per_sample_hint : (uint32_t)(info_.bit_depth + 7u) / 8u;
        block_ = bps * info_.channels;
        if (block_ == 0) return fail();
        info_.total_frames = data_bytes / block_;
        s_stream.start(&f, data_off, data_off + info_.total_frames * block_);
        return SrcStatus::Ready;
    }
    SrcStatus probe() override { return dec_ ? SrcStatus::Ready : SrcStatus::Error; }
    const TrackInfo &info() const override { return info_; }
    uint32_t minChunkFrames() const override { return 1; }
    int32_t decode(int16_t *dst, uint32_t max_frames) override
    {
        if (!dec_ || !dst || max_frames == 0) return -1;
        if (produced_ >= info_.total_frames) return 0;
        if (s_stream.avail() < block_) s_stream.fill();
        uint32_t give = s_stream.avail() - s_stream.avail() % block_;
        if (give == 0) return 0;                                 /* the file ended short */
        uint64_t want = info_.total_frames - produced_;
        if (want > max_frames) want = max_frames;
        if ((uint64_t)give > want * block_) give = (uint32_t)(want * block_);
        const int n = dec_->decode_frame_i16(s_stream.ptr(), give, dst, (size_t)want * info_.channels);
        const size_t adv = dec_->get_last_frame_bytes();
        if (n < 0) return -1;
        if (n == 0 || adv == 0) return 0;
        s_stream.skip(adv);
        produced_ += (uint32_t)n / info_.channels;
        return n / info_.channels;
    }
    void close() override { if (dec_) { std::destroy_at(dec_); dec_ = nullptr; } }

private:
    SrcStatus fail() { close(); return SrcStatus::Error; }

    static bool walk_wav(SeekableReader &f, uint8_t *mini, size_t &mlen, uint64_t &data_off, uint64_t &data_bytes)
    {
        uint8_t h[12], fmt[40]; uint32_t fmt_len = 0; bool have_data = false;
        if (!read_at(f, 0, h, 12) || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) return false;
        uint64_t off = 12;
        for (int i = 0; i < 4096 && off + 8 <= f.size(); i++) {
            if (!read_at(f, off, h, 8)) return false;
            const uint32_t size = le32(h + 4);
            if (!memcmp(h, "fmt ", 4)) {
                fmt_len = size < sizeof(fmt) ? size : (uint32_t)sizeof(fmt);
                if (fmt_len < 16 || !read_at(f, off + 8, fmt, fmt_len)) return false;
            } else if (!memcmp(h, "data", 4)) {
                data_off = off + 8; data_bytes = size; have_data = true;
                break;
            }
            off += 8u + (uint64_t)size + (size & 1u);
        }
        if (!have_data || fmt_len == 0) return false;
        /* a streamed WAV says 0 (or a size it never reached): the audio runs to the end of the file */
        if (data_bytes == 0 || data_off + data_bytes > f.size()) data_bytes = f.size() - data_off;
        memcpy(mini, "RIFF", 4); put_le32(mini + 4, 4u + 8u + fmt_len + 8u + (uint32_t)data_bytes); memcpy(mini + 8, "WAVEfmt ", 8);
        put_le32(mini + 16, fmt_len); memcpy(mini + 20, fmt, fmt_len);
        memcpy(mini + 20 + fmt_len, "data", 4); put_le32(mini + 24 + fmt_len, (uint32_t)data_bytes);
        mlen = 28 + fmt_len;
        return true;
    }
    static bool walk_aiff(SeekableReader &f, uint8_t *mini, size_t &mlen, uint64_t &data_off, uint64_t &data_bytes, uint8_t &bps_hint)
    {
        uint8_t h[12], comm[64], s8[8]; uint32_t comm_len = 0; bool have_ssnd = false;
        if (!read_at(f, 0, h, 12) || memcmp(h, "FORM", 4) != 0 || (memcmp(h + 8, "AIFF", 4) != 0 && memcmp(h + 8, "AIFC", 4) != 0)) return false;
        uint8_t type[4]; memcpy(type, h + 8, 4);
        uint64_t off = 12;
        /* COMM may come after SSND: walk until both are found */
        for (int i = 0; i < 4096 && off + 8 <= f.size() && !(comm_len && have_ssnd); i++) {
            if (!read_at(f, off, h, 8)) return false;
            const uint32_t size = be32(h + 4);
            if (!memcmp(h, "COMM", 4)) {
                comm_len = size < sizeof(comm) ? size : (uint32_t)sizeof(comm);
                if (comm_len < 18 || !read_at(f, off + 8, comm, comm_len)) return false;
            } else if (!memcmp(h, "SSND", 4)) {
                if (size < 8 || !read_at(f, off + 8, s8, 8)) return false;
                const uint32_t skip = be32(s8);
                if ((uint64_t)skip + 8u > size) return false;
                data_off = off + 16u + skip; data_bytes = (uint64_t)size - 8u - skip; have_ssnd = true;
            }
            off += 8u + (uint64_t)size + (size & 1u);
        }
        if (!comm_len || !have_ssnd) return false;
        if (data_off + data_bytes > f.size()) data_bytes = f.size() > data_off ? f.size() - data_off : 0;
        /* G.711 in an AIFF-C is one byte per sample whatever COMM's sampleSize says */
        if (!memcmp(type, "AIFC", 4) && comm_len >= 22 && (!memcmp(comm + 18, "ulaw", 4) || !memcmp(comm + 18, "alaw", 4) ||
                                                           !memcmp(comm + 18, "ULAW", 4) || !memcmp(comm + 18, "ALAW", 4))) bps_hint = 1;
        const uint32_t frames = be32(comm + 2);
        const uint32_t bps = bps_hint ? bps_hint : ((uint32_t)(comm[6] << 8 | comm[7]) + 7u) / 8u;
        const uint32_t ch = (uint32_t)(comm[0] << 8 | comm[1]);
        if (bps && ch && (uint64_t)frames * bps * ch < data_bytes) data_bytes = (uint64_t)frames * bps * ch;   /* COMM is the authority on length */
        const uint32_t cpad = comm_len & 1u;
        memcpy(mini, "FORM", 4); put_be32(mini + 4, 4u + 8u + comm_len + cpad + 16u + (uint32_t)data_bytes); memcpy(mini + 8, type, 4);
        memcpy(mini + 12, "COMM", 4); put_be32(mini + 16, comm_len); memcpy(mini + 20, comm, comm_len);
        size_t p = 20 + comm_len; if (cpad) mini[p++] = 0;
        memcpy(mini + p, "SSND", 4); put_be32(mini + p + 4, 8u + (uint32_t)data_bytes); memset(mini + p + 8, 0, 8);
        mlen = p + 16;
        return true;
    }

    const bool aiff_;
    Dec *dec_ = nullptr;
    TrackInfo info_;
    uint32_t block_ = 0;
    uint64_t produced_ = 0;
};

/* ---- FLAC --------------------------------------------------------------- */
class FlacSource : public TrackSource {
public:
    SrcStatus open(SeekableReader &f) override
    {
        close();
        info_ = TrackInfo();
        produced_ = 0;
        uint64_t off = id3v2_size(f, 0);
        uint8_t h[4], mini[42];
        if (!read_at(f, off, h, 4) || memcmp(h, "fLaC", 4) != 0) return SrcStatus::Error;
        off += 4;
        bool last = false, have_info = false;
        for (int i = 0; i < 4096 && !last; i++) {              /* metadata blocks: STREAMINFO first, then whatever (artwork, padding) */
            if (!read_at(f, off, h, 4)) return SrcStatus::Error;
            last = (h[0] & 0x80) != 0;
            const uint32_t len = (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
            if (i == 0) {
                if ((h[0] & 0x7F) != 0 || len != 34 || !read_at(f, off + 4, mini + 8, 34)) return SrcStatus::Error;
                have_info = true;
            }
            off += 4u + len;
        }
        if (!last || !have_info || off > f.size()) return SrcStatus::Error;
        memcpy(mini, "fLaC", 4); mini[4] = 0x80; mini[5] = 0; mini[6] = 0; mini[7] = 34;       /* STREAMINFO, the only block */
        dec_ = new (s_arena) ac::flac::FlacDecoder();
        ac::AudioConfig cfg;
        dec_->init(cfg);
        size_t used = 0;
        if (!dec_->parse_stream_header(mini, sizeof(mini), used)) { close(); return SrcStatus::Error; }
        info_.sample_rate = dec_->get_sample_rate();
        info_.channels = dec_->get_channels();
        info_.bit_depth = dec_->get_bit_depth();
        info_.total_frames = dec_->get_total_samples();
        if (info_.sample_rate == 0 || info_.channels < 1 || info_.channels > 2 || info_.total_frames == 0) { close(); return SrcStatus::Error; }
        s_stream.start(&f, off, f.size());
        return SrcStatus::Ready;
    }
    SrcStatus probe() override { return dec_ ? SrcStatus::Ready : SrcStatus::Error; }
    const TrackInfo &info() const override { return info_; }
    uint32_t minChunkFrames() const override { return 8192; }
    int32_t decode(int16_t *dst, uint32_t max_frames) override
    {
        if (!dec_ || !dst || max_frames < minChunkFrames()) return -1;
        if (produced_ >= info_.total_frames) return 0;
        for (int attempt = 0; attempt < 2; attempt++) {
            if (attempt == 1 || s_stream.avail() < 4096) s_stream.fill();
            if (s_stream.avail() == 0) return 0;
            const int n = dec_->decode_frame_i16(s_stream.ptr(), s_stream.avail(), dst, (size_t)max_frames * info_.channels);
            if (n > 0) {
                s_stream.skip(dec_->get_last_frame_bytes());
                uint64_t frames = (uint32_t)n / info_.channels;
                if (produced_ + frames > info_.total_frames) frames = info_.total_frames - produced_;
                produced_ += frames;
                return (int32_t)frames;
            }
            /* The frame may simply not be all in the window yet: slide and refill once. */
            if (attempt == 0 && !s_stream.full()) continue;
            break;
        }
        return s_stream.at_end() ? 0 : -1;                      /* cut short at the end of the file, or damaged in the middle */
    }
    void close() override { if (dec_) { std::destroy_at(dec_); dec_ = nullptr; } }

private:
    ac::flac::FlacDecoder *dec_ = nullptr;
    TrackInfo info_;
    uint64_t produced_ = 0;
};

/* ---- MP3 ---------------------------------------------------------------- */
/* An MPEG audio frame header at p?  The frame length in bytes for MPEG-1
 * Layer III; -1 for a well-formed MPEG-2 / 2.5 Layer III header; 0 otherwise. */
static int mp3_header(const uint8_t *p, uint32_t *rate)
{
    static const uint16_t kbps[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const uint32_t hz[4] = {44100, 48000, 32000, 0};
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;
    const int version = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3;
    const int br = p[2] >> 4, sr = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
    if (layer != 1 || version == 1 || br == 0 || br == 15 || sr == 3) return 0;
    if (version != 3) return -1;
    *rate = hz[sr];
    return (int)(144000u * kbps[br] / hz[sr]) + pad;
}

class Mp3Source : public TrackSource {
public:
    SrcStatus open(SeekableReader &f) override
    {
        close();
        info_ = TrackInfo();
        produced_ = 0; frames_ = 0; junk_ = 0; found_ = false; scanning_ = true;
        file_ = &f;
        origin_ = id3v2_size(f, 0);
        if (origin_ >= f.size()) { scanning_ = false; return SrcStatus::Error; }
        s_stream.start(&f, origin_, f.size());
        return SrcStatus::Working;
    }
    /* Walk the frame headers: find the first real frame, then count frames. */
    SrcStatus probe() override
    {
        if (!scanning_) return dec_ ? SrcStatus::Ready : SrcStatus::Error;
        const uint64_t slice_start = s_stream.pos();
        while (s_stream.pos() - slice_start < 65536u) {
            if (s_stream.avail() < 4) { s_stream.fill(); if (s_stream.avail() < 4) return finish(); }
            uint32_t rate = 0, rate2 = 0;
            const int len = mp3_header(s_stream.ptr(), &rate);
            if (!found_) {
                if (len > 4) {
                    if (s_stream.avail() < (uint32_t)len + 4u) s_stream.fill();
                    /* one header is four bytes that noise makes by chance: it must be followed by another */
                    const bool next = s_stream.avail() >= (uint32_t)len + 4u
                                        ? (mp3_header(s_stream.ptr() + len, &rate2) > 4 && rate2 == rate)
                                        : (s_stream.at_end() && s_stream.avail() >= (uint32_t)len);      /* the only frame */
                    if (next) {
                        found_ = true; first_ = s_stream.pos(); frames_ = 1;
                        info_.sample_rate = rate;
                        info_.channels = ((s_stream.ptr()[3] >> 6) == 3) ? 1 : 2;
                        info_.bit_depth = 16;
                        s_stream.skip((uint64_t)len);
                        continue;
                    }
                } else if (len == -1 && s_stream.pos() == origin_) {
                    scanning_ = false;                           /* an MPEG-2 / 2.5 file: not supported */
                    return SrcStatus::Error;
                }
                s_stream.skip(1);
                if (s_stream.pos() - origin_ > 262144u) { scanning_ = false; return SrcStatus::Error; }
            } else if (len > 4) {
                frames_++; junk_ = 0;
                s_stream.skip((uint64_t)len);
            } else {
                if (++junk_ > 65536u) return finish();           /* that much that is not audio: stop counting here */
                s_stream.skip(1);
            }
        }
        return SrcStatus::Working;
    }
    const TrackInfo &info() const override { return info_; }
    uint32_t minChunkFrames() const override { return 1152; }
    int32_t decode(int16_t *dst, uint32_t max_frames) override
    {
        if (!dec_ || !dst || max_frames < minChunkFrames()) return -1;
        for (int guard = 0; guard < 64; guard++) {
            if (produced_ >= info_.total_frames) return 0;
            if (s_stream.avail() < 2048 && !s_stream.at_end()) s_stream.fill();
            if (s_stream.avail() < 4) return 0;
            const int n = dec_->decode_frame_i16(s_stream.ptr(), s_stream.avail(), dst, (size_t)max_frames * info_.channels);
            const size_t adv = dec_->get_last_sync_offset() + dec_->get_last_frame_bytes();
            if (n == -1 || n == -2 || adv == 0) {                /* no whole frame in what it was given */
                if (!s_stream.at_end() && !s_stream.full()) { s_stream.fill(); continue; }
                return 0;
            }
            s_stream.skip(adv < s_stream.avail() ? adv : s_stream.avail());
            if (n > 0) {
                uint64_t frames = (uint32_t)n / info_.channels;
                if (produced_ + frames > info_.total_frames) frames = info_.total_frames - produced_;
                produced_ += frames;
                return (int32_t)frames;
            }
        }
        return -1;
    }
    void close() override { if (dec_) { std::destroy_at(dec_); dec_ = nullptr; } scanning_ = false; }

private:
    SrcStatus finish()
    {
        scanning_ = false;
        if (!found_ || frames_ == 0) return SrcStatus::Error;
        info_.total_frames = (uint64_t)frames_ * 1152u;
        dec_ = new (s_arena) ac::mp3::Mp3Decoder();
        ac::AudioConfig cfg;
        dec_->init(cfg);
        s_stream.start(file_, first_, file_->size());
        return SrcStatus::Ready;
    }

    ac::mp3::Mp3Decoder *dec_ = nullptr;
    SeekableReader *file_ = nullptr;
    TrackInfo info_;
    uint64_t origin_ = 0, first_ = 0, produced_ = 0;
    uint32_t frames_ = 0, junk_ = 0;
    bool found_ = false, scanning_ = false;
};

/* ---- by name ------------------------------------------------------------ */
static bool ext_is(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    dot++;
    for (;; dot++, ext++) {
        const char a = (*dot >= 'A' && *dot <= 'Z') ? (char)(*dot + 32) : *dot;
        if (a != *ext) return false;
        if (!a) return true;
    }
}

TrackSource *track_source_for_name(const char *name)
{
    static PcmSource<ac::wav::WavDecoder> wav(false);
    static PcmSource<ac::aiff::AiffDecoder> aiff(true);
    static FlacSource flac;
    static Mp3Source mp3;
    if (!name) return nullptr;
    if (ext_is(name, "wav")) return &wav;
    if (ext_is(name, "aif") || ext_is(name, "aiff")) return &aiff;
    if (ext_is(name, "flac")) return &flac;
    if (ext_is(name, "mp3")) return &mp3;
    return nullptr;
}
