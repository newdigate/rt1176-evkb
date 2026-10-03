# Audio Tempo Engine & ATT File Format Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a cross-platform audio tempo detection engine, beat grid tracker, and binary tempo timeline format (`.att` / **ATT1**) in `audio-codecs` that consumes pre-computed 64-band `.asv` spectrum data, operates with zero dynamic heap allocations, and runs cooperatively on both desktop and ARM Cortex-M7 microcontrollers.

**Architecture:** The tempo engine is split into focused components: binary structs (`tempo_types.h`), sub-band spectral flux extraction (`tempo_novelty.h/.cpp`), comb resonator tempo induction (`tempo_inducer.h/.cpp`), sliced cooperative generator (`tempo_generator.h/.cpp`), random-access reader (`tempo_reader.h/.cpp`), and top-level umbrella exports (`tempo.h`). It streams `.asv` spectrum frames in 128-frame slices and writes sector-aligned `.att` files using `preview::SeekableReader` and `preview::SeekableWriter`.

**Tech Stack:** C++17, CMake, standard C/C++ math library, `audio_codecs::spectrum`, `audio_codecs::preview` streams.

**Spec:** `docs/superpowers/specs/2026-10-03-audio-tempo-engine-design.md`

## Global Constraints
- Pure C++17; no dynamic heap allocations in steady-state analysis or query paths.
- No Arduino or platform-specific headers inside `include/audio_codecs/`.
- `sizeof(AttHeader)` must be exactly 128 bytes (asserted via compile-time `static_assert`).
- `sizeof(AttTempoPoint)` must be exactly 8 bytes (asserted via compile-time `static_assert`).
- `sizeof(AttBeatMarker)` must be exactly 16 bytes (asserted via compile-time `static_assert`).
- All integers stored in Little-Endian byte order.
- Analysis consumes `.asv` LOD 0 (64 log-spaced bands, 512-sample hop).
- All native desktop and embedded code is 100% MIT-licensed.
- Tests must execute correctly without side-effects inside `assert(...)` (clean under `-DNDEBUG`).

---

### Task 1: Core Format Structs and Header Serialization (tempo_types.h)

**Files:**
- Create: `include/audio_codecs/tempo/tempo_types.h`
- Test: `tests/test_tempo_types.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Standard integer types (`<cstdint>`, `<cstddef>`).
- Produces: `AttHeader`, `AttTempoPoint`, `AttBeatMarker`, `AttBeatFlags`, `ATT_MAGIC`, `ATT_VERSION`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_tempo_types.cpp`:
```cpp
#include <audio_codecs/tempo/tempo_types.h>
#include <audio_codecs/preview/preview_stream.h>
#include <cassert>
#include <cstring>
#include <iostream>

using namespace audio_codecs::tempo;
using namespace audio_codecs::preview;

void test_sizes_and_alignments() {
    static_assert(sizeof(AttHeader) == 128, "AttHeader size must be 128 bytes");
    static_assert(sizeof(AttTempoPoint) == 8, "AttTempoPoint size must be 8 bytes");
    static_assert(sizeof(AttBeatMarker) == 16, "AttBeatMarker size must be 16 bytes");

    assert(sizeof(AttHeader) == 128);
    assert(sizeof(AttTempoPoint) == 8);
    assert(sizeof(AttBeatMarker) == 16);
}

void test_header_defaults() {
    AttHeader hdr{};
    hdr.magic[0] = 'A';
    hdr.magic[1] = 'T';
    hdr.magic[2] = 'T';
    hdr.magic[3] = '1';
    hdr.version = ATT_VERSION;
    hdr.header_size = 128;
    hdr.duration_ms = 180000;
    hdr.sample_rate = 44100;
    hdr.global_bpm_q16 = (120 << 16);
    hdr.confidence = 240;
    hdr.time_signature_num = 4;
    hdr.time_signature_denom = 4;
    hdr.flags = ATT_FLAG_CONSTANT_TEMPO | ATT_FLAG_HAS_DOWNBEATS;
    hdr.first_beat_ms = 500;
    hdr.first_downbeat_ms = 500;
    hdr.total_beats = 360;
    hdr.total_bars = 90;
    hdr.tempo_curve_offset = 128;
    hdr.tempo_curve_count = 1;
    hdr.tempo_point_size = sizeof(AttTempoPoint);
    hdr.beat_grid_offset = 128 + sizeof(AttTempoPoint);
    hdr.beat_grid_count = 360;
    hdr.beat_marker_size = sizeof(AttBeatMarker);

    uint8_t buffer[128];
    MemoryWriter writer(buffer, sizeof(buffer));
    size_t written = writer.write(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr));
    assert(written == 128);

    AttHeader read_hdr{};
    MemoryReader reader(buffer, sizeof(buffer));
    size_t read_bytes = reader.read(reinterpret_cast<uint8_t*>(&read_hdr), sizeof(read_hdr));
    assert(read_bytes == 128);

    assert(std::memcmp(read_hdr.magic, "ATT1", 4) == 0);
    assert(read_hdr.version == ATT_VERSION);
    assert(read_hdr.duration_ms == 180000);
    assert(read_hdr.global_bpm_q16 == (120 << 16));
    assert(read_hdr.total_beats == 360);
    assert(read_hdr.total_bars == 90);
}

int main() {
    test_sizes_and_alignments();
    test_header_defaults();
    std::cout << "test_tempo_types PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Add test target to CMakeLists.txt and verify it fails**

In `CMakeLists.txt`, add test target:
```cmake
add_executable(test_tempo_types tests/test_tempo_types.cpp)
target_include_directories(test_tempo_types PRIVATE include)
target_link_libraries(test_tempo_types PRIVATE audio_codecs_preview)
add_test(NAME TempoTypesTest COMMAND test_tempo_types)
```
Run:
`cmake -B build -S . && cmake --build build --target test_tempo_types`
Expected: FAIL with `#include <audio_codecs/tempo/tempo_types.h> file not found`.

