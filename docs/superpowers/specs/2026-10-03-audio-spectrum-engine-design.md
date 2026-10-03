# Audio Spectrum Engine & ASV File Format Specification

**Status:** Approved Design Spec  
**Target:** RT1176 / Teensy Cores (Cortex-M7 / Cortex-M4), Linux / macOS / POSIX  
**Namespace:** `audio_codecs::spectrum`  
**Location:** `include/audio_codecs/spectrum/`, `src/spectrum/` in `audio-codecs`  
**License:** MIT  

---

## 1. Executive Summary

This document specifies a cross-platform, zero-heap audio spectrum analysis engine and binary file format (`.asv` / `ASV1`) designed for microcontrollers (NXP MIMXRT1176, Teensy 4.0 / 4.1 / MicroMod) and desktop platforms (Linux, macOS, x86_64, aarch64).

The engine analyzes audio PCM streams decoded by `audio-codecs` (WAV, MP3, FLAC, Vorbis, AAC, AIFF). It segments audio into 1024-sample windows with 50% overlap (512-sample hop $\approx 11.6\text{ ms}$ at 44.1 kHz), computes magnitude spectra via a decoupled FFT backend, and maps the spectra into 64 perceptually log-spaced frequency bands quantized to unsigned 8-bit dB energy. The output is structured into a multi-resolution Level-of-Detail (LOD) pyramid stored in an `.asv` file.

The resulting `.asv` file enables:
1. **Instant full-track spectrogram overview queries**: Displaying a 3- to 10-minute track overview across a 320–800 pixel UI display from a single fast read of the LOD 1 overview tier ($<2\text{ ms}$ query time).
2. **Smooth live scrolling spectrograms**: Millisecond-window queries with peak-hold decimation (zoomed-out) and linear interpolation (zoomed-in) during playback.
3. **Sub-band transient and tempo analysis**: Preserving sharp rhythmic transients across frequency bands (sub-bass kick drum, mid snare, high hats) for subsequent tempo/beat detection algorithms.
4. **Cooperative execution**: Non-blocking generation running in an Arduino `loop()`, FreeRTOS task, or secondary Cortex-M4 core without starving audio I2S interrupts or UI rendering.

---

## 2. Binary File Format Specification (`.asv` — "ASV1")

### 2.1 File Structure Overview

All multi-byte integers in `.asv` are stored in Little-Endian byte order. The layout consists of a fixed 128-byte sector-aligned header followed by contiguous frame arrays for each Level of Detail:

```
+-----------------------------------------------------------------------------------+
| AsvHeader (Fixed 128 bytes, sector-aligned)                                       |
|   magic: "ASV1" (0x31565341)                                                      |
|   format metadata, sample rate, channels, num_bands, fft_size, hop_size, duration |
|   lods[4] table (downsample ratios, frame counts, byte offsets)                   |
+-----------------------------------------------------------------------------------+
| LOD 0: Base Frames (1x downsample = 512-sample hop, ~11.6 ms per frame)           |
|   Contiguous array of 64-byte (Mono) or 128-byte (Stereo) frames                  |
+-----------------------------------------------------------------------------------+
| LOD 1: Overview Tier (16x downsample = 8192-sample hop, ~185.8 ms per frame)      |
|   Contiguous array of 64-byte (Mono) or 128-byte (Stereo) frames                  |
+-----------------------------------------------------------------------------------+
```

### 2.2 Header Definition

