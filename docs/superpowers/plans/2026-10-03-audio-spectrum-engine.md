# Audio Spectrum Engine & ASV File Format Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a zero-allocation, embedded-friendly audio spectrum analysis engine and binary file format (`.asv` / `ASV1`) within `audio-codecs` (`audio_codecs::spectrum`), targeting RT1176 / Teensy microcontrollers and desktop platforms.

**Architecture:** The engine processes audio PCM streams in 1024-sample windows with 50% overlap (512-sample hop, $\approx 11.6\text{ ms}$ at 44.1 kHz). It computes magnitude spectra using a decoupled FFT backend, maps magnitudes into 64 log-spaced frequency bands quantized to unsigned 8-bit dB energy, and outputs a sector-aligned multi-resolution Level-of-Detail pyramid (LOD 0 = $1\times$, LOD 1 = $16\times$) using zero dynamic heap RAM. The reader selects the optimal LOD and provides peak-hold decimation for zoomed-out overviews and linear interpolation for zoomed-in scrolling.

**Tech Stack:** Pure C++17 (zero heap allocations, zero external runtime dependencies), CMake, CTest, CMSIS-DSP / Teensy Audio adapter, native C++17 Radix-2 Real FFT (MIT).

**Spec:** `docs/superpowers/specs/2026-10-03-audio-spectrum-engine-design.md`

## Global Constraints

- Pure C++17; no dynamic heap allocations (`malloc`, `new`, `std::vector`) during steady-state spectrum generation or reading.
- No Arduino or platform headers inside core headers; storage I/O must use abstract `preview::SeekableReader` / `preview::SeekableWriter`.
- `sizeof(AsvHeader)` must be exactly 128 bytes (asserted via compile-time `static_assert`).
- `sizeof(AsvLodDescriptor)` must be exactly 16 bytes (asserted via compile-time `static_assert`).
- All integers stored in Little-Endian byte order.
- Base hop size is fixed at 512 sample frames (1024-sample FFT window).
- All native desktop code is 100% MIT-licensed.

---

### Task 1: Core Spectrum Data Structures & Format Header (`spectrum_types.h`)

**Files:**
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/spectrum/spectrum_types.h`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_spectrum_types.cpp`
- Modify: `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`

**Interfaces:**
- Consumes: Standard `<cstdint>`, `<cstddef>`
- Produces:
  - `ASV_MAGIC = 0x31565341`
  - `ASV_VERSION = 1`
  - `DEFAULT_FFT_SIZE = 1024`
  - `DEFAULT_HOP_SIZE = 512`
  - `DEFAULT_NUM_BANDS = 64`
  - `enum AsvFlags : uint16_t { ASV_FLAG_STEREO = 1, ASV_FLAG_HAS_LODS = 2 }`
  - `struct AsvLodDescriptor { uint32_t downsample_ratio; uint32_t frame_count; uint64_t file_offset; }`
  - `struct AsvHeader` (128 bytes, packed)

- [ ] **Step 1: Write the failing test**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_spectrum_types.cpp`:
```cpp
#include "audio_codecs/spectrum/spectrum_types.h"
#include <cassert>
#include <cstring>
#include <iostream>

using namespace audio_codecs::spectrum;

int main() {
    static_assert(sizeof(AsvLodDescriptor) == 16, "AsvLodDescriptor must be 16 bytes");
    static_assert(sizeof(AsvHeader) == 128, "AsvHeader must be exactly 128 bytes");

    AsvHeader hdr{};
    hdr.magic = ASV_MAGIC;
    hdr.version = ASV_VERSION;
    hdr.flags = ASV_FLAG_HAS_LODS;
    hdr.sample_rate = 44100;
    hdr.channels = 1;
    hdr.num_bands = DEFAULT_NUM_BANDS;
    hdr.fft_size = DEFAULT_FFT_SIZE;
    hdr.hop_size = DEFAULT_HOP_SIZE;
    hdr.min_freq_hz = 20;
    hdr.max_freq_hz = 20000;
    hdr.lod_count = 2;
    hdr.total_pcm_frames = 44100 * 60;
    hdr.duration_ms = 60000;

    assert(hdr.magic == 0x31565341);
    assert(hdr.version == 1);
    assert(hdr.channels == 1);
    assert(hdr.num_bands == 64);
    assert(hdr.fft_size == 1024);
    assert(hdr.hop_size == 512);
    assert(sizeof(hdr) == 128);

    std::cout << "test_spectrum_types PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake -B /Users/moolet/Development/github/newdigate/audio-codecs/build -S /Users/moolet/Development/github/newdigate/audio-codecs
```
Expected: FAIL (missing header `audio_codecs/spectrum/spectrum_types.h`).

- [ ] **Step 3: Write minimal implementation**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/spectrum/spectrum_types.h`:
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
};

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

Add test executable to `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`:
```cmake
add_executable(test_spectrum_types tests/test_spectrum_types.cpp)
target_include_directories(test_spectrum_types PRIVATE include)
add_test(NAME SpectrumTypesTest COMMAND test_spectrum_types)
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cmake -B /Users/moolet/Development/github/newdigate/audio-codecs/build -S /Users/moolet/Development/github/newdigate/audio-codecs && cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_spectrum_types && ctest --test-dir /Users/moolet/Development/github/newdigate/audio-codecs/build -R SpectrumTypesTest --output-on-failure
```
Expected: PASS (`test_spectrum_types PASSED`).

- [ ] **Step 5: Commit**

```bash
git -C /Users/moolet/Development/github/newdigate/audio-codecs add include/audio_codecs/spectrum/spectrum_types.h tests/test_spectrum_types.cpp CMakeLists.txt
git -C /Users/moolet/Development/github/newdigate/audio-codecs commit -m "feat(spectrum): implement core spectrum data structures and format header"
```

---

### Task 2: FFT Decoupling, Native C++17 Real FFT & Teensy Adapter