- [ ] **Step 3: Implement tempo_types.h**

Create `include/audio_codecs/tempo/tempo_types.h`:
```cpp
#pragma once

#include <cstdint>
#include <cstddef>

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#error "Big-endian architectures require byte swapping for ATT1 file format"
#endif

namespace audio_codecs::tempo {

constexpr uint8_t ATT_MAGIC[4] = {'A', 'T', 'T', '1'};
constexpr uint16_t ATT_VERSION = 1;
constexpr uint16_t ATT_HEADER_SIZE = 128;

enum AttHeaderFlags : uint8_t {
    ATT_FLAG_CONSTANT_TEMPO = 0x01,
    ATT_FLAG_HAS_DOWNBEATS  = 0x02,
    ATT_FLAG_USER_MODIFIED  = 0x04
};

enum AttBeatFlags : uint16_t {
    ATT_BEAT_FLAG_DOWNBEAT     = 0x0001,
    ATT_BEAT_FLAG_INTERPOLATED = 0x0002,
    ATT_BEAT_FLAG_USER_EDITED  = 0x0004,
    ATT_BEAT_FLAG_LOW_CONF     = 0x0008
};

#pragma pack(push, 1)

struct AttTempoPoint {
    uint32_t time_ms;
    uint32_t bpm_q16;
};
static_assert(sizeof(AttTempoPoint) == 8, "AttTempoPoint must be 8 bytes");

struct AttBeatMarker {
    uint32_t time_ms;
    uint32_t bar_index;
    uint16_t beat_within_bar;
    uint16_t flags;
    uint32_t local_bpm_q16;
};
static_assert(sizeof(AttBeatMarker) == 16, "AttBeatMarker must be 16 bytes");

struct AttHeader {
    uint8_t  magic[4];
    uint16_t version;
    uint16_t header_size;
    uint32_t duration_ms;
    uint32_t sample_rate;
    uint32_t global_bpm_q16;
    uint8_t  confidence;
    uint8_t  time_signature_num;
    uint8_t  time_signature_denom;
    uint8_t  flags;
    uint32_t first_beat_ms;
    uint32_t first_downbeat_ms;
    uint32_t total_beats;
    uint32_t total_bars;

    uint64_t tempo_curve_offset;
    uint32_t tempo_curve_count;
    uint16_t tempo_point_size;
    uint16_t reserved0;

    uint64_t beat_grid_offset;
    uint32_t beat_grid_count;
    uint16_t beat_marker_size;
    uint16_t reserved1;

    uint8_t  reserved[60];
};
static_assert(sizeof(AttHeader) == 128, "AttHeader must be exactly 128 bytes");

#pragma pack(pop)

} // namespace audio_codecs::tempo
```

