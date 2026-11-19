/* loader.cpp - see loader.h.
 * Copyright (c) 2026 Nicholas Newdigate.  SPDX-License-Identifier: MIT */
#include "loader.h"
#include "deck_math.h"
#include "audio_codecs/preview/preview_stream.h"
#include "audio_codecs/spectrum/spectrum_generator.h"
#include "audio_codecs/spectrum/desktop_fft.h"
#include "audio_codecs/tempo/tempo_generator.h"
#include "audio_codecs/tempo/tempo_reader.h"
#include <new>
#include <memory>
#include <stdio.h>
#include <string.h>

using namespace audio_codecs;
using preview::MemoryReader;
using preview::MemoryWriter;

static const uint32_t DECODE_CHUNK = 8192;      /* frames per decode unit: one FLAC frame at most, 7 MP3 frames, 0.19 s of PCM */
static const size_t SAVE_CHUNK = 32768;

/* The spectrum and tempo engines and their streams.  Built with placement new
 * in the decoder arena (fast memory) after the source has been closed. */
struct Loader::Analysis {
    MemoryReader pcm_in;
    MemoryWriter asv_out;
    spectrum::DesktopRealFftBackend fft;
    spectrum::SpectrumGenerator spectrum;
    MemoryReader asv_in;
    MemoryWriter att_out;
    tempo::TempoGenerator tempo;
    bool tempo_started = false;
    Analysis(const uint8_t *pcm, size_t pcm_bytes, uint8_t *asv, size_t asv_cap, uint8_t *att, size_t att_cap)
        : pcm_in(pcm, pcm_bytes), asv_out(asv, asv_cap), asv_in(asv, 0), att_out(att, att_cap) {}
};

static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

Loader::Loader(Storage &storage, LoaderMem mem, uint64_t pcm_budget_bytes) : storage_(storage), mem_(mem), budget_(pcm_budget_bytes) {}
Loader::~Loader() { free_all(); }

void Loader::free_all()
{
    if (ana_) { std::destroy_at(ana_); ana_ = nullptr; }
    if (src_) { src_->close(); src_ = nullptr; }
    if (audio_) { storage_.close(audio_); audio_ = nullptr; }
    if (out_) { storage_.close(out_); out_ = nullptr; }
    if (pcm_) { mem_.release(pcm_); pcm_ = nullptr; }
    if (peak_store_) { mem_.release(peak_store_); peak_store_ = nullptr; }
    if (asv_) { mem_.release(asv_); asv_ = nullptr; }
    if (att_) { mem_.release(att_); att_ = nullptr; }
    if (beats_) { mem_.release(beats_); beats_ = nullptr; }
    peaks_ = PeakBuilder();
    nbeats_ = 0; decoded_ = total_ = 0; asv_len_ = att_len_ = asv_cap_ = att_cap_ = 0;
    tempo_ready_ = tempo_from_file_ = peaks_from_file_ = false;
    bpm_q16_ = 0;
    info_ = TrackInfo();
    sidecars_ = SidecarResult::Pending;
    save_file_ = 0; save_pos_ = 0;
}

void Loader::fail(const char *why)
{
    /* The source and the files are closed; the memory is kept until cancel() or the next start(),
     * so nothing the screen still points at disappears under it. */
    if (ana_) { std::destroy_at(ana_); ana_ = nullptr; }
    if (src_) { src_->close(); src_ = nullptr; }
    if (audio_) { storage_.close(audio_); audio_ = nullptr; }
    if (out_) { storage_.close(out_); out_ = nullptr; }
    snprintf(error_, sizeof(error_), "%s", why);
    stage_ = LoadStage::Failed;
}

void Loader::cancel()
{
    free_all();
    generation_++;
    error_[0] = 0;
    stage_ = LoadStage::Idle;
}

void Loader::sidecar_path(char *out, size_t cap, const char *ext) const { deck_sidecar_name(out, cap, path_, ext); }

bool Loader::start(const char *path)
{
    cancel();
    if (!path || strlen(path) >= sizeof(path_) - 5) { fail("unsupported"); return false; }      /* room for ".apv" */
    snprintf(path_, sizeof(path_), "%s", path);
    src_ = track_source_for_name(path);
    if (!src_) { fail("unsupported"); return false; }
    audio_ = storage_.openRead(path);
    if (!audio_) { src_ = nullptr; fail("read error"); return false; }
    stage_ = LoadStage::Probe;
    probe_status_ = src_->open(*audio_);
    return true;
}