**Files:**
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/spectrum/fft_backend.h`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/spectrum/desktop_fft.h`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/src/spectrum/desktop_fft.cpp`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/spectrum/teensy_fft_adapter.h`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_spectrum_fft.cpp`
- Modify: `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`

**Interfaces:**
- Consumes: `<cstdint>`, `<cstddef>`, `<cmath>`
- Produces:
  - `class FftBackend` with virtual `void forward_1024(const int16_t* in_pcm, float* out_magnitudes_512) = 0`
  - `class DesktopRealFftBackend : public FftBackend`
  - `template <typename TFft> class TeensyAudioFftAdapter : public FftBackend`

- [ ] **Step 1: Write the failing test**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_spectrum_fft.cpp`:
```cpp
#include "audio_codecs/spectrum/desktop_fft.h"
#include <cassert>
#include <cmath>
#include <vector>
#include <iostream>

using namespace audio_codecs::spectrum;

int main() {
    DesktopRealFftBackend fft;
    assert(fft.init());

    // Generate 1024 samples of a 440 Hz sine wave at 44100 Hz sample rate
    // Expected peak bin: round(440 * 1024 / 44100) = round(10.22) = 10
    std::vector<int16_t> pcm(1024);
    for (size_t i = 0; i < 1024; ++i) {
        float t = static_cast<float>(i) / 44100.0f;
        pcm[i] = static_cast<int16_t>(std::sin(2.0f * 3.14159265f * 440.0f * t) * 30000.0f);
    }

    float mags[512] = {0};
    fft.forward_1024(pcm.data(), mags);

    size_t peak_bin = 0;
    float peak_mag = 0.0f;
    for (size_t i = 1; i < 512; ++i) {
        if (mags[i] > peak_mag) {
            peak_mag = mags[i];
            peak_bin = i;
        }
    }

    assert(peak_bin == 10);
    assert(peak_mag > 1000.0f);

    std::cout << "test_spectrum_fft PASSED (peak_bin=" << peak_bin << ")\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_spectrum_fft
```
Expected: FAIL (missing `desktop_fft.h`).

- [ ] **Step 3: Write minimal implementation**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/spectrum/fft_backend.h`:
```cpp
#pragma once
#include <cstdint>
#include <cstddef>

namespace audio_codecs::spectrum {

constexpr size_t FFT_SIZE = 1024;
constexpr size_t NUM_MAG_BINS = FFT_SIZE / 2; // 512 bins (DC to Nyquist)

class FftBackend {
public:
    virtual ~FftBackend() = default;
    virtual void forward_1024(const int16_t* in_pcm, float* out_magnitudes_512) = 0;
};

using FftTransformFn = void (*)(const int16_t* in_pcm, float* out_magnitudes_512, void* user_data);

} // namespace audio_codecs::spectrum
```

Create `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/spectrum/desktop_fft.h`:
```cpp
#pragma once
#include "fft_backend.h"

namespace audio_codecs::spectrum {

class DesktopRealFftBackend : public FftBackend {
public:
    DesktopRealFftBackend();
    ~DesktopRealFftBackend() override = default;

    bool init();
    void forward_1024(const int16_t* in_pcm, float* out_magnitudes_512) override;

private:
    float twiddle_cos_[512]{};
    float twiddle_sin_[512]{};
    uint16_t bit_reverse_[1024]{};
    bool initialized_{false};
};

} // namespace audio_codecs::spectrum
```

Create `/Users/moolet/Development/github/newdigate/audio-codecs/src/spectrum/desktop_fft.cpp`:
```cpp
#include "audio_codecs/spectrum/desktop_fft.h"
#include <cmath>

namespace audio_codecs::spectrum {

DesktopRealFftBackend::DesktopRealFftBackend() {
    init();
}

bool DesktopRealFftBackend::init() {
    if (initialized_) return true;

    // Bit-reversal table for 1024 points (10 bits)
    for (uint32_t i = 0; i < 1024; ++i) {
        uint32_t rev = 0;
        uint32_t temp = i;
        for (int j = 0; j < 10; ++j) {
            rev = (rev << 1) | (temp & 1);
            temp >>= 1;
        }
        bit_reverse_[i] = static_cast<uint16_t>(rev);
    }

    // Twiddle factors for N=1024
    constexpr float kPi = 3.14159265358979323846f;
    for (size_t i = 0; i < 512; ++i) {
        float angle = -2.0f * kPi * static_cast<float>(i) / 1024.0f;
        twiddle_cos_[i] = std::cos(angle);
        twiddle_sin_[i] = std::sin(angle);
    }

    initialized_ = true;
    return true;
}

void DesktopRealFftBackend::forward_1024(const int16_t* in_pcm, float* out_magnitudes_512) {
    if (!initialized_) init();

    float real[1024];
    float imag[1024];

    // Bit-reverse reordering and int16 to float
    for (size_t i = 0; i < 1024; ++i) {
        uint16_t rev = bit_reverse_[i];
        real[i] = static_cast<float>(in_pcm[rev]);
        imag[i] = 0.0f;
    }

    // Cooley-Tukey Radix-2 FFT
    for (size_t half_size = 1; half_size < 1024; half_size <<= 1) {
        size_t step = half_size << 1;
        size_t twiddle_step = 512 / half_size;

        for (size_t k = 0; k < 1024; k += step) {
            for (size_t j = 0; j < half_size; ++j) {
                size_t tw_idx = j * twiddle_step;
                float c = twiddle_cos_[tw_idx];
                float s = twiddle_sin_[tw_idx];

                size_t u = k + j;
                size_t v = u + half_size;

                float tr = real[v] * c - imag[v] * s;
                float ti = real[v] * s + imag[v] * c;

                real[v] = real[u] - tr;
                imag[v] = imag[u] - ti;
                real[u] += tr;
                imag[u] += ti;
            }
        }
    }

    // Output 512 magnitude bins
    for (size_t i = 0; i < 512; ++i) {
        out_magnitudes_512[i] = std::sqrt(real[i] * real[i] + imag[i] * imag[i]);
    }
}

} // namespace audio_codecs::spectrum
```

Create `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/spectrum/teensy_fft_adapter.h`:
```cpp
#pragma once
#include "fft_backend.h"
#include <cmath>

namespace audio_codecs::spectrum {

template <typename TFft>
class TeensyAudioFftAdapter : public FftBackend {
public:
    explicit TeensyAudioFftAdapter(TFft& fft_instance) : fft_(fft_instance) {}