```cpp
#pragma once
#include <cstdint>
#include <cstddef>

namespace audio_codecs::spectrum {

constexpr uint32_t ASV_MAGIC = 0x31565341; // "ASV1" in ASCII Little-Endian
constexpr uint16_t ASV_VERSION = 1;
constexpr uint16_t DEFAULT_FFT_SIZE = 1024;
constexpr uint16_t DEFAULT_HOP_SIZE = 512;
constexpr uint8_t  DEFAULT_NUM_BANDS = 64;

enum AsvFlags : uint16_t {
    ASV_FLAG_STEREO   = (1 << 0), // 0 = Mono (summed), 1 = Stereo
    ASV_FLAG_HAS_LODS = (1 << 1), // 1 if LOD hierarchy is present
};

#pragma pack(push, 1)

struct AsvLodDescriptor {
    uint32_t downsample_ratio; // 1 for base LOD 0, 16 for LOD 1
    uint32_t frame_count;      // Total spectrum frames stored in this LOD tier
    uint64_t file_offset;      // Byte offset from start of file to frame data
}; // sizeof = 16 bytes

struct AsvHeader {
    uint32_t magic;                    // 0x31565341 ("ASV1")
    uint16_t version;                  // Format version (1)
    uint16_t flags;                    // AsvFlags bitmask
    uint32_t sample_rate;              // Audio sample rate (e.g., 44100, 48000)
    uint8_t  channels;                 // 1 (Mono) or 2 (Stereo)
    uint8_t  num_bands;                // Frequency bands per frame (default: 64)
    uint16_t fft_size;                 // FFT window size (default: 1024)
    uint16_t hop_size;                 // Hop size in samples (default: 512)
    uint16_t min_freq_hz;              // Lower bound of filterbank (default: 20 Hz)
    uint16_t max_freq_hz;              // Upper bound of filterbank (default: 20000 Hz)
    uint16_t lod_count;                // Number of valid LOD tiers (typically 2)
    uint64_t total_pcm_frames;         // Total PCM sample frames in source audio
    uint32_t duration_ms;              // Total track duration in milliseconds
    AsvLodDescriptor lods[4];          // Up to 4 LOD descriptors (64 bytes total)
    uint8_t  reserved[24];             // Reserved padding to align struct to 128 bytes
};

static_assert(sizeof(AsvLodDescriptor) == 16, "AsvLodDescriptor must be exactly 16 bytes");
static_assert(sizeof(AsvHeader) == 128, "AsvHeader must be exactly 128 bytes");

#pragma pack(pop)

} // namespace audio_codecs::spectrum
```

### 2.3 Band Data Representation & Quantization

Each frequency band energy is quantized to an unsigned 8-bit integer (`uint8_t` in $[0, 255]$), mapping log-magnitude dB energy from $-96\text{ dB}$ (silence / digital zero) to $0\text{ dB}$ (digital full scale):

$$\text{val} = \text{clamp}\left(\left\lfloor \frac{\text{dB} + 96.0}{96.0} \times 255.0 \right\rfloor, 0, 255\right)$$

- Value `0`: Signal $\le -96\text{ dB}$ (below audible noise floor).
- Value `255`: Peak digital full scale ($0\text{ dBFS}$).
- Frame payload size:
  - Mono: `num_bands` bytes (64 bytes per frame).
  - Stereo: `num_bands * 2` bytes (128 bytes per frame: 64 Left bands followed by 64 Right bands).
- Direct $O(1)$ seek arithmetic:
  $$\text{offset} = \text{lod.file\_offset} + (\text{frame\_idx} \times \text{bytes\_per\_frame})$$

### 2.4 Multi-LOD Pyramid Specification

| Tier | Downsample Ratio | Hop Size (@ 44.1 kHz) | Frame Duration | 5-Minute Track Size | Purpose |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **LOD 0** | $1\times$ | 512 samples | $\approx 11.61\text{ ms}$ | $\approx 1.65\text{ MB}$ (25,839 frames) | High-res scrolling spectrogram & tempo beat-tracking |
| **LOD 1** | $16\times$ | 8,192 samples | $\approx 185.76\text{ ms}$ | $\approx 103\text{ KB}$ (1,615 frames) | Full-track overview thumbnail ($<2\text{ ms}$ query) |

---

## 3. FFT Decoupling & Architecture

### 3.1 Abstract Backend Interface (`fft_backend.h`)

Core headers remain completely free of `<Arduino.h>`, `<arm_math.h>`, or external dependencies:

