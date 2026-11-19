/* loader.h - loading a track, a few milliseconds at a time.
 *
 * step() is called from the main loop between screen updates.  It does small
 * units of work until its time budget is spent, so the screen and the touch
 * panel stay alive and the audio interrupt is never kept waiting:
 *
 *   Probe     open the file, find its format and length (for MP3 a pass over
 *             the frame headers), refuse what will not fit, allocate the PCM,
 *             and look for sidecars next to the file
 *   Decode    into the PCM buffer, front to back.  decodedFrames() grows; the
 *             player may play, and the waveform draw, everything below it.
 *             Peaks are built on the way unless a valid .apv was found.
 *   Spectrum  \ only when there is no valid .att: the spectrum of the whole
 *   Tempo     / track, then tempo and the beat grid from it
 *   Save      whatever was generated: .apv, .asv, .att.  A card that cannot be
 *             written costs the analysis files, never the track.
 *   Done
 *
 * Sidecars are "<audio file name>.apv / .asv / .att" beside the audio file.
 * An .apv belongs to the file if it records the file's size and the CRC-32 of
 * its first 4 KiB and holds the right number of frames; an .att is trusted
 * only alongside such an .apv and if their durations agree.  Anything else is
 * rebuilt and overwritten.
 *
 * pcm(), peaks() and beats() stay valid until the next start() or cancel();
 * generation() changes whenever they stop being so.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "track_source.h"
#include "peaks.h"

/* Files, as the loader sees them.  The board implements this over the SD
 * card, the tests over a directory. */
class Storage {
public:
    virtual ~Storage() = default;
    virtual audio_codecs::preview::SeekableReader *openRead(const char *path) = 0;     /* nullptr if it cannot be opened */
    virtual audio_codecs::preview::SeekableWriter *openWrite(const char *path) = 0;    /* created or truncated; nullptr on failure */
    virtual void close(audio_codecs::preview::SeekableReader *r) = 0;
    virtual void close(audio_codecs::preview::SeekableWriter *w) = 0;
};

struct LoaderMem { void *(*alloc)(size_t); void (*release)(void *); };
struct LoaderBeat { uint32_t frame; uint8_t downbeat; };        /* the layout of synthui_wave_beat_t */

enum class LoadStage : uint8_t { Idle, Probe, Decode, Spectrum, Tempo, Save, Done, Failed };
enum class SidecarResult : uint8_t { Pending, Reused, Saved, SaveFailed };

class Loader {
public:
    Loader(Storage &storage, LoaderMem mem, uint64_t pcm_budget_bytes);
    ~Loader();

    /* Begin loading; whatever was loaded or loading before is dropped first.
     * False (and stage() Failed) if it cannot even be opened. */
    bool start(const char *path);
    void cancel();                                              /* back to Idle, everything freed */
    /* Work for about budget_us microseconds (it finishes the unit in hand). */
    void step(uint32_t budget_us, uint32_t (*now_us)());

    LoadStage stage() const { return stage_; }
    uint8_t progressPct() const;
    const char *error() const { return error_; }                /* "unsupported", "too long (max N min)", "read error", "decode error", "no memory" */

    const TrackInfo &info() const { return info_; }
    const int16_t *pcm() const { return pcm_; }
    uint64_t decodedFrames() const { return decoded_; }
    uint64_t totalFrames() const { return total_; }             /* shrinks to the decoded count when an MP3 ends */
    const PeakBuilder &peaks() const { return peaks_; }
    bool peaksFromFile() const { return peaks_from_file_; }
    bool tempoReady() const { return tempo_ready_; }
    uint32_t bpmQ16() const { return bpm_q16_; }
    const LoaderBeat *beats() const { return beats_; }
    uint32_t beatCount() const { return nbeats_; }
    SidecarResult sidecars() const { return sidecars_; }
    uint32_t generation() const { return generation_; }

private:
    struct Analysis;
    void fail(const char *why);
    void free_all();
    bool unit_probe();
    bool unit_decode();
    bool unit_spectrum();
    bool unit_tempo();
    bool unit_save();
    void begin_analysis_or_finish();
    bool read_whole(const char *path, uint8_t **out, size_t *size, size_t max_bytes);
    bool take_att(const uint8_t *att, size_t size);
    void sidecar_path(char *out, size_t cap, const char *ext) const;

    Storage &storage_;
    LoaderMem mem_;
    uint64_t budget_;
    LoadStage stage_ = LoadStage::Idle;
    char error_[40] = "";
    char path_[256] = "";
    uint32_t generation_ = 0;

    TrackSource *src_ = nullptr;
    audio_codecs::preview::SeekableReader *audio_ = nullptr;
    SrcStatus probe_status_ = SrcStatus::Error;
    TrackInfo info_;
    int16_t *pcm_ = nullptr;
    uint64_t decoded_ = 0, total_ = 0;
    uint32_t audio_size_ = 0, audio_crc_ = 0;

    PeakBuilder peaks_;
    uint8_t *peak_store_ = nullptr;
    bool peaks_from_file_ = false;

    uint8_t *asv_ = nullptr, *att_ = nullptr;
    size_t asv_cap_ = 0, att_cap_ = 0, asv_len_ = 0, att_len_ = 0;
    Analysis *ana_ = nullptr;                                   /* lives in the decoder arena once decoding is over */
    bool tempo_ready_ = false, tempo_from_file_ = false;
    uint32_t bpm_q16_ = 0;
    LoaderBeat *beats_ = nullptr;
    uint32_t nbeats_ = 0;

    SidecarResult sidecars_ = SidecarResult::Pending;
    int save_file_ = 0;                                         /* 0 .apv, 1 .asv, 2 .att, 3 finished */
    size_t save_pos_ = 0;
    audio_codecs::preview::SeekableWriter *out_ = nullptr;
};