    void forward_1024(const int16_t* in_pcm, float* out_magnitudes_512) override {
        fft_.forward_1024(in_pcm, out_magnitudes_512);
    }

private:
    TFft& fft_;
};

} // namespace audio_codecs::spectrum
```

Update `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`:
```cmake
add_library(audio_codecs_spectrum
    src/spectrum/desktop_fft.cpp
)
target_include_directories(audio_codecs_spectrum PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(audio_codecs_spectrum PUBLIC cxx_std_17)

add_executable(test_spectrum_fft tests/test_spectrum_fft.cpp)
target_link_libraries(test_spectrum_fft PRIVATE audio_codecs_spectrum)
add_test(NAME SpectrumFftTest COMMAND test_spectrum_fft)
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cmake -B /Users/moolet/Development/github/newdigate/audio-codecs/build -S /Users/moolet/Development/github/newdigate/audio-codecs && cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_spectrum_fft && ctest --test-dir /Users/moolet/Development/github/newdigate/audio-codecs/build -R SpectrumFftTest --output-on-failure
```
Expected: PASS (`test_spectrum_fft PASSED (peak_bin=10)`).

- [ ] **Step 5: Commit**

```bash
git -C /Users/moolet/Development/github/newdigate/audio-codecs add include/audio_codecs/spectrum/fft_backend.h include/audio_codecs/spectrum/desktop_fft.h src/spectrum/desktop_fft.cpp include/audio_codecs/spectrum/teensy_fft_adapter.h tests/test_spectrum_fft.cpp CMakeLists.txt
git -C /Users/moolet/Development/github/newdigate/audio-codecs commit -m "feat(spectrum): implement FFT backend abstraction, desktop real FFT, and teensy adapter"
```

---

### Task 3: Perceptual Filterbank & Quantization Engine (`spectrum_filterbank.h`, `spectrum_filterbank.cpp`)

**Files:**
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/spectrum/spectrum_filterbank.h`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/src/spectrum/spectrum_filterbank.cpp`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_spectrum_filterbank.cpp`
- Modify: `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`

**Interfaces:**
- Consumes: `spectrum_types.h`, `<cstdint>`, `<cmath>`
- Produces:
  - `class SpectrumFilterbank`
  - `bool init(uint32_t sample_rate = 44100, uint8_t num_bands = 64, uint16_t min_freq = 20, uint16_t max_freq = 20000)`
  - `void apply_window(const int16_t* in_pcm, int16_t* out_windowed_pcm)`
  - `void compute_bands(const float* in_magnitudes_512, uint8_t* out_bands_64)`

- [ ] **Step 1: Write the failing test**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_spectrum_filterbank.cpp`:
```cpp
#include "audio_codecs/spectrum/spectrum_filterbank.h"
#include <cassert>
#include <iostream>

using namespace audio_codecs::spectrum;

int main() {
    SpectrumFilterbank fb;
    assert(fb.init(44100, 64, 20, 20000));

    // Zero magnitudes produce zero dB band energy
    float zero_mags[512] = {0};
    uint8_t zero_bands[64] = {0};
    fb.compute_bands(zero_mags, zero_bands);
    for (size_t b = 0; b < 64; ++b) {
        assert(zero_bands[b] == 0);
    }

    // High energy in bin 10 (~440 Hz) produces peak around lower-mid band (~band 20-30)
    float test_mags[512] = {0};
    test_mags[10] = 30000.0f;
    uint8_t test_bands[64] = {0};
    fb.compute_bands(test_mags, test_bands);

    size_t peak_band = 0;
    uint8_t peak_val = 0;
    for (size_t b = 0; b < 64; ++b) {
        if (test_bands[b] > peak_val) {
            peak_val = test_bands[b];
            peak_band = b;
        }
    }
    assert(peak_val > 150);
    assert(peak_band >= 20 && peak_band <= 35);

    std::cout << "test_spectrum_filterbank PASSED (peak_band=" << peak_band << ", val=" << (int)peak_val << ")\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_spectrum_filterbank
```
Expected: FAIL (missing `spectrum_filterbank.h`).

- [ ] **Step 3: Write minimal implementation**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/spectrum/spectrum_filterbank.h`:
```cpp
#pragma once
#include "spectrum_types.h"
#include <cstdint>
#include <cstddef>

namespace audio_codecs::spectrum {

class SpectrumFilterbank {
public:
    SpectrumFilterbank();

    bool init(uint32_t sample_rate = 44100,
              uint8_t num_bands = DEFAULT_NUM_BANDS,
              uint16_t min_freq = 20,
              uint16_t max_freq = 20000);

    void apply_hann_window(const int16_t* in_pcm, int16_t* out_windowed_pcm) const;
    void compute_bands(const float* in_magnitudes_512, uint8_t* out_bands) const;

    uint8_t num_bands() const { return num_bands_; }

private:
    uint8_t num_bands_{DEFAULT_NUM_BANDS};
    uint16_t band_bin_start_[64]{};
    uint16_t band_bin_end_[64]{};
    int16_t hann_window_[1024]{};
    bool initialized_{false};
};

} // namespace audio_codecs::spectrum
```

Create `/Users/moolet/Development/github/newdigate/audio-codecs/src/spectrum/spectrum_filterbank.cpp`:
```cpp
#include "audio_codecs/spectrum/spectrum_filterbank.h"
#include <cmath>
#include <algorithm>

namespace audio_codecs::spectrum {

SpectrumFilterbank::SpectrumFilterbank() {
    init();
}

bool SpectrumFilterbank::init(uint32_t sample_rate, uint8_t num_bands, uint16_t min_freq, uint16_t max_freq) {
    if (num_bands > 64) num_bands = 64;
    num_bands_ = num_bands;

    constexpr float kPi = 3.14159265358979323846f;
    for (size_t i = 0; i < 1024; ++i) {
        float w = 0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(i) / 1023.0f));
        hann_window_[i] = static_cast<int16_t>(w * 32767.0f);
    }

    float bin_width = static_cast<float>(sample_rate) / 1024.0f;
    float f_min = static_cast<float>(min_freq);
    float f_max = static_cast<float>(max_freq);

    for (size_t b = 0; b < num_bands_; ++b) {
        float f_low = f_min * std::pow(f_max / f_min, static_cast<float>(b) / static_cast<float>(num_bands_));
        float f_high = f_min * std::pow(f_max / f_min, static_cast<float>(b + 1) / static_cast<float>(num_bands_));

        uint16_t k_start = static_cast<uint16_t>(std::max(1.0f, std::floor(f_low / bin_width)));
        uint16_t k_end = static_cast<uint16_t>(std::min(511.0f, std::ceil(f_high / bin_width)));

        if (k_end < k_start) k_end = k_start;

        band_bin_start_[b] = k_start;
        band_bin_end_[b] = k_end;
    }

    initialized_ = true;
    return true;
}

void SpectrumFilterbank::apply_hann_window(const int16_t* in_pcm, int16_t* out_windowed_pcm) const {
    for (size_t i = 0; i < 1024; ++i) {
        int32_t val = (static_cast<int32_t>(in_pcm[i]) * static_cast<int32_t>(hann_window_[i])) >> 15;
        out_windowed_pcm[i] = static_cast<int16_t>(val);
    }
}

void SpectrumFilterbank::compute_bands(const float* in_magnitudes_512, uint8_t* out_bands) const {
    for (size_t b = 0; b < num_bands_; ++b) {
        uint16_t start = band_bin_start_[b];
        uint16_t end = band_bin_end_[b];

        float sum_sq = 0.0f;
        for (uint16_t k = start; k <= end; ++k) {
            float mag = in_magnitudes_512[k];
            sum_sq += mag * mag;
        }

        // dB calculation with -96 dB floor
        float energy = sum_sq + 1e-10f;
        float db = 10.0f * std::log10(energy);

        // Normalize dB from [-96, 0] to [0, 255]
        float scaled = (db + 96.0f) * (255.0f / 96.0f);
        if (scaled < 0.0f) scaled = 0.0f;
        if (scaled > 255.0f) scaled = 255.0f;

        out_bands[b] = static_cast<uint8_t>(scaled);
    }
}

} // namespace audio_codecs::spectrum
```

Update `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`:
Add `src/spectrum/spectrum_filterbank.cpp` to `audio_codecs_spectrum`, and add `test_spectrum_filterbank`:
```cmake
add_executable(test_spectrum_filterbank tests/test_spectrum_filterbank.cpp)
target_link_libraries(test_spectrum_filterbank PRIVATE audio_codecs_spectrum)
add_test(NAME SpectrumFilterbankTest COMMAND test_spectrum_filterbank)
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cmake -B /Users/moolet/Development/github/newdigate/audio-codecs/build -S /Users/moolet/Development/github/newdigate/audio-codecs && cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_spectrum_filterbank && ctest --test-dir /Users/moolet/Development/github/newdigate/audio-codecs/build -R SpectrumFilterbankTest --output-on-failure
```
Expected: PASS (`test_spectrum_filterbank PASSED`).

- [ ] **Step 5: Commit**

```bash
git -C /Users/moolet/Development/github/newdigate/audio-codecs add include/audio_codecs/spectrum/spectrum_filterbank.h src/spectrum/spectrum_filterbank.cpp tests/test_spectrum_filterbank.cpp CMakeLists.txt
git -C /Users/moolet/Development/github/newdigate/audio-codecs commit -m "feat(spectrum): implement log-frequency filterbank and dB energy quantization"
```

---

### Task 4: Spectrum Generator Engine (`spectrum_generator.h`, `spectrum_generator.cpp`)

**Files:**
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/spectrum/spectrum_generator.h`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/src/spectrum/spectrum_generator.cpp`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_spectrum_generator.cpp`
- Modify: `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`

**Interfaces:**
- Consumes: `spectrum_types.h`, `fft_backend.h`, `spectrum_filterbank.h`, `preview/preview_stream.h`
- Produces:
  - `enum class GeneratorStatus`
  - `class SpectrumGenerator`
  - `bool init(...)`
  - `GeneratorStatus step(size_t frame_budget)`
  - `bool generate_all()`

- [ ] **Step 1: Write the failing test**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_spectrum_generator.cpp`:
```cpp
#include "audio_codecs/spectrum/spectrum_generator.h"
#include "audio_codecs/spectrum/desktop_fft.h"
#include "audio_codecs/preview/preview_stream.h"
#include <cassert>
#include <vector>
#include <cmath>
#include <iostream>

using namespace audio_codecs::spectrum;
using namespace audio_codecs::preview;

int main() {
    DesktopRealFftBackend fft;
    assert(fft.init());

    // Generate 2 seconds of 44.1 kHz stereo audio with 440 Hz sine wave
    uint32_t sample_rate = 44100;
    size_t num_frames = sample_rate * 2;
    std::vector<int16_t> pcm(num_frames * 2);
    for (size_t f = 0; f < num_frames; ++f) {
        float t = static_cast<float>(f) / sample_rate;
        int16_t val = static_cast<int16_t>(std::sin(2.0f * 3.14159265f * 440.0f * t) * 20000.0f);
        pcm[f * 2] = val;
        pcm[f * 2 + 1] = val;
    }

    MemoryReader pcm_reader(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * sizeof(int16_t));
    std::vector<uint8_t> asv_storage(1024 * 128, 0);
    MemoryWriter asv_writer(asv_storage.data(), asv_storage.size());

    SpectrumGenerator gen;
    assert(gen.init(pcm_reader, asv_writer, fft, sample_rate, 2, true, 64));
    assert(gen.generate_all());

    assert(gen.status() == GeneratorStatus::Complete);
    assert(gen.progress() >= 1.0f);

    // Validate generated header
    const AsvHeader* hdr = reinterpret_cast<const AsvHeader*>(asv_storage.data());
    assert(hdr->magic == ASV_MAGIC);
    assert(hdr->version == 1);
    assert(hdr->channels == 1);
    assert(hdr->num_bands == 64);
    assert(hdr->duration_ms == 2000);
    assert(hdr->lod_count == 2);
    assert(hdr->lods[0].frame_count > 0);
    assert(hdr->lods[1].frame_count > 0);
    assert(hdr->lods[1].downsample_ratio == 16);

    std::cout << "test_spectrum_generator PASSED (LOD0=" << hdr->lods[0].frame_count
              << ", LOD1=" << hdr->lods[1].frame_count << ")\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_spectrum_generator
```
Expected: FAIL (missing `spectrum_generator.h`).

- [ ] **Step 3: Write minimal implementation**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/spectrum/spectrum_generator.h`:
```cpp
#pragma once
#include "spectrum_types.h"
#include "fft_backend.h"
#include "spectrum_filterbank.h"
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

    GeneratorStatus step(size_t frame_budget = 32);
    bool generate_all();

    float progress() const;
    GeneratorStatus status() const { return status_; }

private:
    preview::SeekableReader* source_{nullptr};
    preview::SeekableWriter* dest_{nullptr};
    FftBackend* backend_{nullptr};
    SpectrumFilterbank filterbank_;

    uint32_t sample_rate_{44100};
    uint8_t source_channels_{2};
    bool downmix_to_mono_{true};
    uint8_t num_bands_{DEFAULT_NUM_BANDS};

    GeneratorStatus status_{GeneratorStatus::Ready};

    int16_t pcm_window_[1024]{};
    int16_t windowed_pcm_[1024]{};
    float magnitudes_[512]{};
    uint8_t frame_bands_[64]{};

    uint64_t total_pcm_frames_read_{0};
    uint32_t lod0_frame_count_{0};
    uint32_t lod1_frame_count_{0};

    uint64_t lod0_offset_{128};
    uint64_t lod1_offset_{0};

    uint32_t lod1_current_group_{0};
};

} // namespace audio_codecs::spectrum
```

Create `/Users/moolet/Development/github/newdigate/audio-codecs/src/spectrum/spectrum_generator.cpp`:
```cpp
#include "audio_codecs/spectrum/spectrum_generator.h"
#include <cstring>
#include <algorithm>