```cpp
#pragma once
#include <cstdint>
#include <cstddef>

namespace audio_codecs::spectrum {

constexpr size_t FFT_SIZE = 1024;
constexpr size_t NUM_MAG_BINS = FFT_SIZE / 2; // 512 magnitude bins (DC to Nyquist)

class FftBackend {
public:
    virtual ~FftBackend() = default;

    // Windows 1024 real PCM samples (int16_t) and calculates 512 magnitude bins
    virtual void forward_1024(const int16_t* in_pcm, float* out_magnitudes_512) = 0;
};

// C-style function callback alternative for non-OOP integration:
using FftTransformFn = void (*)(const int16_t* in_pcm, float* out_magnitudes_512, void* user_data);

} // namespace audio_codecs::spectrum
```

### 3.2 Native C++17 Desktop Backend (`DesktopRealFftBackend`)

- Built directly into `audio_codecs_spectrum` under the **MIT License**.
- Self-contained Radix-2 Real FFT (~100 lines of standard C++17):
  - Precomputes bit-reversal and twiddle tables at initialization.
  - Transforms 1024 real samples into 512 complex frequency bins, computing magnitude via $\sqrt{\text{re}^2 + \text{im}^2}$.
  - Zero dynamic heap allocation during transform.
  - Execution speed: $\approx 25\,\mu\text{s}$ per 1024-point frame on host CPU.

### 3.3 Cortex-M7 / Teensy Adapter (`teensy_fft_adapter.h`)

- Header-only adapter for NXP RT1176 / Teensy 4.x:
  - Binds CMSIS-DSP `arm_cfft_radix4_q15` / `arm_rfft_fast_f32` or wraps `AudioAnalyzeFFT1024` from `newdigate/Audio`.
  - Employs Cortex-M7 dual-issue DSP instructions and hardware SIMD instructions.
  - Execution speed: **$< 40\,\mu\text{s}$** per 1024-point FFT on Cortex-M7 at 600–1000 MHz ($>150\times$ faster than real-time).
  - Intermediate buffers (1024-sample windowed Q15 array) are embedded statically within the adapter class.

---

## 4. Perceptual Filterbank & Quantization Math

### 4.1 Logarithmic Band Spacing

At 44.1 kHz, 1024-point FFT produces 512 bins spaced linearly at $\Delta f = 43.066\text{ Hz}$. To match human frequency perception and capture musical fundamentals:

1. **Center Frequencies**: For 64 bands between $f_{\min} = 20\text{ Hz}$ and $f_{\max} = 20000\text{ Hz}$:
   $$f_b = f_{\min} \cdot \left(\frac{f_{\max}}{f_{\min}}\right)^{\frac{b}{63}}, \quad b \in [0, 63]$$
2. **Bin Boundaries**: For each band $b$, calculate start frequency $f_{\text{low}}$ and end frequency $f_{\text{high}}$:
   $$k_{\text{start}} = \max\left(1, \left\lfloor \frac{f_{\text{low}}}{\Delta f} \right\rfloor\right), \quad k_{\text{end}} = \min\left(511, \left\lceil \frac{f_{\text{high}}}{\Delta f} \right\rceil\right)$$
3. **Energy Aggregation**:
   - For low bands where multiple bands share the same bin (sub-bass $< 80\text{ Hz}$): interpolate or sample the nearest bin magnitude.
   - For mid/high bands spanning multiple bins: sum energy $\sum_{k} \text{mag}_k^2$ or take peak magnitude.
4. **Logarithmic dB Compression**:
   $$\text{dB}_b = 10 \cdot \log_{10}\left(E_b + 10^{-9.6}\right)$$
   $$\text{band\_byte}[b] = \text{clamp}\left(\left\lfloor \frac{\text{dB}_b + 96.0}{96.0} \times 255.0 \right\rfloor, 0, 255\right)$$

---

## 5. Spectrum Generator Engine (`SpectrumGenerator`)

### 5.1 Class Interface