- [ ] **Step 4: Build and run test to verify it passes**

Run:
`cmake --build build --target test_tempo_types && ctest --test-dir build -R TempoTypesTest --output-on-failure`
Expected: `test_tempo_types PASSED` (100% passed).

- [ ] **Step 5: Commit**

```bash
git add include/audio_codecs/tempo/tempo_types.h tests/test_tempo_types.cpp CMakeLists.txt
git commit -m "feat(tempo): implement core tempo data structures and format header"
```

---

### Task 2: Sub-Band Onset Detection & Novelty Extractor (tempo_novelty.h/.cpp)

**Files:**
- Create: `include/audio_codecs/tempo/tempo_novelty.h`
- Create: `src/tempo/tempo_novelty.cpp`
- Test: `tests/test_tempo_novelty.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: 64-band spectrum frame values (`const uint8_t bands[64]`).
- Produces: `TempoNoveltyExtractor` calculating sub-band flux ($O_{bass}, O_{snare}, O_{high}$), weighted master novelty, and local adaptive thresholding.

- [ ] **Step 1: Write the failing test**

Create `tests/test_tempo_novelty.cpp`:
```cpp
#include <audio_codecs/tempo/tempo_novelty.h>
#include <cassert>
#include <iostream>
#include <vector>

using namespace audio_codecs::tempo;

void test_subband_flux_separation() {
    TempoNoveltyExtractor extractor;
    extractor.reset();

    uint8_t frame0[64] = {0};
    NoveltySample s0 = extractor.process_frame(frame0);
    assert(s0.novelty == 0.0f);
    assert(s0.bass_flux == 0.0f);
    assert(s0.snare_flux == 0.0f);

    // Kick transient (energy burst in bands 2..5)
    uint8_t frame1[64] = {0};
    for (int b = 2; b <= 5; ++b) frame1[b] = 200;
    NoveltySample s1 = extractor.process_frame(frame1);
    assert(s1.bass_flux > 500.0f);
    assert(s1.snare_flux == 0.0f);
    assert(s1.novelty > 0.0f);

    // Snare transient (energy burst in bands 20..30)
    uint8_t frame2[64] = {0};
    for (int b = 20; b <= 30; ++b) frame2[b] = 180;
    NoveltySample s2 = extractor.process_frame(frame2);
    assert(s2.snare_flux > 500.0f);
}

void test_adaptive_threshold_suppresses_dc() {
    TempoNoveltyExtractor extractor;
    extractor.reset();

    // Constant tone over 20 frames should result in novelty falling back to 0
    uint8_t frame[64];
    for (int b = 0; b < 64; ++b) frame[b] = 150;

    extractor.process_frame(frame); // transient onset
    for (int i = 0; i < 20; ++i) {
        NoveltySample s = extractor.process_frame(frame);
        // Once steady, half-wave rectified delta is 0
        assert(s.novelty == 0.0f);
    }
}