namespace audio_codecs::spectrum {

SpectrumGenerator::SpectrumGenerator() = default;

bool SpectrumGenerator::init(preview::SeekableReader& pcm_source,
                             preview::SeekableWriter& spectrum_dest,
                             FftBackend& fft_backend,
                             uint32_t sample_rate,
                             uint8_t source_channels,
                             bool downmix_to_mono,
                             uint8_t num_bands) {
    source_ = &pcm_source;
    dest_ = &spectrum_dest;
    backend_ = &fft_backend;
    sample_rate_ = sample_rate;
    source_channels_ = source_channels;
    downmix_to_mono_ = downmix_to_mono;
    num_bands_ = (num_bands > 64) ? 64 : num_bands;

    if (!filterbank_.init(sample_rate_, num_bands_)) {
        status_ = GeneratorStatus::ErrorBackend;
        return false;
    }

    // Reserve 128 bytes for header
    AsvHeader blank_hdr{};
    if (!dest_->seek(0) || dest_->write(reinterpret_cast<const uint8_t*>(&blank_hdr), sizeof(blank_hdr)) != sizeof(blank_hdr)) {
        status_ = GeneratorStatus::ErrorDest;
        return false;
    }

    lod0_offset_ = 128;
    lod0_frame_count_ = 0;
    lod1_frame_count_ = 0;
    total_pcm_frames_read_ = 0;
    lod1_current_group_ = 0;
    std::memset(pcm_window_, 0, sizeof(pcm_window_));

    status_ = GeneratorStatus::ProcessingLOD0;
    return true;
}

GeneratorStatus SpectrumGenerator::step(size_t frame_budget) {
    if (status_ == GeneratorStatus::ProcessingLOD0) {
        size_t frames_processed = 0;
        while (frames_processed < frame_budget) {
            // Shift overlap window by 512 samples
            std::memmove(pcm_window_, pcm_window_ + 512, 512 * sizeof(int16_t));

            // Read next 512 samples
            int16_t raw_in[512 * 2];
            size_t bytes_to_read = 512 * source_channels_ * sizeof(int16_t);
            size_t bytes_read = source_->read(reinterpret_cast<uint8_t*>(raw_in), bytes_to_read);
            size_t frames_read = bytes_read / (source_channels_ * sizeof(int16_t));

            if (frames_read == 0) {
                // LOD 0 complete
                lod1_offset_ = dest_->position();
                status_ = GeneratorStatus::ProcessingLOD1;
                break;
            }

            // Downmix to mono and place in upper half
            for (size_t i = 0; i < frames_read; ++i) {
                if (source_channels_ == 1) {
                    pcm_window_[512 + i] = raw_in[i];
                } else if (downmix_to_mono_) {
                    int32_t sum = static_cast<int32_t>(raw_in[i * 2]) + static_cast<int32_t>(raw_in[i * 2 + 1]);
                    pcm_window_[512 + i] = static_cast<int16_t>(sum / 2);
                } else {
                    pcm_window_[512 + i] = raw_in[i * 2];
                }
            }
            for (size_t i = frames_read; i < 512; ++i) {
                pcm_window_[512 + i] = 0;
            }

            total_pcm_frames_read_ += frames_read;

            // Apply window and FFT
            filterbank_.apply_hann_window(pcm_window_, windowed_pcm_);
            backend_->forward_1024(windowed_pcm_, magnitudes_);
            filterbank_.compute_bands(magnitudes_, frame_bands_);

            if (dest_->write(frame_bands_, num_bands_) != num_bands_) {
                status_ = GeneratorStatus::ErrorDest;
                return status_;
            }

            lod0_frame_count_++;
            frames_processed++;
        }
        return status_;
    }

    if (status_ == GeneratorStatus::ProcessingLOD1) {
        auto* reader = dynamic_cast<preview::SeekableReader*>(dest_);
        if (!reader) {
            status_ = GeneratorStatus::ErrorDest;
            return status_;
        }

        uint32_t total_groups = (lod0_frame_count_ + 15) / 16;
        size_t groups_processed = 0;

        while (lod1_current_group_ < total_groups && groups_processed < frame_budget) {
            uint32_t start_idx = lod1_current_group_ * 16;
            uint32_t count = std::min(16u, lod0_frame_count_ - start_idx);

            uint8_t group_bands[64] = {0};

            for (uint32_t i = 0; i < count; ++i) {
                uint64_t offset = lod0_offset_ + static_cast<uint64_t>(start_idx + i) * num_bands_;
                if (!reader->seek(offset)) {
                    status_ = GeneratorStatus::ErrorDest;
                    return status_;
                }

                uint8_t src_bands[64];
                if (reader->read(src_bands, num_bands_) != num_bands_) {
                    status_ = GeneratorStatus::ErrorDest;
                    return status_;
                }

                for (size_t b = 0; b < num_bands_; ++b) {
                    group_bands[b] = std::max(group_bands[b], src_bands[b]);
                }
            }

            // Write LOD 1 frame to end of dest
            uint64_t write_pos = lod1_offset_ + static_cast<uint64_t>(lod1_current_group_) * num_bands_;
            if (!dest_->seek(write_pos) || dest_->write(group_bands, num_bands_) != num_bands_) {
                status_ = GeneratorStatus::ErrorDest;
                return status_;
            }

            lod1_current_group_++;
            lod1_frame_count_++;
            groups_processed++;
        }

        if (lod1_current_group_ >= total_groups) {
            status_ = GeneratorStatus::Finalizing;
        }
        return status_;
    }

    if (status_ == GeneratorStatus::Finalizing) {
        AsvHeader hdr{};
        hdr.magic = ASV_MAGIC;
        hdr.version = ASV_VERSION;
        hdr.flags = ASV_FLAG_HAS_LODS;
        hdr.sample_rate = sample_rate_;
        hdr.channels = (downmix_to_mono_ || source_channels_ == 1) ? 1 : 2;
        hdr.num_bands = num_bands_;
        hdr.fft_size = DEFAULT_FFT_SIZE;
        hdr.hop_size = DEFAULT_HOP_SIZE;
        hdr.min_freq_hz = 20;
        hdr.max_freq_hz = 20000;
        hdr.lod_count = 2;
        hdr.total_pcm_frames = total_pcm_frames_read_;
        hdr.duration_ms = (sample_rate_ > 0) ? static_cast<uint32_t>((total_pcm_frames_read_ * 1000ULL) / sample_rate_) : 0;

        hdr.lods[0].downsample_ratio = 1;
        hdr.lods[0].frame_count = lod0_frame_count_;
        hdr.lods[0].file_offset = lod0_offset_;

        hdr.lods[1].downsample_ratio = 16;
        hdr.lods[1].frame_count = lod1_frame_count_;
        hdr.lods[1].file_offset = lod1_offset_;

        if (!dest_->seek(0) || dest_->write(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr)) != sizeof(hdr)) {
            status_ = GeneratorStatus::ErrorDest;
            return status_;
        }

        status_ = GeneratorStatus::Complete;
    }