```cpp
#pragma once
#include "spectrum_types.h"
#include "fft_backend.h"
#include "audio_codecs/preview/preview_stream.h"

namespace audio_codecs::spectrum {

enum class GeneratorStatus {
    Ready,
    ProcessingLOD0,
    ProcessingLOD1,
    Finalizing,
    Complete,
    ErrorSource,
    ErrorDest,
    ErrorBackend
};

class SpectrumGenerator {
public:
    SpectrumGenerator();

    bool init(preview::SeekableReader& pcm_source,
              preview::SeekableWriter& spectrum_dest,
              FftBackend& fft_backend,
              uint32_t sample_rate,
              uint8_t source_channels,
              bool downmix_to_mono = true,
              uint8_t num_bands = DEFAULT_NUM_BANDS);

    // Executes up to frame_budget FFT frames per call.
    // Call cooperatively in loop() or RTOS task until Complete or Error.
    GeneratorStatus step(size_t frame_budget = 32);

    // Convenience batch method (runs step() until done)
    bool generate_all();

    float progress() const;
    GeneratorStatus status() const;

private:
    // Fixed internal stack/struct buffers (~5 KB total, zero heap)
};

} // namespace audio_codecs::spectrum
```

### 5.2 Multi-Pass Pipeline

1. **Pass 1: Base LOD 0 Generation (Streaming)**:
   - Reads 1024 samples from `pcm_source` (downmixing stereo to mono $(L+R)/2$).
   - Multiplies samples by 1024-point Hann window.
   - Computes FFT via `fft_backend.forward_1024()`.
   - Aggregates 512 magnitude bins into 64 log bands and quantizes to `uint8_t`.
   - Writes 64-byte frame to `spectrum_dest`.
   - Slides overlap window by 512 samples and repeats until EOF.
2. **Pass 2: Zero-RAM LOD 1 Reduction (Sector Read-Back)**:
   - Reads back LOD 0 from `spectrum_dest` in 512-byte sector chunks (8 frames of 64 bytes).
   - Aggregates 16 consecutive LOD 0 frames into 1 LOD 1 frame using **peak-hold per band**:
     $$\text{LOD1\_band}[b] = \max_{i=0\dots15} \left(\text{LOD0\_band}_i[b]\right)$$
   - Peak-hold ensures transients (kick hits, snares, rimshots) remain visible on full-track spectrogram overviews.
   - Sliced across `step()` calls; runs in $<5\text{ ms}$ on embedded SD cards.
3. **Pass 3: Header Patch**:
   - Seeks to file offset 0 and writes the completed `AsvHeader` with final frame counts and byte offsets.

### 5.3 Static Memory Budget

| Buffer | Size | Purpose |
| :--- | :--- | :--- |
| PCM Overlap Buffer | $1024 \times \text{sizeof(int16\_t)} = 2048\text{ B}$ | 1024-sample window buffer |
| Magnitude Buffer | $512 \times \text{sizeof(float)} = 2048\text{ B}$ | FFT magnitude bin output |
| Output Frame Buffer | $64 \times \text{sizeof(uint8\_t)} = 64\text{ B}$ | Current 64-band quantized frame |
| Hann Window Table | $1024 \times \text{sizeof(int16\_t)} = 2048\text{ B}$ | Stored in Flash / ROM |
| I/O Sector Buffer | $512\text{ B}$ | LOD 1 read-back buffer on stack |
| **Total Dynamic RAM** | **0 bytes** | **Pure zero-heap execution** |

---

## 6. Spectrum Reader Engine (`SpectrumReader`)

### 6.1 Class Interface

```cpp
#pragma once
#include "spectrum_types.h"
#include "audio_codecs/preview/preview_stream.h"

namespace audio_codecs::spectrum {

class SpectrumReader {
public:
    SpectrumReader();

    bool init(preview::SeekableReader& spectrum_source);

    // Metadata getters
    uint32_t duration_ms() const;
    uint32_t sample_rate() const;
    uint8_t  channels() const;
    uint8_t  num_bands() const;
    uint16_t hop_size() const;
    uint16_t lod_count() const;

    // Queries time window [start_ms, start_ms + duration_ms]
    // Writes up to max_frames frames (each num_bands bytes) into out_bands.
    // Returns actual number of frames written.
    size_t read_spectrum(uint32_t start_ms,
                         uint32_t duration_ms,
                         uint8_t* out_bands,
                         size_t max_frames);

private:
    preview::SeekableReader* source_{nullptr};
    AsvHeader header_{};
    uint8_t sector_cache_[512]{};
    uint64_t cached_sector_idx_{UINT64_MAX};

    uint8_t select_lod(uint32_t duration_ms, size_t target_frames) const;
};

} // namespace audio_codecs::spectrum
```

