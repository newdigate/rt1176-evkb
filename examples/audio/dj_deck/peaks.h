/* peaks.h - PeakBuilder: the waveform's peak tables, built as a track decodes.
 *
 * The tables are audio-codecs' APV1 preview format: for every 128 frames a
 * {min,max} pair per channel as signed bytes (level 0), then the same over 16
 * of those (level 1: 2048 frames) and over 16 of those (level 2: 32768).  The
 * library's PreviewGenerator builds the coarse levels only after the whole
 * source has been read, so the overview could not follow a load in progress;
 * this builds all three at once, and what it saves is byte for byte the file
 * PreviewGenerator would have written (tests/peaks_test.cpp).
 *
 * The caller provides the storage: storageBytes() for the frame count the
 * track announces.  While building, each level sits at a fixed place in that
 * storage, so table() pointers stay valid as points() grows.  finish() closes
 * the partial points and, if the track came out shorter than announced, packs
 * the levels together: ask for table() again after finish().
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>

class PeakBuilder {
public:
    static const int LEVELS = 3;

    static size_t storageBytes(uint64_t total_frames, uint8_t channels);

    /* channels 1 or 2.  False if the storage is missing or too small. */
    bool begin(uint8_t *storage, size_t cap, uint8_t channels, uint32_t sample_rate, uint64_t total_frames);
    /* Interleaved 16-bit frames, any amount per call.  Frames beyond what the storage holds are dropped. */
    void push(const int16_t *pcm, uint32_t frames);
    /* The track is over, at actual_frames (<= what was pushed).  Writes the header. */
    void finish(uint64_t actual_frames);
    /* Use a complete APV1 file image instead of building.  False if it is not a sound one. */
    bool adopt(uint8_t *image, size_t size);

    const int8_t *table(int level) const;          /* points are 2 * channels() bytes each */
    uint32_t points(int level) const;              /* complete points so far */
    uint32_t framesPerPoint(int level) const;      /* 128, 2048, 32768 */
    uint8_t  channels() const { return ch_; }
    uint32_t sampleRate() const { return rate_; }
    uint64_t totalFrames() const { return total_; }
    bool     ready() const { return base_ != nullptr; }

    /* The whole file, once finished or adopted. */
    const uint8_t *image() const { return base_; }
    size_t imageBytes() const;
    /* Which audio file this belongs to: its size and the CRC-32 of its first 4 KiB. */
    void stamp(uint32_t source_file_size, uint32_t source_header_crc32);
    uint32_t sourceFileSize() const;
    uint32_t sourceHeaderCrc() const;

private:
    void close_point(int level);
    void fold(int level, const int8_t *pt);

    uint8_t *base_ = nullptr;
    uint8_t  ch_ = 2, bpp_ = 4;
    uint32_t rate_ = 44100;
    uint64_t total_ = 0;
    uint32_t cap_[LEVELS] = {0, 0, 0};             /* points each level can hold */
    uint32_t off_[LEVELS] = {0, 0, 0};             /* byte offset of each level in the storage */
    uint32_t n_[LEVELS] = {0, 0, 0};               /* complete points */
    int8_t   acc_[LEVELS][4] = {{0}};              /* the point being gathered at each level */
    uint32_t fill_[LEVELS] = {0, 0, 0};            /* frames (level 0) or points (levels 1, 2) gathered into acc_ */
    bool     finished_ = false;
};