uint8_t Loader::progressPct() const
{
    switch (stage_) {
    case LoadStage::Decode:   return (uint8_t)(total_ ? decoded_ * 70u / total_ : 0);
    case LoadStage::Spectrum: return (uint8_t)(70u + (ana_ ? (uint32_t)(ana_->spectrum.progress() * 20.0f) : 0u));
    case LoadStage::Tempo:    return (uint8_t)(90u + (ana_ ? (uint32_t)(ana_->tempo.progress() * 8.0f) : 0u));
    case LoadStage::Save:     return 99;
    case LoadStage::Done:     return 100;
    default:                  return 0;
    }
}

void Loader::step(uint32_t budget_us, uint32_t (*now_us)())
{
    const uint32_t t0 = now_us();
    for (;;) {
        bool more = false;
        switch (stage_) {
        case LoadStage::Probe:    more = unit_probe(); break;
        case LoadStage::Decode:   more = unit_decode(); break;
        case LoadStage::Spectrum: more = unit_spectrum(); break;
        case LoadStage::Tempo:    more = unit_tempo(); break;
        case LoadStage::Save:     more = unit_save(); break;
        default: return;                                          /* Idle, Done, Failed */
        }
        if (!more) return;
        if ((uint32_t)(now_us() - t0) >= budget_us) return;
    }
}

/* Read a whole (small) file into memory from mem_.  False if it is missing, empty, or larger than max_bytes. */
bool Loader::read_whole(const char *path, uint8_t **out, size_t *size, size_t max_bytes)
{
    *out = nullptr; *size = 0;
    if (!path[0]) return false;
    preview::SeekableReader *r = storage_.openRead(path);
    if (!r) return false;
    const uint64_t n = r->size();
    bool ok = n > 0 && n <= max_bytes;
    uint8_t *buf = ok ? (uint8_t *)mem_.alloc((size_t)n) : nullptr;
    ok = ok && buf != nullptr;
    size_t got = 0;
    while (ok && got < n) {
        const size_t k = r->read(buf + got, (size_t)n - got);
        if (k == 0) ok = false;
        got += k;
    }
    storage_.close(r);
    if (!ok) { if (buf) mem_.release(buf); return false; }
    *out = buf; *size = (size_t)n;
    return true;
}

/* Beats and tempo out of an ATT1 image. */
bool Loader::take_att(const uint8_t *att, size_t size)
{
    MemoryReader in(att, size);
    tempo::TempoReader tr;
    if (!tr.init(&in)) return false;
    const tempo::AttHeader &h = tr.header();
    const uint32_t n = h.total_beats;
    if (n > 20000u) return false;
    LoaderBeat *b = n ? (LoaderBeat *)mem_.alloc((size_t)n * sizeof(LoaderBeat)) : nullptr;
    if (n && !b) return false;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; i++) {
        tempo::AttBeatMarker m;
        if (!tr.get_beat_at_index(i, &m)) break;
        const uint64_t frame = (uint64_t)m.time_ms * info_.sample_rate / 1000u;
        if (frame >= total_) break;                               /* the spectrum rounds the track up to whole hops: no beat past the audio */
        if (kept && frame <= b[kept - 1].frame) continue;         /* keep them strictly ascending */
        b[kept].frame = (uint32_t)frame;
        b[kept].downbeat = (m.flags & tempo::ATT_BEAT_FLAG_DOWNBEAT) ? 1 : 0;
        kept++;
    }
    if (beats_) mem_.release(beats_);
    beats_ = b; nbeats_ = kept;
    bpm_q16_ = h.global_bpm_q16;
    return true;
}