### 6.2 Adaptive LOD Selection & Resampling

- **Adaptive LOD Selection**: Computes time-per-pixel ratio $\tau = \frac{\text{duration\_ms}}{\text{target\_frames}}$:
  - If $\tau \ge 90\text{ ms}$ (full-track or zoomed-out overview), reads **LOD 1** (fast $<2\text{ ms}$ query).
  - If $\tau < 90\text{ ms}$ (zoomed-in live scrolling), reads **LOD 0**.
- **Peak-Hold Decimation**: When multiple source frames map to one output column, takes the maximum value for each band to preserve transients.
- **Linear Interpolation**: When output resolution exceeds source frame rate (deep zoom), linearly interpolates between adjacent frames.
- **512-Byte Sector Cache**: Retains the most recently read storage sector to avoid redundant SD/USB reads during live scrolling.

---

## 7. File Layout & CMake Architecture

```
audio-codecs/
├── include/audio_codecs/
│   ├── spectrum.h                      # Umbrella header
│   └── spectrum/
│       ├── spectrum_types.h            # AsvHeader, AsvLodDescriptor, constants
│       ├── fft_backend.h               # FftBackend abstract interface
│       ├── spectrum_generator.h        # SpectrumGenerator class
│       ├── spectrum_reader.h           # SpectrumReader class
│       ├── desktop_fft.h               # Native C++17 Real FFT (MIT)
│       └── teensy_fft_adapter.h        # Cortex-M7 CMSIS-DSP / newdigate/Audio adapter
├── src/spectrum/
│   ├── desktop_fft.cpp                 # Desktop FFT implementation
│   ├── spectrum_generator.cpp          # SpectrumGenerator implementation
│   └── spectrum_reader.cpp             # SpectrumReader implementation
└── tests/
    ├── test_spectrum_types.cpp         # Struct size & alignment assertions
    ├── test_spectrum_fft.cpp           # FFT accuracy on synthetic test sines
    ├── test_spectrum_generator.cpp     # Slicing, impulse preservation, multi-LOD
    ├── test_spectrum_reader.cpp        # Window queries, decimation, interpolation
    └── test_spectrum_integration.cpp   # End-to-end WAV -> ASV -> Reader query
```

---

## 8. Verification & Test Plan

1. **Format & Alignment Tests (`test_spectrum_types.cpp`)**:
   - `static_assert(sizeof(AsvHeader) == 128)` and `static_assert(sizeof(AsvLodDescriptor) == 16)`.
   - Verify Little-Endian serialization and round-trip consistency.
2. **FFT & Filterbank Tests (`test_spectrum_fft.cpp`)**:
   - Test synthetic sine tones (e.g. 440 Hz, 1 kHz, 5 kHz) on `DesktopRealFftBackend`.
   - Verify peak frequency bin matches known input frequency within $\pm 1$ bin.
   - Verify 64-band log filterbank isolates test tone energy into expected band index.
3. **Generator Tests (`test_spectrum_generator.cpp`)**:
   - Impulse preservation: synthetic impulse injected at known timestamp preserves peak band energy in both LOD 0 and LOD 1.
   - Slicing equivalence: verify `step(8)` across calls produces bit-identical output to `generate_all()`.
   - Truncated source file and destination write error handling.
4. **Reader Tests (`test_spectrum_reader.cpp`)**:
   - Full-track query vs sub-window queries.
   - Bounds checking (seek beyond duration, zero duration, clamping).
   - LOD selection threshold verification.
   - Decimation peak-hold verification.
5. **End-to-End Integration Test (`test_spectrum_integration.cpp`)**:
   - Generate synthetic stereo WAV $\to$ decode via `audio-codecs` WAV decoder $\to$ run `SpectrumGenerator` $\to$ query via `SpectrumReader` $\to$ verify frequency content, duration, and LODs match source.
