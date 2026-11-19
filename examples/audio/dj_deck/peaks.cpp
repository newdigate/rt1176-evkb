/* peaks.cpp - see peaks.h.  Copyright (c) 2026 Nicholas Newdigate.  SPDX-License-Identifier: MIT */
#include "peaks.h"
#include "audio_codecs/preview/preview_types.h"
#include <string.h>

using audio_codecs::preview::ApvHeader;
using audio_codecs::preview::APV_MAGIC;
using audio_codecs::preview::APV_VERSION;
using audio_codecs::preview::APV_FLAG_STEREO;
using audio_codecs::preview::APV_FLAG_HAS_LODS;

static const uint32_t kPer[PeakBuilder::LEVELS] = {128u, 16u, 16u};     /* what one point of each level gathers */
static const uint32_t kFrames[PeakBuilder::LEVELS] = {128u, 2048u, 32768u};

static void level_caps(uint64_t total_frames, uint32_t cap[PeakBuilder::LEVELS])
{
    uint64_t c = (total_frames + 127u) / 128u;
    for (int l = 0; l < PeakBuilder::LEVELS; l++) {
        cap[l] = (uint32_t)c;
        c = (c + 15u) / 16u;
    }
}

size_t PeakBuilder::storageBytes(uint64_t total_frames, uint8_t channels)
{
    uint32_t cap[LEVELS];
    level_caps(total_frames, cap);
    const size_t bpp = channels == 1 ? 2u : 4u;
    return sizeof(ApvHeader) + ((size_t)cap[0] + cap[1] + cap[2]) * bpp;
}

bool PeakBuilder::begin(uint8_t *storage, size_t cap, uint8_t channels, uint32_t sample_rate, uint64_t total_frames)
{
    base_ = nullptr;
    if (!storage || (channels != 1 && channels != 2) || cap < storageBytes(total_frames, channels)) return false;
    ch_ = channels; bpp_ = (uint8_t)(2 * channels); rate_ = sample_rate; total_ = total_frames;
    level_caps(total_frames, cap_);
    uint32_t off = sizeof(ApvHeader);
    for (int l = 0; l < LEVELS; l++) { off_[l] = off; off += cap_[l] * bpp_; n_[l] = 0; fill_[l] = 0; memset(acc_[l], 0, 4); }
    memset(storage, 0, sizeof(ApvHeader));
    finished_ = false;
    base_ = storage;
    return true;
}

/* Merge one finished point of the level below into this level's gathering point. */
void PeakBuilder::fold(int level, const int8_t *pt)
{
    int8_t *a = acc_[level];
    if (fill_[level] == 0) memcpy(a, pt, bpp_);
    else for (int c = 0; c < bpp_; c += 2) { if (pt[c] < a[c]) a[c] = pt[c]; if (pt[c + 1] > a[c + 1]) a[c + 1] = pt[c + 1]; }
    if (++fill_[level] == kPer[level]) close_point(level);
}

/* The gathering point of this level is complete: store it and pass it up. */
void PeakBuilder::close_point(int level)
{
    if (n_[level] < cap_[level]) {
        memcpy(base_ + off_[level] + (size_t)n_[level] * bpp_, acc_[level], bpp_);
        n_[level]++;
        if (level + 1 < LEVELS) fold(level + 1, acc_[level]);
    }
    fill_[level] = 0;
    memset(acc_[level], 0, 4);
}

void PeakBuilder::push(const int16_t *pcm, uint32_t frames)
{
    if (!base_ || finished_ || !pcm) return;
    int8_t *a = acc_[0];
    for (uint32_t f = 0; f < frames; f++) {
        if (n_[0] >= cap_[0]) return;                       /* more than was announced: dropped */
        /* PreviewGenerator's scale_pcm: the top byte.  Min and max start at 0. */
        for (int c = 0; c < ch_; c++) {
            const int8_t v = (int8_t)(pcm[(size_t)f * ch_ + c] >> 8);
            if (v < a[2 * c]) a[2 * c] = v;
            if (v > a[2 * c + 1]) a[2 * c + 1] = v;
        }
        if (++fill_[0] == kPer[0]) close_point(0);
    }
}