bool Loader::unit_probe()
{
    if (probe_status_ == SrcStatus::Working) { probe_status_ = src_->probe(); return true; }
    if (probe_status_ != SrcStatus::Ready) { fail(track_source_read_error() ? "read error" : "unsupported"); return false; }
    info_ = src_->info();
    total_ = info_.total_frames;
    const uint64_t bytes = total_ * info_.channels * 2u;
    if (bytes > budget_) {
        char msg[40];
        snprintf(msg, sizeof(msg), "too long (max %u min)", (unsigned)(budget_ / ((uint64_t)info_.sample_rate * info_.channels * 2u * 60u)));
        fail(msg);
        return false;
    }
    /* a decode call may need room for a whole decoder frame at the very end */
    pcm_ = (int16_t *)mem_.alloc((size_t)bytes + (size_t)DECODE_CHUNK * info_.channels * 2u);
    if (!pcm_) { fail("no memory"); return false; }

    /* who is this file?  its size and the CRC-32 of its first 4 KiB */
    {
        uint8_t head[4096];
        const uint64_t fsize = audio_->size();
        const size_t n = fsize < sizeof(head) ? (size_t)fsize : sizeof(head);
        size_t got = 0;
        if (audio_->seek(0)) got = audio_->read(head, n);
        if (got != n) { fail("read error"); return false; }
        audio_size_ = (uint32_t)fsize;
        audio_crc_ = crc32_ieee(head, n);
    }
    /* a valid .apv gives the whole waveform at once */
    char p[sizeof(path_) + 8];
    const size_t store_bytes = PeakBuilder::storageBytes(total_, info_.channels);
    uint8_t *img = nullptr; size_t img_size = 0;
    sidecar_path(p, sizeof(p), "apv");
    if (read_whole(p, &img, &img_size, store_bytes)) {
        PeakBuilder pb;
        const bool mp3 = src_->minChunkFrames() == 1152u;        /* its announced length is an upper bound */
        if (pb.adopt(img, img_size) && pb.sourceFileSize() == audio_size_ && pb.sourceHeaderCrc() == audio_crc_ &&
            pb.channels() == info_.channels && pb.sampleRate() == info_.sample_rate &&
            (mp3 ? (pb.totalFrames() <= total_ && pb.totalFrames() + 2304u >= total_) : pb.totalFrames() == total_)) {
            peaks_ = pb; peak_store_ = img; peaks_from_file_ = true;
        } else {
            mem_.release(img);
        }
    }
    if (!peaks_from_file_) {
        peak_store_ = (uint8_t *)mem_.alloc(store_bytes);
        if (!peak_store_ || !peaks_.begin(peak_store_, store_bytes, info_.channels, info_.sample_rate, total_)) { fail("no memory"); return false; }
    } else {
        /* ...and the .att is trusted only beside it */
        uint8_t *att = nullptr; size_t att_size = 0;
        sidecar_path(p, sizeof(p), "att");
        if (read_whole(p, &att, &att_size, 1u << 20)) {
            tempo::AttHeader h;
            const uint32_t apv_ms = info_.sample_rate ? (uint32_t)(peaks_.totalFrames() * 1000ull / info_.sample_rate) : 0;
            if (att_size >= sizeof(h)) {
                memcpy(&h, att, sizeof(h));
                const uint32_t d = h.duration_ms > apv_ms ? h.duration_ms - apv_ms : apv_ms - h.duration_ms;
                /* the spectrum's duration is rounded to whole hops: a few ms either way */
                if (memcmp(h.magic, tempo::ATT_MAGIC, 4) == 0 && h.version == tempo::ATT_VERSION && d <= 25u && take_att(att, att_size))
                    tempo_ready_ = tempo_from_file_ = true;
            }
            mem_.release(att);
        }
    }
    stage_ = LoadStage::Decode;
    return true;
}

bool Loader::unit_decode()
{
    uint32_t want = DECODE_CHUNK;
    if (want < src_->minChunkFrames()) want = src_->minChunkFrames();
    const int32_t n = decoded_ < total_ ? src_->decode(pcm_ + decoded_ * info_.channels, want) : 0;
    if (n < 0) { fail(track_source_read_error() ? "read error" : "decode error"); return false; }
    if (n > 0) {
        uint64_t take = (uint32_t)n;
        if (decoded_ + take > total_) take = total_ - decoded_;
        if (!peaks_from_file_) peaks_.push(pcm_ + decoded_ * info_.channels, (uint32_t)take);
        decoded_ += take;
        if (decoded_ < total_) return true;
    }
    /* the end of the track */
    if (track_source_read_error()) { fail("read error"); return false; }
    if (decoded_ == 0) { fail("decode error"); return false; }
    total_ = decoded_;
    if (!peaks_from_file_) peaks_.finish(decoded_);
    src_->close(); src_ = nullptr;
    storage_.close(audio_); audio_ = nullptr;
    begin_analysis_or_finish();
    return stage_ != LoadStage::Done && stage_ != LoadStage::Failed;
}

