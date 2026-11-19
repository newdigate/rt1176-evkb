/* track_source.h - one way to read any audio file the deck loads.
 *
 * WAV, AIFF, FLAC and MP3 through audio-codecs' decoders, fed from a file a
 * window at a time.  One track is open at a time: the decoders (131 KB for
 * MP3, 99 KB for FLAC) are constructed when needed in ONE static arena, and
 * that arena and the read window must be in fast memory.  On the RT1176 (data
 * cache off) that means DTCM, which is where plain statics go: the same 8 s
 * MP3 decodes in 1 s with the decoder there and 40 s with it in SDRAM
 * (examples/audio/audio_codecs_probe/transcript_hw_nosh.txt).
 *
 * What the adapters take care of, so the loader need not:
 *   - tags and artwork before or after the audio, of any size (RIFF, AIFF and
 *     FLAC chunks are walked with seeks; an ID3v2 tag is skipped by its size)
 *   - the length: exact for WAV / AIFF / FLAC; for MP3, which has none in its
 *     header, a pass over the frame headers (probe()), an upper bound that is
 *     right to within a frame or two
 *   - an MP3 starts at the first MPEG-1 Layer III header that is followed by
 *     another; MPEG-2 / 2.5 Layer III is refused (the decoder gets it wrong)
 *   - a truncated file ends early and cleanly; it never yields more frames
 *     than announced
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "audio_codecs/preview/preview_stream.h"

struct TrackInfo {
    uint32_t sample_rate = 0;
    uint8_t  channels = 0;          /* 1 or 2 */
    uint8_t  bit_depth = 0;         /* of the file; the output is always 16-bit */
    uint64_t total_frames = 0;      /* MP3: an upper bound */
};

enum class SrcStatus { Working, Ready, Error };

class TrackSource {
public:
    virtual ~TrackSource() = default;
    /* Parse the header.  Ready, Error, or Working: call probe() until it is not. */
    virtual SrcStatus open(audio_codecs::preview::SeekableReader &file) = 0;
    /* One bounded slice of the MP3 length scan (at most 64 KiB of file per call). */
    virtual SrcStatus probe() = 0;
    virtual const TrackInfo &info() const = 0;
    /* Interleaved 16-bit frames into dst.  Returns frames written, 0 at the end
     * of the track, < 0 on a decode error.  max_frames must be at least
     * minChunkFrames() (a FLAC or MP3 frame is decoded whole). */
    virtual int32_t decode(int16_t *dst, uint32_t max_frames) = 0;
    virtual uint32_t minChunkFrames() const = 0;
    virtual void close() = 0;
};

/* The adapter for a file name, by extension (case-blind); nullptr if the
 * format is not one of the four.  The instances are static: one track at a time. */
TrackSource *track_source_for_name(const char *name);

/* A read that returned nothing before the end of the file (the card went
 * away) is treated by the adapters as the end of the track; this says whether
 * that happened since the last open(). */
bool track_source_read_error();

/* The decoder arena, for other use while NO source is open (the loader builds
 * its analysis objects there once decoding is over: the fast memory is scarce
 * and the two are never needed together). */
void *track_source_arena(size_t *bytes);