void PeakBuilder::finish(uint64_t actual_frames)
{
    if (!base_ || finished_) return;
    /* partial points, lowest level first, each feeding the one above */
    for (int l = 0; l < LEVELS; l++) if (fill_[l] != 0) close_point(l);
    /* pack the levels: the file has no gaps */
    uint32_t off = sizeof(ApvHeader);
    for (int l = 0; l < LEVELS; l++) {
        if (off != off_[l]) memmove(base_ + off, base_ + off_[l], (size_t)n_[l] * bpp_);
        off_[l] = off;
        off += n_[l] * bpp_;
    }
    ApvHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = APV_MAGIC;
    h.version = APV_VERSION;
    h.flags = (uint16_t)((ch_ == 2 ? APV_FLAG_STEREO : 0) | APV_FLAG_HAS_LODS);
    h.sample_rate = rate_;
    h.channels = ch_;
    h.bytes_per_chunk = bpp_;
    h.samples_per_base_chunk = 128;
    h.total_pcm_frames = actual_frames;
    h.duration_ms = rate_ ? (uint32_t)(actual_frames * 1000ull / rate_) : 0;
    h.lod_count = 3;
    static const uint32_t ratio[LEVELS] = {1, 16, 256};
    for (int l = 0; l < LEVELS; l++) { h.lods[l].downsample_ratio = ratio[l]; h.lods[l].chunk_count = n_[l]; h.lods[l].file_offset = off_[l]; }
    memcpy(base_, &h, sizeof(h));
    total_ = actual_frames;
    finished_ = true;
}

bool PeakBuilder::adopt(uint8_t *image, size_t size)
{
    base_ = nullptr;
    if (!image || size < sizeof(ApvHeader)) return false;
    ApvHeader h;
    memcpy(&h, image, sizeof(h));
    if (h.magic != APV_MAGIC || h.version != APV_VERSION || h.lod_count != 3) return false;
    if ((h.channels != 1 && h.channels != 2) || h.bytes_per_chunk != 2 * h.channels || h.samples_per_base_chunk != 128) return false;
    uint64_t expect = sizeof(ApvHeader);
    for (int l = 0; l < LEVELS; l++) {
        /* the levels follow one another with no gaps, and all of it is inside the file */
        if (h.lods[l].file_offset != expect) return false;
        expect += (uint64_t)h.lods[l].chunk_count * h.bytes_per_chunk;
        if (expect > size) return false;
    }
    if (h.lods[0].chunk_count != (h.total_pcm_frames + 127u) / 128u) return false;
    ch_ = h.channels; bpp_ = h.bytes_per_chunk; rate_ = h.sample_rate; total_ = h.total_pcm_frames;
    for (int l = 0; l < LEVELS; l++) { off_[l] = (uint32_t)h.lods[l].file_offset; n_[l] = cap_[l] = h.lods[l].chunk_count; fill_[l] = 0; }
    finished_ = true;
    base_ = image;
    return true;
}

const int8_t *PeakBuilder::table(int level) const
{
    return (base_ && level >= 0 && level < LEVELS) ? (const int8_t *)(base_ + off_[level]) : nullptr;
}
uint32_t PeakBuilder::points(int level) const { return (base_ && level >= 0 && level < LEVELS) ? n_[level] : 0; }
uint32_t PeakBuilder::framesPerPoint(int level) const { return (level >= 0 && level < LEVELS) ? kFrames[level] : 0; }

size_t PeakBuilder::imageBytes() const
{
    if (!base_ || !finished_) return 0;
    return (size_t)off_[LEVELS - 1] + (size_t)n_[LEVELS - 1] * bpp_;
}

void PeakBuilder::stamp(uint32_t source_file_size, uint32_t source_header_crc32)
{
    if (!base_ || !finished_) return;
    ApvHeader h;
    memcpy(&h, base_, sizeof(h));
    h.source_file_size = source_file_size;
    h.source_header_crc32 = source_header_crc32;
    memcpy(base_, &h, sizeof(h));
}
uint32_t PeakBuilder::sourceFileSize() const { ApvHeader h; if (!base_ || !finished_) return 0; memcpy(&h, base_, sizeof(h)); return h.source_file_size; }
uint32_t PeakBuilder::sourceHeaderCrc() const { ApvHeader h; if (!base_ || !finished_) return 0; memcpy(&h, base_, sizeof(h)); return h.source_header_crc32; }