void Loader::begin_analysis_or_finish()
{
    if (tempo_ready_) {                                           /* both sidecars were good */
        sidecars_ = SidecarResult::Reused;
        stage_ = LoadStage::Done;
        return;
    }
    const uint64_t hops = (total_ + 511u) / 512u;
    asv_cap_ = 128u + (size_t)(hops * 64u * 108u / 100u) + 8192u;                         /* three levels: 1 + 1/16 + 1/256 */
    att_cap_ = 128u + (size_t)(hops / 64u + 64u) * 12u + (size_t)(hops / 10u + 256u) * 16u + 2048u;
    asv_ = (uint8_t *)mem_.alloc(asv_cap_);
    att_ = (uint8_t *)mem_.alloc(att_cap_);
    size_t arena_bytes = 0;
    void *arena = track_source_arena(&arena_bytes);
    if (!asv_ || !att_ || arena_bytes < sizeof(Analysis)) { fail("no memory"); return; }
    ana_ = new (arena) Analysis((const uint8_t *)pcm_, (size_t)total_ * info_.channels * 2u, asv_, asv_cap_, att_, att_cap_);
    ana_->fft.init();
    if (!ana_->spectrum.init(ana_->pcm_in, ana_->asv_out, ana_->fft, info_.sample_rate, info_.channels, true)) { fail("decode error"); return; }
    stage_ = LoadStage::Spectrum;
}

bool Loader::unit_spectrum()
{
    const spectrum::GeneratorStatus s = ana_->spectrum.step(4);   /* 4 hops: 46 ms of audio */
    if (s == spectrum::GeneratorStatus::Complete) {
        asv_len_ = (size_t)ana_->asv_out.size();
        ana_->asv_in = MemoryReader(asv_, asv_len_);
        if (!ana_->tempo.init(&ana_->asv_in, &ana_->att_out)) { fail("decode error"); return false; }
        stage_ = LoadStage::Tempo;
        return true;
    }
    if (s == spectrum::GeneratorStatus::ErrorSource || s == spectrum::GeneratorStatus::ErrorDest || s == spectrum::GeneratorStatus::ErrorBackend) {
        fail("decode error");
        return false;
    }
    return true;
}

bool Loader::unit_tempo()
{
    ana_->tempo.step(64);
    if (ana_->tempo.has_error()) { fail("decode error"); return false; }
    if (!ana_->tempo.is_complete()) return true;
    att_len_ = (size_t)ana_->att_out.size();
    std::destroy_at(ana_); ana_ = nullptr;
    if (take_att(att_, att_len_)) tempo_ready_ = true;
    save_file_ = peaks_from_file_ ? 1 : 0;                        /* a reused .apv is not rewritten */
    save_pos_ = 0;
    if (!peaks_from_file_) peaks_.stamp(audio_size_, audio_crc_);
    stage_ = LoadStage::Save;
    return true;
}

bool Loader::unit_save()
{
    static const char *const ext[3] = {"apv", "asv", "att"};
    if (save_file_ >= 3) {
        if (sidecars_ == SidecarResult::Pending) sidecars_ = SidecarResult::Saved;
        stage_ = LoadStage::Done;
        return false;
    }
    const uint8_t *data = save_file_ == 0 ? peaks_.image() : (save_file_ == 1 ? asv_ : att_);
    const size_t size = save_file_ == 0 ? peaks_.imageBytes() : (save_file_ == 1 ? asv_len_ : att_len_);
    if (!out_) {
        char p[sizeof(path_) + 8];
        sidecar_path(p, sizeof(p), ext[save_file_]);
        out_ = p[0] ? storage_.openWrite(p) : nullptr;
        save_pos_ = 0;
        if (!out_ || size == 0) {                                 /* cannot write here: the track is fine, the files are not kept */
            if (out_) { storage_.close(out_); out_ = nullptr; }
            sidecars_ = SidecarResult::SaveFailed;
            save_file_ = 3;
            return true;
        }
    }
    const size_t n = size - save_pos_ < SAVE_CHUNK ? size - save_pos_ : SAVE_CHUNK;
    if (out_->write(data + save_pos_, n) != n) {
        storage_.close(out_); out_ = nullptr;
        sidecars_ = SidecarResult::SaveFailed;
        save_file_ = 3;
        return true;
    }
    save_pos_ += n;
    if (save_pos_ >= size) { storage_.close(out_); out_ = nullptr; save_file_++; }
    return true;
}