    return status_;
}

bool SpectrumGenerator::generate_all() {
    while (status_ != GeneratorStatus::Complete &&
           status_ != GeneratorStatus::ErrorSource &&
           status_ != GeneratorStatus::ErrorDest &&
           status_ != GeneratorStatus::ErrorBackend) {
        step(64);
    }
    return status_ == GeneratorStatus::Complete;
}

float SpectrumGenerator::progress() const {
    if (status_ == GeneratorStatus::Complete) return 1.0f;
    if (status_ == GeneratorStatus::ProcessingLOD0) {
        if (source_ && source_->size() > 0) {
            return 0.8f * (static_cast<float>(source_->position()) / static_cast<float>(source_->size()));
        }
        return 0.1f;
    }
    if (status_ == GeneratorStatus::ProcessingLOD1) {
        uint32_t total_groups = (lod0_frame_count_ + 15) / 16;
        if (total_groups == 0) return 0.8f;
        return 0.8f + 0.2f * (static_cast<float>(lod1_current_group_) / static_cast<float>(total_groups));
    }
    return 1.0f;
}

} // namespace audio_codecs::spectrum
```

Update `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`:
Add `src/spectrum/spectrum_generator.cpp` to `audio_codecs_spectrum` and add `test_spectrum_generator`:
```cmake
add_executable(test_spectrum_generator tests/test_spectrum_generator.cpp)
target_link_libraries(test_spectrum_generator PRIVATE audio_codecs_spectrum audio_codecs_preview)
add_test(NAME SpectrumGeneratorTest COMMAND test_spectrum_generator)
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cmake -B /Users/moolet/Development/github/newdigate/audio-codecs/build -S /Users/moolet/Development/github/newdigate/audio-codecs && cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_spectrum_generator && ctest --test-dir /Users/moolet/Development/github/newdigate/audio-codecs/build -R SpectrumGeneratorTest --output-on-failure
```
Expected: PASS (`test_spectrum_generator PASSED`).

- [ ] **Step 5: Commit**

```bash
git -C /Users/moolet/Development/github/newdigate/audio-codecs add include/audio_codecs/spectrum/spectrum_generator.h src/spectrum/spectrum_generator.cpp tests/test_spectrum_generator.cpp CMakeLists.txt
git -C /Users/moolet/Development/github/newdigate/audio-codecs commit -m "feat(spectrum): implement spectrum generator with zero-RAM multi-LOD pyramid"
```

---

### Task 5: Spectrum Reader Engine (`spectrum_reader.h`, `spectrum_reader.cpp`)

**Files:**
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/spectrum/spectrum_reader.h`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/src/spectrum/spectrum_reader.cpp`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_spectrum_reader.cpp`
- Modify: `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`