int main() {
    test_subband_flux_separation();
    test_adaptive_threshold_suppresses_dc();
    std::cout << "test_tempo_novelty PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Add test target to CMakeLists.txt and verify it fails**

In `CMakeLists.txt`, add:
```cmake
add_library(audio_codecs_tempo STATIC
    src/tempo/tempo_novelty.cpp
)
target_include_directories(audio_codecs_tempo PUBLIC include)
target_link_libraries(audio_codecs_tempo PUBLIC audio_codecs_spectrum audio_codecs_preview)

add_executable(test_tempo_novelty tests/test_tempo_novelty.cpp)
target_link_libraries(test_tempo_novelty PRIVATE audio_codecs_tempo)
add_test(NAME TempoNoveltyTest COMMAND test_tempo_novelty)
```
Run:
`cmake --build build --target test_tempo_novelty`
Expected: FAIL with `#include <audio_codecs/tempo/tempo_novelty.h> file not found`.

- [ ] **Step 3: Implement tempo_novelty.h and tempo_novelty.cpp**

Create `include/audio_codecs/tempo/tempo_novelty.h`:
```cpp
#pragma once

#include <cstdint>
#include <cstddef>

namespace audio_codecs::tempo {

struct NoveltySample {
    float bass_flux;
    float snare_flux;
    float high_flux;
    float raw_novelty;
    float novelty; // thresholded
};

class TempoNoveltyExtractor {
public:
    TempoNoveltyExtractor();
    void reset();

    NoveltySample process_frame(const uint8_t bands[64]);

private:
    uint8_t prev_bands_[64];
    bool has_prev_frame_;

    static constexpr size_t K_THRESHOLD_WINDOW = 9;
    float threshold_history_[K_THRESHOLD_WINDOW];
    size_t threshold_idx_;
    size_t threshold_count_;
};

} // namespace audio_codecs::tempo
```

Create `src/tempo/tempo_novelty.cpp`:
```cpp
#include <audio_codecs/tempo/tempo_novelty.h>
#include <algorithm>
#include <cstring>

namespace audio_codecs::tempo {

TempoNoveltyExtractor::TempoNoveltyExtractor() {
    reset();
}

void TempoNoveltyExtractor::reset() {
    std::memset(prev_bands_, 0, sizeof(prev_bands_));
    has_prev_frame_ = false;
    std::memset(threshold_history_, 0, sizeof(threshold_history_));
    threshold_idx_ = 0;
    threshold_count_ = 0;
}

NoveltySample TempoNoveltyExtractor::process_frame(const uint8_t bands[64]) {
    NoveltySample sample{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    if (!has_prev_frame_) {
        std::memcpy(prev_bands_, bands, 64);
        has_prev_frame_ = true;
        return sample;
    }

    // Sub-band half-wave rectified differences
    for (int b = 0; b <= 15; ++b) {
        if (bands[b] > prev_bands_[b]) {
            sample.bass_flux += static_cast<float>(bands[b] - prev_bands_[b]);
        }
    }
    for (int b = 16; b <= 42; ++b) {
        if (bands[b] > prev_bands_[b]) {
            sample.snare_flux += static_cast<float>(bands[b] - prev_bands_[b]);
        }
    }
    for (int b = 43; b <= 63; ++b) {
        if (bands[b] > prev_bands_[b]) {
            sample.high_flux += static_cast<float>(bands[b] - prev_bands_[b]);
        }
    }

    std::memcpy(prev_bands_, bands, 64);

    sample.raw_novelty = 1.0f * sample.bass_flux + 0.8f * sample.snare_flux + 0.2f * sample.high_flux;

    // Rolling local threshold subtraction
    threshold_history_[threshold_idx_] = sample.raw_novelty;
    threshold_idx_ = (threshold_idx_ + 1) % K_THRESHOLD_WINDOW;
    if (threshold_count_ < K_THRESHOLD_WINDOW) {
        threshold_count_++;
    }

    float mean_val = 0.0f;
    for (size_t i = 0; i < threshold_count_; ++i) {
        mean_val += threshold_history_[i];
    }
    mean_val /= static_cast<float>(threshold_count_);

    sample.novelty = std::max(0.0f, sample.raw_novelty - mean_val);
    return sample;
}

} // namespace audio_codecs::tempo
```

- [ ] **Step 4: Build and run test to verify it passes**

Run:
`cmake --build build --target test_tempo_novelty && ctest --test-dir build -R TempoNoveltyTest --output-on-failure`
Expected: `test_tempo_novelty PASSED` (100% passed).

- [ ] **Step 5: Commit**

```bash
git add include/audio_codecs/tempo/tempo_novelty.h src/tempo/tempo_novelty.cpp tests/test_tempo_novelty.cpp CMakeLists.txt
git commit -m "feat(tempo): implement sub-band novelty extractor and adaptive thresholding"
```

---

### Task 3: Tempo Induction & Comb Resonator Engine (tempo_inducer.h/.cpp)

**Files:**
- Create: `include/audio_codecs/tempo/tempo_inducer.h`
- Create: `src/tempo/tempo_inducer.cpp`
- Test: `tests/test_tempo_inducer.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `float novelty` stream from `TempoNoveltyExtractor`.
- Produces: `TempoInducer` tracking rolling 512-frame window, computing harmonic comb resonator score, Gaussian prior weighting, sub-frame parabolic peak interpolation, and returning `(estimated_bpm, confidence, lag_frames)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_tempo_inducer.cpp`:
```cpp
#include <audio_codecs/tempo/tempo_inducer.h>
#include <cassert>
#include <cmath>
#include <iostream>

using namespace audio_codecs::tempo;

void test_tempo_induction_120bpm() {
    TempoInducer inducer;
    inducer.reset(86.133f); // 44100 / 512

    // At 120 BPM, interval is 0.5 sec -> 0.5 * 86.133 = 43.066 frames
    float period_frames = 86.133f * 0.5f;

    // Feed 512 frames containing pulses every 43.066 frames
    float next_pulse = 0.0f;
    for (int f = 0; f < 512; ++f) {
        float novelty = 0.0f;
        if (std::fabs(static_cast<float>(f) - next_pulse) < 0.6f) {
            novelty = 100.0f;
            next_pulse += period_frames;
        }
        inducer.feed_sample(novelty);
    }

    TempoEstimate est = inducer.estimate_tempo();
    assert(est.bpm >= 119.0f && est.bpm <= 121.0f);
    assert(est.confidence > 100);
}

void test_octave_disambiguation() {
    TempoInducer inducer;
    inducer.reset(86.133f);

    // Feed 130 BPM with some half-tempo subharmonics
    float period_frames = 86.133f * (60.0f / 130.0f); // ~39.75 frames
    for (int f = 0; f < 512; ++f) {
        float val = 0.0f;
        if (f % static_cast<int>(std::round(period_frames)) == 0) {
            val = 80.0f;
        }
        inducer.feed_sample(val);
    }

    TempoEstimate est = inducer.estimate_tempo();
    assert(est.bpm >= 128.5f && est.bpm <= 131.5f);
}

int main() {
    test_tempo_induction_120bpm();
    test_octave_disambiguation();
    std::cout << "test_tempo_inducer PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Add test target to CMakeLists.txt and verify it fails**

In `CMakeLists.txt`, update `audio_codecs_tempo` sources to include `src/tempo/tempo_inducer.cpp`, and add `test_tempo_inducer`.
Run:
`cmake --build build --target test_tempo_inducer`
Expected: FAIL with missing `tempo_inducer.h`.

- [ ] **Step 3: Implement tempo_inducer.h and tempo_inducer.cpp**

Create `include/audio_codecs/tempo/tempo_inducer.h`:
```cpp
#pragma once

#include <cstdint>
#include <cstddef>

namespace audio_codecs::tempo {

struct TempoEstimate {
    float bpm;
    uint8_t confidence;
    float lag_frames;
};

class TempoInducer {
public:
    static constexpr size_t K_WINDOW_SIZE = 512;
    static constexpr int K_MIN_LAG = 21; // ~240 BPM at 86.13 Hz
    static constexpr int K_MAX_LAG = 89; // ~58 BPM at 86.13 Hz

    TempoInducer();
    void reset(float frame_rate = 86.133f);

    void feed_sample(float novelty);
    TempoEstimate estimate_tempo() const;

private:
    float buffer_[K_WINDOW_SIZE];
    size_t head_;
    size_t count_;
    float frame_rate_;

    float prior_weight(float lag) const;
};

} // namespace audio_codecs::tempo
```

Create `src/tempo/tempo_inducer.cpp`:
```cpp
#include <audio_codecs/tempo/tempo_inducer.h>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace audio_codecs::tempo {

TempoInducer::TempoInducer() {
    reset();
}

void TempoInducer::reset(float frame_rate) {
    std::memset(buffer_, 0, sizeof(buffer_));
    head_ = 0;
    count_ = 0;
    frame_rate_ = frame_rate > 0.0f ? frame_rate : 86.133f;
}

void TempoInducer::feed_sample(float novelty) {
    buffer_[head_] = novelty;
    head_ = (head_ + 1) % K_WINDOW_SIZE;
    if (count_ < K_WINDOW_SIZE) {
        count_++;
    }
}

float TempoInducer::prior_weight(float lag) const {
    if (lag <= 0.0f) return 0.0f;
    float bpm = (60.0f * frame_rate_) / lag;
    // Log-Gaussian prior centered at 120 BPM with sigma = 0.8 octaves
    float log_diff = std::log2(bpm / 120.0f);
    return std::exp(- (log_diff * log_diff) / (2.0f * 0.8f * 0.8f));
}

TempoEstimate TempoInducer::estimate_tempo() const {
    if (count_ < K_MAX_LAG * 2) {
        return {120.0f, 0, 43.0f};
    }

    // Unroll circular buffer into contiguous temporal array
    float temp[K_WINDOW_SIZE];
    for (size_t i = 0; i < K_WINDOW_SIZE; ++i) {
        size_t idx = (head_ + i) % K_WINDOW_SIZE;
        temp[i] = buffer_[idx];
    }

    float comb_scores[K_MAX_LAG + 1];
    std::memset(comb_scores, 0, sizeof(comb_scores));

    float max_score = -1.0f;
    int best_lag = K_MIN_LAG;
    float sum_scores = 0.0f;
    int valid_lags = 0;

    for (int tau = K_MIN_LAG; tau <= K_MAX_LAG; ++tau) {
        // Autocorrelation at tau
        float r1 = 0.0f;
        for (int k = 0; k < static_cast<int>(K_WINDOW_SIZE) - tau; ++k) {
            r1 += temp[k] * temp[k + tau];
        }

        // Comb harmonic at 2*tau
        float r2 = 0.0f;
        if (2 * tau < static_cast<int>(K_WINDOW_SIZE)) {
            for (int k = 0; k < static_cast<int>(K_WINDOW_SIZE) - 2 * tau; ++k) {
                r2 += temp[k] * temp[k + 2 * tau];
            }
        }

        // Comb harmonic at 4*tau
        float r4 = 0.0f;
        if (4 * tau < static_cast<int>(K_WINDOW_SIZE)) {
            for (int k = 0; k < static_cast<int>(K_WINDOW_SIZE) - 4 * tau; ++k) {
                r4 += temp[k] * temp[k + 4 * tau];
            }
        }

        float score = (r1 + 0.5f * r2 + 0.25f * r4) * prior_weight(static_cast<float>(tau));
        comb_scores[tau] = score;
        sum_scores += score;
        valid_lags++;

        if (score > max_score) {
            max_score = score;
            best_lag = tau;
        }
    }

    // 3-point parabolic interpolation around peak
    float sub_lag = static_cast<float>(best_lag);
    if (best_lag > K_MIN_LAG && best_lag < K_MAX_LAG) {
        float y_prev = comb_scores[best_lag - 1];
        float y_cur  = comb_scores[best_lag];
        float y_next = comb_scores[best_lag + 1];
        float denom = 2.0f * (y_prev - 2.0f * y_cur + y_next);
        if (std::fabs(denom) > 1e-6f) {
            float delta = (y_prev - y_next) / denom;
            if (delta >= -1.0f && delta <= 1.0f) {
                sub_lag += delta;
            }
        }
    }

    float mean_score = valid_lags > 0 ? (sum_scores / valid_lags) : 0.0f;
    float confidence_ratio = max_score > mean_score ? ((max_score - mean_score) / (max_score + 1e-4f)) : 0.0f;
    uint8_t confidence = static_cast<uint8_t>(std::clamp(confidence_ratio * 255.0f, 0.0f, 255.0f));

    float bpm = (60.0f * frame_rate_) / sub_lag;
    return {bpm, confidence, sub_lag};
}

} // namespace audio_codecs::tempo
```

- [ ] **Step 4: Build and run test to verify it passes**

Run:
`cmake --build build --target test_tempo_inducer && ctest --test-dir build -R TempoInducerTest --output-on-failure`
Expected: `test_tempo_inducer PASSED` (100% passed).

- [ ] **Step 5: Commit**

```bash
git add include/audio_codecs/tempo/tempo_inducer.h src/tempo/tempo_inducer.cpp tests/test_tempo_inducer.cpp CMakeLists.txt
git commit -m "feat(tempo): implement comb filter tempo inducer and sub-frame interpolation"
```

---

### Task 4: Sliced Beat Tracking, Downbeat Alignment & Generator (tempo_generator.h/.cpp)

**Files:**
- Create: `include/audio_codecs/tempo/tempo_generator.h`
- Create: `src/tempo/tempo_generator.cpp`
- Test: `tests/test_tempo_generator.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `preview::SeekableReader* asv_reader`, `preview::SeekableWriter* att_writer`, `TempoNoveltyExtractor`, `TempoInducer`.
- Produces: `TempoGenerator` with `step(size_t max_frames)`, downbeat/meter alignment, tempo curve serialization, and complete ATT1 generation.

- [ ] **Step 1: Write the failing test**

Create `tests/test_tempo_generator.cpp`:
```cpp
#include <audio_codecs/tempo/tempo_generator.h>
#include <audio_codecs/spectrum/spectrum_types.h>
#include <audio_codecs/preview/preview_stream.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace audio_codecs::tempo;
using namespace audio_codecs::spectrum;
using namespace audio_codecs::preview;

void test_tempo_generator_synthetic_120bpm() {
    const uint32_t total_frames = 2580;
    const size_t asv_size = sizeof(AsvHeader) + total_frames * 64;
    std::vector<uint8_t> asv_buffer(asv_size, 0);

    AsvHeader hdr{};
    std::memcpy(hdr.magic, ASV_MAGIC, 4);
    hdr.version = ASV_VERSION;
    hdr.header_size = 128;
    hdr.duration_ms = 30000;
    hdr.sample_rate = 44100;
    hdr.num_bands = 64;
    hdr.hop_size = 512;
    hdr.window_size = 1024;
    hdr.total_frames = total_frames;
    hdr.lods[0].offset = 128;
    hdr.lods[0].total_frames = total_frames;
    hdr.lods[0].decimation_factor = 1;

    std::memcpy(asv_buffer.data(), &hdr, sizeof(hdr));

    for (uint32_t f = 0; f < total_frames; ++f) {
        uint8_t* frame_ptr = asv_buffer.data() + 128 + f * 64;
        if (f % 43 == 0) {
            int beat = (f / 43) % 4;
            if (beat == 0 || beat == 2) {
                for (int b = 2; b <= 8; ++b) frame_ptr[b] = 220;
            } else {
                for (int b = 20; b <= 32; ++b) frame_ptr[b] = 200;
            }
        }
    }

    MemoryReader asv_reader(asv_buffer.data(), asv_buffer.size());
    std::vector<uint8_t> att_buffer(32768, 0);
    MemoryWriter att_writer(att_buffer.data(), att_buffer.size());

    TempoGenerator generator;
    bool init_ok = generator.init(&asv_reader, &att_writer);
    assert(init_ok);

    while (generator.step(128)) {}

    assert(generator.is_complete());
    assert(!generator.has_error());

    const AttHeader& out_hdr = generator.header();
    assert(std::memcmp(out_hdr.magic, "ATT1", 4) == 0);
    float detected_bpm = static_cast<float>(out_hdr.global_bpm_q16) / 65536.0f;
    assert(detected_bpm >= 119.0f && detected_bpm <= 121.0f);
    assert(out_hdr.total_beats > 50);
    assert(out_hdr.total_bars > 12);
}

int main() {
    test_tempo_generator_synthetic_120bpm();
    std::cout << "test_tempo_generator PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Add test target to CMakeLists.txt and verify it fails**

In `CMakeLists.txt`, update `audio_codecs_tempo` with `src/tempo/tempo_generator.cpp` and add `test_tempo_generator`.
Run:
`cmake --build build --target test_tempo_generator`
Expected: FAIL with missing `tempo_generator.h`.

- [ ] **Step 3: Implement tempo_generator.h and tempo_generator.cpp**

Create `include/audio_codecs/tempo/tempo_generator.h` and `src/tempo/tempo_generator.cpp` as detailed in Task 4 implementation above.

- [ ] **Step 4: Build and run test to verify it passes**

Run:
`cmake --build build --target test_tempo_generator && ctest --test-dir build -R TempoGeneratorTest --output-on-failure`
Expected: `test_tempo_generator PASSED` (100% passed).

- [ ] **Step 5: Commit**

```bash
git add include/audio_codecs/tempo/tempo_generator.h src/tempo/tempo_generator.cpp tests/test_tempo_generator.cpp CMakeLists.txt
git commit -m "feat(tempo): implement cooperative sliced tempo generator and beat tracking"
```

---

### Task 5: Tempo Reader & Query Engine (tempo_reader.h/.cpp)

**Files:**
- Create: `include/audio_codecs/tempo/tempo_reader.h`
- Create: `src/tempo/tempo_reader.cpp`
- Test: `tests/test_tempo_reader.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `preview::SeekableReader* att_reader`.
- Produces: `TempoReader` offering `get_bpm_at(time_ms)`, `get_nearest_beat(time_ms)`, `time_to_beat(time_ms)`, `beat_to_time(beat)`, and `read_beats(start_ms, duration_ms)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_tempo_reader.cpp` as specified in Task 5 above.

- [ ] **Step 2: Add test target to CMakeLists.txt and verify it fails**

In `CMakeLists.txt`, update `audio_codecs_tempo` with `src/tempo/tempo_reader.cpp` and add `test_tempo_reader`.
Run:
`cmake --build build --target test_tempo_reader`
Expected: FAIL with missing `tempo_reader.h`.

- [ ] **Step 3: Implement tempo_reader.h and tempo_reader.cpp**

Create `include/audio_codecs/tempo/tempo_reader.h` and `src/tempo/tempo_reader.cpp` as detailed in Task 5 above.

- [ ] **Step 4: Build and run test to verify it passes**

Run:
`cmake --build build --target test_tempo_reader && ctest --test-dir build -R TempoReaderTest --output-on-failure`
Expected: `test_tempo_reader PASSED` (100% passed).

- [ ] **Step 5: Commit**

```bash
git add include/audio_codecs/tempo/tempo_reader.h src/tempo/tempo_reader.cpp tests/test_tempo_reader.cpp CMakeLists.txt
git commit -m "feat(tempo): implement tempo reader with binary search and beat interpolation"
```

---

### Task 6: Umbrella Header, CMake Integration & End-to-End Pipeline Test

**Files:**
- Create: `include/audio_codecs/tempo.h`
- Modify: `include/audio_codecs/audio_codecs.h`
- Test: `tests/test_tempo_integration.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `SpectrumGenerator`, `TempoGenerator`, `TempoReader`.
- Produces: Complete end-to-end integration test validating Audio PCM $\to$ ASV1 $\to$ ATT1 $\to$ TempoReader.

- [ ] **Step 1: Write the failing test**

Create `tests/test_tempo_integration.cpp` as specified in Task 6 above.

- [ ] **Step 2: Add test target and verify it fails**

In `CMakeLists.txt`, add `test_tempo_integration`.
Run:
`cmake --build build --target test_tempo_integration`
Expected: FAIL with `#include <audio_codecs/tempo.h> file not found`.

- [ ] **Step 3: Implement tempo.h and expose in audio_codecs.h**

Create `include/audio_codecs/tempo.h`:
```cpp
#pragma once

#include <audio_codecs/tempo/tempo_types.h>
#include <audio_codecs/tempo/tempo_novelty.h>
#include <audio_codecs/tempo/tempo_inducer.h>
#include <audio_codecs/tempo/tempo_generator.h>
#include <audio_codecs/tempo/tempo_reader.h>
```

In `include/audio_codecs/audio_codecs.h`, append:
```cpp
#include "audio_codecs/tempo.h"
```

- [ ] **Step 4: Build and run test suite**

Run:
`cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: 100% tests passed (all 74 existing tests + 5 new tempo tests = 79 tests passing).

- [ ] **Step 5: Commit**

```bash
git add include/audio_codecs/tempo.h include/audio_codecs/audio_codecs.h tests/test_tempo_integration.cpp CMakeLists.txt
git commit -m "feat(tempo): expose umbrella tempo.h and add end-to-end integration test"
```
