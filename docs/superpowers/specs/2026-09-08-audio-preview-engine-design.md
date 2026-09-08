# Audio Preview Engine & APV File Format Specification

**Status:** Approved Design Spec  
**Target:** RT1176 / Teensy Cores (Cortex-M7 / Cortex-M4), Linux / POSIX  
**Namespace:** `audio_codecs::preview`  
**Location:** `include/audio_codecs/preview/`, `src/preview/` in `audio-codecs`  
**Linear Issue:** [NEW-40](https://linear.app/newdigate/issue/NEW-40/audio-preview-engine-and-apv-file-format-for-rt1176-teensy)  

---

## 1. Executive Summary

This document specifies a zero-allocation, embedded-friendly audio waveform preview engine and binary file format (`.apv` / `APV1`) designed for microcontrollers (MIMXRT1176, Teensy 4.0/4.1/MicroMod) and desktop platforms.

The engine processes audio streams decoded by `audio-codecs` (WAV, MP3, FLAC, Vorbis, AAC, AIFF) and segments the decoded audio into constant 512-byte PCM sections (128 sample frames, $\approx 2.9\text{ ms}$ at 44.1 kHz). For each section, it calculates bipolar peak amplitude envelopes (`int8_t min`, `int8_t max` per channel) and organizes them into a multi-resolution hierarchical Level-of-Detail (LOD) pyramid. 

The resulting `.apv` file enables:
1. **Instant full-track overview rendering**: Displaying an entire 5-minute song across a 320–800 pixel UI display from a single $\le 512\text{-byte}$ read ($<1\text{ ms}$ query time).
2. **Smooth live scrolling & zoomed waveforms**: Delivering sample-accurate peak preservation and linear interpolation for audio playback position tracking.
3. **Cooperative execution**: Non-blocking generation running in an Arduino `loop()`, FreeRTOS task, or secondary Cortex-M4 core without stalling real-time audio I2S interrupts or UI rendering.

---

## 2. Binary File Format Specification (`.apv`)

### 2.1 File Structure Overview

All integers in `.apv` are stored in Little-Endian byte order. The layout consists of a fixed 128-byte header followed by contiguous chunk arrays for each Level of Detail:

```
+-----------------------------------------------------------------------------------+
| ApvHeader (Fixed 128 bytes, sector-aligned)                                       |
|   magic: "APV1" (0x31565041)                                                      |
|   format metadata, sample rate, channels, total frames, duration                  |
|   lod[4] table (offsets, ratios, chunk counts)                                    |
+-----------------------------------------------------------------------------------+
| LOD 0: Base Chunks (1x downsample = 128 frames / chunk, 512B stereo PCM section)  |
|   Contiguous array of 4-byte (Stereo) or 2-byte (Mono) chunks                     |
+-----------------------------------------------------------------------------------+
| LOD 1: Overview Tier (16x downsample = 2048 frames / chunk)                       |
|   Contiguous array of chunks                                                      |
+-----------------------------------------------------------------------------------+
| LOD 2: Full-Track Thumbnail Tier (256x downsample = 32768 frames / chunk)         |
|   Contiguous array of chunks                                                      |
+-----------------------------------------------------------------------------------+
```

### 2.2 Header Definition

```cpp
#pragma once
#include <cstdint>
#include <cstddef>

namespace audio_codecs::preview {

constexpr uint32_t APV_MAGIC = 0x31565041; // "APV1" in ASCII Little-Endian
constexpr uint16_t APV_VERSION = 1;
constexpr uint16_t BASE_CHUNK_FRAMES = 128; // 128 frames = 512 bytes of 16-bit stereo PCM

enum ApvFlags : uint16_t {
    APV_FLAG_STEREO    = (1 << 0), // 0 = Mono, 1 = Stereo
    APV_FLAG_HAS_LODS  = (1 << 1), // 1 if LOD hierarchy is present
};

#pragma pack(push, 1)

struct ApvLodDescriptor {
    uint32_t downsample_ratio; // 1 for LOD 0, 16 for LOD 1, 256 for LOD 2
    uint32_t chunk_count;      // Total chunks stored in this LOD tier
    uint64_t file_offset;      // Byte offset from start of file to chunk data
};

struct ApvHeader {
    uint32_t magic;                    // 0x31565041 ("APV1")
    uint16_t version;                  // Format version (1)
    uint16_t flags;                    // ApvFlags bitmask
    uint32_t sample_rate;              // Audio sample rate (e.g., 44100, 48000)
    uint8_t  channels;                 // 1 (Mono) or 2 (Stereo)
    uint8_t  bytes_per_chunk;          // 2 (Mono) or 4 (Stereo)
    uint16_t samples_per_base_chunk;   // 128
    uint64_t total_pcm_frames;         // Total sample frames in original audio track
    uint32_t duration_ms;              // Total track duration in milliseconds
    uint32_t source_file_size;         // Original audio file size (for cache validation)
    uint32_t source_header_crc32;      // CRC32 of first 4 KB of audio file
    uint8_t  lod_count;                // Number of LOD tiers stored (typically 3)
    uint8_t  reserved[7];              // Reserved padding to align LOD descriptors
    ApvLodDescriptor lods[4];          // Up to 4 LOD descriptors (64 bytes total)
    uint8_t  padding[16];              // Pads struct to exactly 128 bytes
};

static_assert(sizeof(ApvHeader) == 128, "ApvHeader must be exactly 128 bytes");

#pragma pack(pop)

} // namespace audio_codecs::preview
```

### 2.3 Chunk Envelopes

Audio samples in 16-bit PCM $[-32768, 32767]$ are mapped to signed 8-bit peak bounds $[-128, 127]$:
$$\text{peak}_8 = \text{clamp}(x \gg 8, -128, 127)$$

- **Mono Chunk (2 bytes)**:
  - `int8_t min`: Negative trough in range $[-128, 0]$.
  - `int8_t max`: Positive peak in range $[0, 127]$.
- **Stereo Chunk (4 bytes)**:
  - `int8_t left_min`: Left channel negative trough.
  - `int8_t left_max`: Left channel positive peak.
  - `int8_t right_min`: Right channel negative trough.
  - `int8_t right_max`: Right channel positive peak.

### 2.4 Hierarchical Level of Detail (LOD)

| Level | Downsample Ratio | Frames / Chunk | Chunk Duration @ 44.1 kHz | 5-Min Song Size (Stereo) | 5-Min Song Size (Mono) |
|---|---|---|---|---|---|
| **LOD 0** | $1\times$ | $128$ | $2.902\text{ ms}$ | $413,440\text{ bytes} \approx 403.7\text{ KB}$ | $206,720\text{ bytes} \approx 201.9\text{ KB}$ |
| **LOD 1** | $16\times$ | $2048$ | $46.44\text{ ms}$ | $25,840\text{ bytes} \approx 25.2\text{ KB}$ | $12,920\text{ bytes} \approx 12.6\text{ KB}$ |
| **LOD 2** | $256\times$ | $32768$ | $743.0\text{ ms}$ | $1,616\text{ bytes} \approx 1.58\text{ KB}$ | $808\text{ bytes} \approx 0.79\text{ KB}$ |
| **Total** | — | — | — | $\approx 440.9\text{ KB}$ | $\approx 220.5\text{ KB}$ |

*Note: For stereo, 512 bytes of 16-bit stereo PCM contains 128 frames (2 channels $\times$ 2 bytes = 4 bytes/frame; $512 / 4 = 128$). LOD 0 stores 4 bytes per chunk. The total overhead of storing all 3 LOD tiers compared to flat LOD 0 is $+6.67\%$.*

---

## 3. Storage I/O Abstraction

To keep `audio-codecs` strictly standard C++17 without third-party or Arduino library dependencies, I/O is abstracted through zero-heap interfaces:

```cpp
namespace audio_codecs::preview {

class SeekableReader {
public:
    virtual ~SeekableReader() = default;
    virtual size_t read(uint8_t* dest, size_t bytes) = 0;
    virtual bool seek(uint64_t position) = 0;
    virtual uint64_t position() const = 0;
    virtual uint64_t size() const = 0;
};

class SeekableWriter {
public:
    virtual ~SeekableWriter() = default;
    virtual size_t write(const uint8_t* src, size_t bytes) = 0;
    virtual bool seek(uint64_t position) = 0;
    virtual uint64_t position() const = 0;
    virtual uint64_t size() const = 0;
    virtual void flush() = 0;
};

} // namespace audio_codecs::preview
```

### 3.1 Provided Stream Implementations
- **`MemoryReader` / `MemoryWriter`**: Zero-allocation views over RAM buffers (used in unit tests and memory playback).
- **`FileStreamReader` / `FileStreamWriter`**: Standard C `FILE*` wrapper for Linux/POSIX test binaries.
- **`TeensyFileStream<TFile>`** (`teensy_stream_adapter.h`): Duck-typed template wrapper binding Teensy `File`, `FsFile` (`SD.h`, `SdFat`) and `USBHost_t36` (`UsbFat`) without including `<Arduino.h>`.

---

## 4. Preview Generation Engine (`PreviewGenerator`)

### 4.1 Cooperative Generation Architecture

The generator operates as a non-blocking state machine. It accepts an audio source reader, an audio decoder (`AudioDecoder`), and a preview output writer.

```cpp
enum class GeneratorStatus {
    Working,       // In progress; yield budget reached, call step() again
    Complete,      // All LODs generated and header written
    ErrorSource,   // Audio decoder failure or corrupt input stream
    ErrorDest      // File write or seek failure
};

class PreviewGenerator {
public:
    PreviewGenerator();
    ~PreviewGenerator() = default;

    bool init(SeekableReader& audio_source, 
              SeekableWriter& preview_dest, 
              AudioDecoder& decoder,
              bool stereo = true);

    // Cooperative tick: decodes and writes up to `chunk_budget` base chunks
    GeneratorStatus step(size_t chunk_budget = 64);

    // Blocking convenience method for desktop CLI / background tasks
    bool generate_all();

    // Generation progress [0.0 .. 100.0]
    float progress() const;

    const ApvHeader& header() const;
};
```

### 4.2 Zero-RAM Multi-LOD Generation Pipeline

To ensure the generator runs reliably on microcontrollers with limited RAM regardless of track duration:

1. **Pass 1: Streaming Audio Decode $\to$ LOD 0**:
   - The generator reserves the first 128 bytes of the output file with a zeroed placeholder header.
   - It reads raw audio chunks from `audio_source`, passes them to `decoder.decode_frame_i16()`, and fills a 128-frame PCM accumulation buffer.
   - For each 128 frames, it computes `min` and `max` per channel and writes the 4-byte chunk directly to LOD 0 (`file_offset = 128 + chunk_idx * 4`).
   - Running counters track `total_pcm_frames` and `lod[0].chunk_count`.
2. **Pass 2: LOD 1 Generation (Sector-Buffered Read-Back)**:
   - Upon audio EOF, the generator seeks back to offset `128` (start of LOD 0).
   - It reads 16 chunks at a time from LOD 0 using a 64-byte stack buffer, reduces them to an aggregate min/max, and appends the resulting chunk to the end of the file as **LOD 1**.
   - *Time*: For a 5-minute song, LOD 0 is $\approx 413\text{ KB}$. Reading $413\text{ KB}$ sequentially from SD card over 4-bit SDIO takes $<10\text{ ms}$.
3. **Pass 3: LOD 2 Generation (Sector-Buffered Read-Back)**:
   - The generator seeks to the start of LOD 1.
   - Reads 16 chunks at a time from LOD 1, aggregates min/max, and appends the resulting chunks as **LOD 2** ($\approx 1.6\text{ KB}$).
4. **Pass 4: Header Finalization**:
   - Computes total duration: $\text{duration\_ms} = \frac{\text{total\_pcm\_frames} \times 1000}{\text{sample\_rate}}$.
   - Sets exact file offsets and chunk counts for `lods[0..2]`.
   - Seeks back to offset `0` and writes the completed 128-byte `ApvHeader`.

---

## 5. Preview Query & Interpolation Engine (`PreviewReader`)

### 5.1 Query API

```cpp
struct WaveformPointMono {
    int8_t min;
    int8_t max;
};

struct WaveformPointStereo {
    int8_t left_min;
    int8_t left_max;
    int8_t right_min;
    int8_t right_max;
};

class PreviewReader {
public:
    PreviewReader();
    ~PreviewReader() = default;

    bool init(SeekableReader& preview_file);

    // Track metadata
    uint32_t duration_ms() const;
    uint32_t sample_rate() const;
    uint8_t  channels() const;
    uint64_t total_frames() const;
    const ApvHeader& header() const;

    // Window queries in milliseconds
    size_t read_preview(uint32_t start_ms, uint32_t duration_ms,
                        WaveformPointMono* out_points, size_t num_points);

    size_t read_preview_stereo(uint32_t start_ms, uint32_t duration_ms,
                               WaveformPointStereo* out_points, size_t num_points);
};
```

### 5.2 Automatic LOD Selection Algorithm

Let $T_{\text{pt}} = \frac{\text{duration\_ms}}{\text{num\_points}}$ be the duration represented by each output visual point. The reader compares $T_{\text{pt}}$ with the chunk period of each LOD tier:
- If $T_{\text{pt}} \ge 743.0\text{ ms}$: Select **LOD 2** ($256\times$).
- Else if $T_{\text{pt}} \ge 46.44\text{ ms}$: Select **LOD 1** ($16\times$).
- Else: Select **LOD 0** ($1\times$).

### 5.3 Decimation & Interpolation Logic

Let $R = \frac{\text{LOD chunks in window}}{\text{num\_points}}$:

1. **Zoomed-Out ($R \ge 1.0$) — Peak-Preserving Decimation**:
   - Visual bin $i \in [0, \text{num\_points}-1]$ covers chunks $[c_{\text{start}}, c_{\text{end}}]$.
   - Aggregates the extreme envelope:
     $$\text{out}[i].\min = \min_{c \in [c_{\text{start}}, c_{\text{end}}]} (\text{chunk}[c].\min)$$
     $$\text{out}[i].\max = \max_{c \in [c_{\text{start}}, c_{\text{end}}]} (\text{chunk}[c].\max)$$
   - *Guarantee*: Isolated drum beats or single-sample clicks never disappear.

2. **Zoomed-In ($R < 1.0$) — Linear Peak Interpolation**:
   - Visual bin $i$ maps to fractional chunk position:
     $$c_f = c_{\text{window\_start}} + i \times R, \quad c_0 = \lfloor c_f \rfloor, \quad \alpha = c_f - c_0$$
   - Linearly interpolates bounds:
     $$\text{out}[i].\min = \text{round}\big((1 - \alpha) \cdot \text{chunk}[c_0].\min + \alpha \cdot \text{chunk}[c_0 + 1].\min\big)$$
     $$\text{out}[i].\max = \text{round}\big((1 - \alpha) \cdot \text{chunk}[c_0].\max + \alpha \cdot \text{chunk}[c_0 + 1].\max\big)$$
   - *Guarantee*: Smooth continuous waveform curves without stair-stepping artifacts.

---

## 6. Verification and Test Plan

### 6.1 Automated Desktop Test Suite (`tests/test_preview.cpp`)

1. **Struct Alignment & Magic Verification**:
   - Static asserts for `sizeof(ApvHeader) == 128` and struct member offsets.
   - Header roundtrip serialization to ensure bit-exact field matching.
2. **Impulse Transient Preservation**:
   - Synthesizes 10 seconds of digital zero with an isolated full-scale spike at $t = 500\text{ ms}$.
   - Asserts that LOD 0, LOD 1, and LOD 2 all register peak amplitude $+127$ in the corresponding visual bins.
3. **Decimation and Interpolation Math**:
   - Generates synthetic linear ramp envelopes and verifies that query results match analytical linear interpolation formulas within $\pm 1$ LSB.
4. **Cooperative Generator Equivalence**:
   - Compares output file generated via `step(8)` vs. `generate_all()` to ensure identical bit streams.
5. **Multi-Codec Parity**:
   - Encodes a 30-second test signal to WAV, MP3, and FLAC.
   - Generates `.apv` previews from all three and verifies envelope cross-correlation $>0.98$.
6. **Boundary Conditions**:
   - Zero-length audio file, query beyond track end (`start_ms > duration_ms`), single-point query (`num_points = 1`), and invalid header magic.

### 6.2 Hardware Benchmarking (RT1176 / Teensy 4.1)

1. **Storage Read Latency**:
   - Benchmark `read_preview()` for full 5-minute song overview into 320 pixels on uSD card. Target: $<1\text{ ms}$.
   - Benchmark live scrolling window query ($2\text{ s}$ duration into 320 pixels). Target: $<0.5\text{ ms}$.
2. **Generation Throughput**:
   - Benchmark generation time for a 5-minute 44.1 kHz 16-bit WAV file on SD card. Target: $\le 1.2\text{ s}$ total processing time on Cortex-M7 @ 600 MHz.