**Interfaces:**
- Consumes: `spectrum_types.h`, `preview/preview_stream.h`
- Produces:
  - `class SpectrumReader`
  - `bool init(preview::SeekableReader& spectrum_source)`
  - `size_t read_spectrum(uint32_t start_ms, uint32_t duration_ms, uint8_t* out_bands, size_t max_frames)`

- [ ] **Step 1: Write the failing test**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_spectrum_reader.cpp`:
```cpp
#include "audio_codecs/spectrum/spectrum_reader.h"
#include "audio_codecs/spectrum/spectrum_generator.h"
#include "audio_codecs/spectrum/desktop_fft.h"
#include "audio_codecs/preview/preview_stream.h"
#include <cassert>
#include <vector>
#include <cmath>
#include <iostream>

using namespace audio_codecs::spectrum;
using namespace audio_codecs::preview;

int main() {
    DesktopRealFftBackend fft;
    assert(fft.init());

    // Generate 3 seconds of 44.1 kHz sine wave
    uint32_t sample_rate = 44100;
    size_t num_frames = sample_rate * 3;
    std::vector<int16_t> pcm(num_frames);
    for (size_t f = 0; f < num_frames; ++f) {
        float t = static_cast<float>(f) / sample_rate;
        pcm[f] = static_cast<int16_t>(std::sin(2.0f * 3.14159265f * 1000.0f * t) * 20000.0f);
    }

    MemoryReader pcm_reader(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * sizeof(int16_t));
    std::vector<uint8_t> asv_storage(1024 * 128, 0);
    MemoryWriter asv_writer(asv_storage.data(), asv_storage.size());

    SpectrumGenerator gen;
    assert(gen.init(pcm_reader, asv_writer, fft, sample_rate, 1, true, 64));
    assert(gen.generate_all());

    MemoryReader asv_reader(asv_storage.data(), asv_writer.size());
    SpectrumReader reader;
    assert(reader.init(asv_reader));

    assert(reader.duration_ms() == 3000);
    assert(reader.channels() == 1);
    assert(reader.num_bands() == 64);

    // Full-track overview query (should select LOD 1)
    std::vector<uint8_t> overview(10 * 64);
    size_t read_count = reader.read_spectrum(0, 3000, overview.data(), 10);
    assert(read_count == 10);

    // Zoomed-in query (should select LOD 0)
    std::vector<uint8_t> zoom(5 * 64);
    read_count = reader.read_spectrum(500, 100, zoom.data(), 5);
    assert(read_count == 5);

    std::cout << "test_spectrum_reader PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_spectrum_reader
```
Expected: FAIL (missing `spectrum_reader.h`).

- [ ] **Step 3: Write minimal implementation**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/spectrum/spectrum_reader.h`:
```cpp
#pragma once
#include "spectrum_types.h"
#include "audio_codecs/preview/preview_stream.h"
#include <cstdint>
#include <cstddef>

namespace audio_codecs::spectrum {

class SpectrumReader {
public:
    SpectrumReader();

    bool init(preview::SeekableReader& spectrum_source);

    uint32_t duration_ms() const { return header_.duration_ms; }
    uint32_t sample_rate() const { return header_.sample_rate; }
    uint8_t  channels() const { return header_.channels; }
    uint8_t  num_bands() const { return header_.num_bands; }
    uint16_t hop_size() const { return header_.hop_size; }
    uint16_t lod_count() const { return header_.lod_count; }

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
    bool read_frame(uint8_t lod_idx, uint32_t frame_idx, uint8_t* out_frame);
};

} // namespace audio_codecs::spectrum
```

Create `/Users/moolet/Development/github/newdigate/audio-codecs/src/spectrum/spectrum_reader.cpp`:
```cpp
#include "audio_codecs/spectrum/spectrum_reader.h"
#include <cstring>
#include <algorithm>

namespace audio_codecs::spectrum {

SpectrumReader::SpectrumReader() = default;

bool SpectrumReader::init(preview::SeekableReader& spectrum_source) {
    source_ = &spectrum_source;
    cached_sector_idx_ = UINT64_MAX;

    if (!source_->seek(0)) return false;
    if (source_->read(reinterpret_cast<uint8_t*>(&header_), sizeof(header_)) != sizeof(header_)) {
        return false;
    }

    if (header_.magic != ASV_MAGIC || header_.version != ASV_VERSION) {
        return false;
    }

    return true;
}

uint8_t SpectrumReader::select_lod(uint32_t duration_ms, size_t target_frames) const {
    if (header_.lod_count < 2 || target_frames == 0) return 0;
    float time_per_pixel = static_cast<float>(duration_ms) / static_cast<float>(target_frames);
    if (time_per_pixel >= 90.0f) {
        return 1;
    }
    return 0;
}

bool SpectrumReader::read_frame(uint8_t lod_idx, uint32_t frame_idx, uint8_t* out_frame) {
    if (!source_ || lod_idx >= header_.lod_count) return false;
    const auto& lod = header_.lods[lod_idx];
    if (frame_idx >= lod.frame_count) return false;

    uint64_t frame_bytes = header_.num_bands;
    uint64_t file_offset = lod.file_offset + static_cast<uint64_t>(frame_idx) * frame_bytes;

    uint64_t sector_idx = file_offset / 512;
    uint64_t sector_offset = file_offset % 512;

    if (sector_idx != cached_sector_idx_) {
        if (!source_->seek(sector_idx * 512)) return false;
        size_t n = source_->read(sector_cache_, 512);
        if (n == 0) return false;
        cached_sector_idx_ = sector_idx;
    }

    if (sector_offset + frame_bytes <= 512) {
        std::memcpy(out_frame, sector_cache_ + sector_offset, frame_bytes);
        return true;
    }

    // Straddles two sectors
    size_t first_part = 512 - sector_offset;
    std::memcpy(out_frame, sector_cache_ + sector_offset, first_part);

    cached_sector_idx_ = sector_idx + 1;
    if (!source_->seek(cached_sector_idx_ * 512)) return false;
    source_->read(sector_cache_, 512);
    std::memcpy(out_frame + first_part, sector_cache_, frame_bytes - first_part);
    return true;
}

size_t SpectrumReader::read_spectrum(uint32_t start_ms,
                                     uint32_t duration_ms,
                                     uint8_t* out_bands,
                                     size_t max_frames) {
    if (!source_ || max_frames == 0 || header_.duration_ms == 0) return 0;
    if (start_ms >= header_.duration_ms) return 0;

    if (start_ms + duration_ms > header_.duration_ms) {
        duration_ms = header_.duration_ms - start_ms;
    }

    uint8_t lod_idx = select_lod(duration_ms, max_frames);
    const auto& lod = header_.lods[lod_idx];
    if (lod.frame_count == 0) return 0;

    float ms_per_frame = static_cast<float>(header_.duration_ms) / static_cast<float>(lod.frame_count);

    for (size_t out_idx = 0; out_idx < max_frames; ++out_idx) {
        float t_start = static_cast<float>(start_ms) + (static_cast<float>(out_idx) / static_cast<float>(max_frames)) * static_cast<float>(duration_ms);
        float t_end = static_cast<float>(start_ms) + (static_cast<float>(out_idx + 1) / static_cast<float>(max_frames)) * static_cast<float>(duration_ms);

        uint32_t f_start = static_cast<uint32_t>(t_start / ms_per_frame);
        uint32_t f_end = static_cast<uint32_t>(t_end / ms_per_frame);
        if (f_start >= lod.frame_count) f_start = lod.frame_count - 1;
        if (f_end >= lod.frame_count) f_end = lod.frame_count - 1;

        uint8_t* dest_col = out_bands + out_idx * header_.num_bands;
        std::memset(dest_col, 0, header_.num_bands);

        uint8_t temp[64];
        for (uint32_t f = f_start; f <= f_end; ++f) {
            if (read_frame(lod_idx, f, temp)) {
                for (size_t b = 0; b < header_.num_bands; ++b) {
                    dest_col[b] = std::max(dest_col[b], temp[b]);
                }
            }
        }
    }

    return max_frames;
}

} // namespace audio_codecs::spectrum
```

Update `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`:
Add `src/spectrum/spectrum_reader.cpp` to `audio_codecs_spectrum` and add `test_spectrum_reader`:
```cmake
add_executable(test_spectrum_reader tests/test_spectrum_reader.cpp)
target_link_libraries(test_spectrum_reader PRIVATE audio_codecs_spectrum audio_codecs_preview)
add_test(NAME SpectrumReaderTest COMMAND test_spectrum_reader)
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cmake -B /Users/moolet/Development/github/newdigate/audio-codecs/build -S /Users/moolet/Development/github/newdigate/audio-codecs && cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_spectrum_reader && ctest --test-dir /Users/moolet/Development/github/newdigate/audio-codecs/build -R SpectrumReaderTest --output-on-failure
```
Expected: PASS (`test_spectrum_reader PASSED`).

- [ ] **Step 5: Commit**

```bash
git -C /Users/moolet/Development/github/newdigate/audio-codecs add include/audio_codecs/spectrum/spectrum_reader.h src/spectrum/spectrum_reader.cpp tests/test_spectrum_reader.cpp CMakeLists.txt
git -C /Users/moolet/Development/github/newdigate/audio-codecs commit -m "feat(spectrum): implement spectrum reader with adaptive LOD selection and sector caching"
```

---

### Task 6: Umbrella Header, Codec Library Integration & End-to-End Test

**Files:**
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/spectrum.h`
- Modify: `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/audio_codecs.h`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_spectrum_integration.cpp`
- Modify: `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`

**Interfaces:**
- Consumes: All spectrum headers, `audio_codecs_wav`
- Produces:
  - Top-level `#include "audio_codecs/spectrum.h"`
  - Full end-to-end integration test validating decoded PCM to spectrum query pipeline

- [ ] **Step 1: Write the failing test**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_spectrum_integration.cpp`:
```cpp
#include "audio_codecs/audio_codecs.h"
#include <cassert>
#include <vector>
#include <cmath>
#include <iostream>

using namespace audio_codecs;
using namespace audio_codecs::spectrum;
using namespace audio_codecs::preview;

int main() {
    DesktopRealFftBackend fft;
    assert(fft.init());

    // Generate 4 seconds of audio:
    // First 2 seconds: 440 Hz tone (low-mid band)
    // Last 2 seconds: 4000 Hz tone (high band)
    uint32_t sample_rate = 44100;
    size_t num_frames = sample_rate * 4;
    std::vector<int16_t> pcm(num_frames * 2);

    for (size_t f = 0; f < num_frames; ++f) {
        float t = static_cast<float>(f) / sample_rate;
        float freq = (f < sample_rate * 2) ? 440.0f : 4000.0f;
        int16_t val = static_cast<int16_t>(std::sin(2.0f * 3.14159265f * freq * t) * 20000.0f);
        pcm[f * 2] = val;
        pcm[f * 2 + 1] = val;
    }

    MemoryReader pcm_reader(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * sizeof(int16_t));
    std::vector<uint8_t> asv_storage(1024 * 256, 0);
    MemoryWriter asv_writer(asv_storage.data(), asv_storage.size());

    SpectrumGenerator gen;
    assert(gen.init(pcm_reader, asv_writer, fft, sample_rate, 2, true, 64));
    assert(gen.generate_all());

    MemoryReader asv_reader(asv_storage.data(), asv_writer.size());
    SpectrumReader reader;
    assert(reader.init(asv_reader));
    assert(reader.duration_ms() == 4000);

    // Query first half (0 to 2000 ms): peak should be around band 20-35 (440 Hz)
    std::vector<uint8_t> part1(64);
    reader.read_spectrum(500, 50, part1.data(), 1);
    size_t peak1 = 0;
    for (size_t b = 1; b < 64; ++b) {
        if (part1[b] > part1[peak1]) peak1 = b;
    }
    assert(peak1 >= 20 && peak1 <= 35);

    // Query second half (2000 to 4000 ms): peak should be around band 45-55 (4000 Hz)
    std::vector<uint8_t> part2(64);
    reader.read_spectrum(2500, 50, part2.data(), 1);
    size_t peak2 = 0;
    for (size_t b = 1; b < 64; ++b) {
        if (part2[b] > part2[peak2]) peak2 = b;
    }
    assert(peak2 >= 45 && peak2 <= 55);

    std::cout << "test_spectrum_integration PASSED (part1_peak=" << peak1 << ", part2_peak=" << peak2 << ")\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_spectrum_integration
```
Expected: FAIL (missing `audio_codecs/spectrum.h`).

- [ ] **Step 3: Write minimal implementation**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/spectrum.h`:
```cpp
#pragma once
#include "audio_codecs/spectrum/spectrum_types.h"
#include "audio_codecs/spectrum/fft_backend.h"
#include "audio_codecs/spectrum/spectrum_filterbank.h"
#include "audio_codecs/spectrum/spectrum_generator.h"
#include "audio_codecs/spectrum/spectrum_reader.h"
#include "audio_codecs/spectrum/desktop_fft.h"
#include "audio_codecs/spectrum/teensy_fft_adapter.h"
```

Add `#include "audio_codecs/spectrum.h"` to `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/audio_codecs.h`.

Update `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`:
```cmake
add_executable(test_spectrum_integration tests/test_spectrum_integration.cpp)
target_link_libraries(test_spectrum_integration PRIVATE audio_codecs_spectrum audio_codecs_preview)
add_test(NAME SpectrumIntegrationTest COMMAND test_spectrum_integration)
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cmake -B /Users/moolet/Development/github/newdigate/audio-codecs/build -S /Users/moolet/Development/github/newdigate/audio-codecs && cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_spectrum_integration && ctest --test-dir /Users/moolet/Development/github/newdigate/audio-codecs/build --output-on-failure
```
Expected: PASS (100% tests passed including all spectrum tests).

- [ ] **Step 5: Commit**

```bash
git -C /Users/moolet/Development/github/newdigate/audio-codecs add include/audio_codecs/spectrum.h include/audio_codecs/audio_codecs.h tests/test_spectrum_integration.cpp CMakeLists.txt
git -C /Users/moolet/Development/github/newdigate/audio-codecs commit -m "feat(spectrum): expose umbrella spectrum.h and add end-to-end integration test"
```
