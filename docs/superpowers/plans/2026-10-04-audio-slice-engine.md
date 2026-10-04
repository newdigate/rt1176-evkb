# Audio Slice Engine & ASL File Format Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a cross-platform audio slice detection engine, beat-quantized loop slicer, tempo-independent playback helper, Type 0 MIDI exporter, and binary slice format (`.asl` / **ASL1**) in `audio-codecs` that operates with zero dynamic heap allocations during steady-state processing and runs cooperatively on both desktop and ARM Cortex-M7 microcontrollers.

**Architecture:** The slice engine consists of decoupled modular components: binary format structs (`slice_types.h`), time-domain transient detector with pre-emphasis and zero-crossing snapping (`slice_detector.h/.cpp`), multi-mode metric grid quantizer and cooperative generator (`slice_generator.h/.cpp`), zero-heap random-access reader (`slice_reader.h/.cpp`), playback voice envelope and tempo-stretching helper (`slice_playback.h/.cpp`), standard Type 0 MIDI file exporter (`slice_midi_writer.h/.cpp`), and top-level umbrella exports (`slice.h`). It streams PCM audio in cooperative slices and writes sector-aligned `.asl` files using zero-allocation buffers.

**Tech Stack:** C++17, CMake, standard C/C++ math library, `audio_codecs::preview` streams.

**Spec:** `docs/superpowers/specs/2026-10-04-audio-slice-engine-design.md`

## Global Constraints
- Pure C++17; zero dynamic heap allocations in steady-state analysis, query, or playback paths.
- No Arduino or platform-specific headers inside `include/audio_codecs/`.
- `sizeof(AslHeader)` must be exactly 128 bytes (asserted via compile-time `static_assert`).
- `sizeof(AslSlice)` must be exactly 32 bytes (asserted via compile-time `static_assert`).
- All integers stored in Little-Endian byte order.
- Time-domain transient analysis requires zero FFTs, micro-hop size $H = 32$ samples, RAM working state $< 200\text{ bytes}$.
- All native desktop and embedded code is 100% MIT-licensed.
- Tests must execute correctly without side-effects inside `assert(...)` (clean under `-DNDEBUG`, using `bool ok = ...; assert(ok);`).

---

### Task 1: Core Format Structs and Header Serialization (slice_types.h)

**Files:**
- Create: `include/audio_codecs/slice/slice_types.h`
- Test: `tests/test_slice_types.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Standard integer types (`<cstdint>`, `<cstddef>`).
- Produces: `AslHeader`, `AslSlice`, `AslSliceMode`, `AslTailMode`, `AslFlags`, `ASL_MAGIC`, `ASL_VERSION`, `ASL_PPQN`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_slice_types.cpp`:
```cpp
#include <audio_codecs/slice/slice_types.h>
#include <cassert>
#include <cstring>
#include <iostream>

using namespace audio_codecs::slice;

void test_sizes_and_alignments() {
    static_assert(sizeof(AslHeader) == 128, "AslHeader size must be exactly 128 bytes");
    static_assert(sizeof(AslSlice) == 32, "AslSlice size must be exactly 32 bytes");

    assert(sizeof(AslHeader) == 128);
    assert(sizeof(AslSlice) == 32);
}

void test_header_defaults_and_roundtrip() {
    AslHeader hdr{};
    hdr.magic = ASL_MAGIC;
    hdr.version = ASL_VERSION;
    hdr.header_size = 128;
    hdr.duration_samples = 44100 * 4;
    hdr.duration_ms = 4000;
    hdr.sample_rate = 44100;
    hdr.channels = 2;
    hdr.time_signature_num = 4;
    hdr.time_signature_denom = 4;
    hdr.slice_mode = ASL_MODE_TRANSIENT_TO_GRID;
    hdr.original_bpm_q16 = (120 << 16);
    hdr.ppqn = ASL_PPQN;
    hdr.num_bars = 2;
    hdr.total_slices = 16;
    hdr.slice_descriptor_size = sizeof(AslSlice);
    hdr.slices_offset = 128;
    hdr.flags = ASL_FLAG_LOOPABLE;

    uint8_t buffer[128];
    std::memcpy(buffer, &hdr, sizeof(hdr));

    AslHeader read_hdr{};
    std::memcpy(&read_hdr, buffer, sizeof(read_hdr));

    assert(read_hdr.magic == ASL_MAGIC);
    assert(read_hdr.version == ASL_VERSION);
    assert(read_hdr.header_size == 128);
    assert(read_hdr.duration_samples == 44100 * 4);
    assert(read_hdr.duration_ms == 4000);
    assert(read_hdr.sample_rate == 44100);
    assert(read_hdr.channels == 2);
    assert(read_hdr.time_signature_num == 4);
    assert(read_hdr.time_signature_denom == 4);
    assert(read_hdr.slice_mode == ASL_MODE_TRANSIENT_TO_GRID);
    assert(read_hdr.original_bpm_q16 == (120 << 16));
    assert(read_hdr.ppqn == 480);
    assert(read_hdr.num_bars == 2);
    assert(read_hdr.total_slices == 16);
    assert(read_hdr.slice_descriptor_size == 32);
    assert(read_hdr.slices_offset == 128);
    assert(read_hdr.flags == ASL_FLAG_LOOPABLE);
}

void test_slice_descriptor_fields() {
    AslSlice slice{};
    slice.start_sample = 22050;
    slice.length_samples = 11025;
    slice.musical_tick = 240;
    slice.midi_note = 36;
    slice.bar_index = 0;
    slice.beat_within_bar = 0;
    slice.subdivision = 2;
    slice.gain_db_q8 = 0;
    slice.transient_energy = 32000;
    slice.decay_ms = 250;
    slice.tail_mode = ASL_TAIL_STRETCH_DECAY;
    slice.flags = ASL_SLICE_FLAG_LOCKED;

    uint8_t buffer[32];
    std::memcpy(buffer, &slice, sizeof(slice));

    AslSlice read_slice{};
    std::memcpy(&read_slice, buffer, sizeof(read_slice));

    assert(read_slice.start_sample == 22050);
    assert(read_slice.length_samples == 11025);
    assert(read_slice.musical_tick == 240);
    assert(read_slice.midi_note == 36);
    assert(read_slice.bar_index == 0);
    assert(read_slice.beat_within_bar == 0);
    assert(read_slice.subdivision == 2);
    assert(read_slice.gain_db_q8 == 0);
    assert(read_slice.transient_energy == 32000);
    assert(read_slice.decay_ms == 250);
    assert(read_slice.tail_mode == ASL_TAIL_STRETCH_DECAY);
    assert(read_slice.flags == ASL_SLICE_FLAG_LOCKED);
}

int main() {
    test_sizes_and_alignments();
    test_header_defaults_and_roundtrip();
    test_slice_descriptor_fields();
    std::cout << "test_slice_types PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake -B build && cmake --build build --target test_slice_types`
Expected: Build failure due to missing `audio_codecs/slice/slice_types.h`.

- [ ] **Step 3: Write minimal implementation**

Create `include/audio_codecs/slice/slice_types.h`:
```cpp
#pragma once

#include <cstdint>
#include <cstddef>

namespace audio_codecs::slice {

constexpr uint32_t ASL_MAGIC = 0x314C5341; // 'ASL1' in Little-Endian
constexpr uint16_t ASL_VERSION = 1;
constexpr uint16_t ASL_PPQN = 480;

enum AslSliceMode : uint8_t {
    ASL_MODE_TRANSIENT         = 0,
    ASL_MODE_GRID              = 1,
    ASL_MODE_TRANSIENT_TO_GRID = 2
};

enum AslTailMode : uint8_t {
    ASL_TAIL_ONE_SHOT      = 0,
    ASL_TAIL_STRETCH_DECAY = 1,
    ASL_TAIL_SUSTAIN_LOOP  = 2
};

enum AslHeaderFlags : uint16_t {
    ASL_FLAG_NONE              = 0x0000,
    ASL_FLAG_LOOPABLE          = 0x0001,
    ASL_FLAG_HAS_SUSTAIN_LOOPS = 0x0002
};

enum AslSliceFlags : uint8_t {
    ASL_SLICE_FLAG_NONE     = 0x00,
    ASL_SLICE_FLAG_LOCKED   = 0x01,
    ASL_SLICE_FLAG_REVERSED = 0x02,
    ASL_SLICE_FLAG_MUTED    = 0x04
};

#pragma pack(push, 1)

struct AslHeader {
    uint32_t magic;                 // 0x314C5341 ('ASL1')
    uint16_t version;               // Format version: 1
    uint16_t header_size;           // Size of this header in bytes: 128
    uint32_t duration_samples;      // Total audio duration in PCM frames
    uint32_t duration_ms;           // Total audio duration in milliseconds
    uint32_t sample_rate;           // Audio sample rate (e.g. 44100, 48000)
    uint8_t  channels;              // Channel count (1 = mono, 2 = stereo)
    uint8_t  time_signature_num;    // Time signature numerator (e.g. 4)
    uint8_t  time_signature_denom;  // Time signature denominator (e.g. 4)
    uint8_t  slice_mode;            // AslSliceMode (0 = Transient, 1 = Grid, 2 = Quantized)
    uint32_t original_bpm_q16;      // Original tempo in Q16 fixed-point (e.g. 120.0 << 16)
    uint16_t ppqn;                  // Pulses Per Quarter Note (standard: 480)
    uint16_t num_bars;              // Total bars spanned by loop
    uint16_t total_slices;          // Total number of slices in file
    uint16_t slice_descriptor_size; // Size of each descriptor: 32 bytes
    uint32_t slices_offset;         // File offset to slice descriptors: 128
    uint16_t flags;                 // Bitmask of AslHeaderFlags
    uint8_t  reserved[80];          // Zero-padded future expansion
};
static_assert(sizeof(AslHeader) == 128, "AslHeader must be exactly 128 bytes");

struct AslSlice {
    uint32_t start_sample;          // Sample offset from start of audio
    uint32_t length_samples;        // Length of slice in PCM frames
    uint32_t musical_tick;          // Musical position in PPQN ticks from start
    uint8_t  midi_note;             // Chromatic MIDI note trigger (36 = C1, 37 = C#1...)
    uint8_t  bar_index;             // Bar index (0-indexed)
    uint8_t  beat_within_bar;       // Beat index within bar (0-indexed: 0..num-1)
    uint8_t  subdivision;           // Subdivision within beat (0-indexed)
    int16_t  gain_db_q8;            // Recommended gain trim in Q8 dB (0 = 0 dB)
    uint16_t transient_energy;      // Peak transient energy novelty (0..65535)
    uint16_t decay_ms;              // Natural decay time in milliseconds
    uint8_t  tail_mode;             // AslTailMode (0 = OneShot, 1 = StretchDecay, 2 = SustainLoop)
    uint8_t  flags;                 // Bitmask of AslSliceFlags
    uint8_t  reserved[8];           // Zero-padded alignment
};
static_assert(sizeof(AslSlice) == 32, "AslSlice must be exactly 32 bytes");

#pragma pack(pop)

} // namespace audio_codecs::slice
```

Update `CMakeLists.txt` to add `test_slice_types`:
```cmake
add_executable(test_slice_types tests/test_slice_types.cpp)
target_include_directories(test_slice_types PRIVATE include)
add_test(NAME SliceTypesTest COMMAND test_slice_types)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake -B build && cmake --build build --target test_slice_types && ctest --test-dir build -R SliceTypesTest --output-on-failure`
Expected: PASS (`test_slice_types PASSED`).

- [ ] **Step 5: Commit**

```bash
git add include/audio_codecs/slice/slice_types.h tests/test_slice_types.cpp CMakeLists.txt
git commit -m "feat(slice): add AslHeader and AslSlice structs with static assertions"
```

---

### Task 2: MCU Time-Domain Transient Detector (slice_detector.h/.cpp)

**Files:**
- Create: `include/audio_codecs/slice/slice_detector.h`
- Create: `src/slice/slice_detector.cpp`
- Test: `tests/test_slice_detector.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `include/audio_codecs/slice/slice_types.h`
- Produces: `audio_codecs::slice::SliceDetector`, `DetectorConfig`, `SliceDetector::find_zero_crossing_backward`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_slice_detector.cpp`:
```cpp
#include <audio_codecs/slice/slice_detector.h>
#include <cassert>
#include <cmath>
#include <vector>
#include <iostream>

using namespace audio_codecs::slice;

void test_zero_crossing_backward() {
    // Generate a sine wave that crosses zero at index 50
    std::vector<int16_t> pcm(128, 0);
    for (int i = 0; i < 128; ++i) {
        // Zero at i = 50
        double val = std::sin((i - 50) * 0.1) * 10000.0;
        pcm[i] = static_cast<int16_t>(val);
    }
    // Peak is at around index 65
    uint32_t peak = 65;
    uint32_t zc = SliceDetector::find_zero_crossing_backward(pcm.data(), peak, 32);
    assert(zc == 50 || zc == 51);
}

void test_detector_impulse_detection() {
    SliceDetector detector;
    DetectorConfig cfg;
    cfg.sample_rate = 44100;
    cfg.sensitivity = 0.5f;
    cfg.min_slice_samples = 512;
    cfg.pre_emphasis_alpha = 0.95f;
    bool ok = detector.init(cfg);
    assert(ok);

    // Create 44100 samples (1 sec) with 2 synthetic impulses at sample 4410 (0.1s) and 22050 (0.5s)
    std::vector<int16_t> pcm(44100, 0);
    // Impulse 1 at 4410
    pcm[4410] = 20000;
    pcm[4411] = 15000;
    pcm[4412] = 10000;
    pcm[4413] = -5000;

    // Impulse 2 at 22050
    pcm[22050] = 25000;
    pcm[22051] = 18000;
    pcm[22052] = 8000;
    pcm[22053] = -6000;

    uint32_t detected[16];
    uint32_t count = detector.process_block(pcm.data(), static_cast<uint32_t>(pcm.size()), detected, 16);
    
    // We should detect exactly 2 transients
    assert(count == 2);
    // Transient 1 should be snapped close to 4410
    assert(detected[0] >= 4380 && detected[0] <= 4412);
    // Transient 2 should be snapped close to 22050
    assert(detected[1] >= 22020 && detected[1] <= 22052);
}

void test_detector_silence_rejection() {
    SliceDetector detector;
    DetectorConfig cfg;
    bool ok = detector.init(cfg);
    assert(ok);

    std::vector<int16_t> silence(4410, 0);
    uint32_t detected[16];
    uint32_t count = detector.process_block(silence.data(), static_cast<uint32_t>(silence.size()), detected, 16);
    assert(count == 0);
}

int main() {
    test_zero_crossing_backward();
    test_detector_impulse_detection();
    test_detector_silence_rejection();
    std::cout << "test_slice_detector PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake -B build && cmake --build build --target test_slice_detector`
Expected: Build failure due to missing `audio_codecs/slice/slice_detector.h`.

- [ ] **Step 3: Write minimal implementation**

Create `include/audio_codecs/slice/slice_detector.h`:
```cpp
#pragma once

#include <audio_codecs/slice/slice_types.h>
#include <cstdint>
#include <cstddef>

namespace audio_codecs::slice {

struct DetectorConfig {
    float sensitivity = 0.5f;          // 0.0f (least sensitive) to 1.0f (most sensitive)
    uint32_t min_slice_samples = 512;    // Minimum samples between slices (~11.6 ms @ 44.1k)
    float pre_emphasis_alpha = 0.95f;  // High-pass filter coefficient
    uint32_t sample_rate = 44100;
};

class SliceDetector {
public:
    static constexpr uint32_t HOP_SIZE = 32;       // Micro-hop size in samples
    static constexpr uint32_t ROLLING_HOPS = 16;   // History window for adaptive threshold

    SliceDetector();
    bool init(const DetectorConfig& config);
    void reset();

    // Process a block of mono PCM samples
    // Returns number of transients detected in this block
    uint32_t process_block(const int16_t* pcm_samples, uint32_t count,
                           uint32_t* out_transient_indices, uint32_t max_indices);

    // Search backward from peak_index to find the zero-crossing sample
    static uint32_t find_zero_crossing_backward(const int16_t* pcm_samples,
                                                uint32_t peak_index,
                                                uint32_t max_search_samples);

private:
    DetectorConfig config_{};
    int16_t prev_raw_sample_ = 0;
    float prev_hop_energy_ = 0.0f;
    uint32_t total_samples_processed_ = 0;
    uint32_t last_transient_sample_ = 0;
    bool has_previous_transient_ = false;

    // Rolling statistics for adaptive threshold
    float energy_history_[ROLLING_HOPS]{};
    uint32_t history_idx_ = 0;
    uint32_t history_count_ = 0;
};

} // namespace audio_codecs::slice
```

Create `src/slice/slice_detector.cpp`:
```cpp
#include <audio_codecs/slice/slice_detector.h>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace audio_codecs::slice {

SliceDetector::SliceDetector() {
    reset();
}

bool SliceDetector::init(const DetectorConfig& config) {
    config_ = config;
    reset();
    return true;
}

void SliceDetector::reset() {
    prev_raw_sample_ = 0;
    prev_hop_energy_ = 0.0f;
    total_samples_processed_ = 0;
    last_transient_sample_ = 0;
    has_previous_transient_ = false;
    history_idx_ = 0;
    history_count_ = 0;
    std::memset(energy_history_, 0, sizeof(energy_history_));
}

uint32_t SliceDetector::find_zero_crossing_backward(const int16_t* pcm_samples,
                                                    uint32_t peak_index,
                                                    uint32_t max_search_samples) {
    if (pcm_samples == nullptr || peak_index == 0) {
        return peak_index;
    }

    uint32_t limit = (peak_index > max_search_samples) ? (peak_index - max_search_samples) : 0;
    for (uint32_t i = peak_index; i > limit; --i) {
        int32_t s0 = pcm_samples[i - 1];
        int32_t s1 = pcm_samples[i];
        if ((s0 <= 0 && s1 >= 0) || (s0 >= 0 && s1 <= 0)) {
            return i;
        }
    }
    return limit;
}

uint32_t SliceDetector::process_block(const int16_t* pcm_samples, uint32_t count,
                                      uint32_t* out_transient_indices, uint32_t max_indices) {
    if (pcm_samples == nullptr || count == 0 || out_transient_indices == nullptr || max_indices == 0) {
        return 0;
    }

    uint32_t detected_count = 0;
    uint32_t num_hops = count / HOP_SIZE;

    // Sensitivity factor mapping: sensitivity 0.0 -> k = 3.5, sensitivity 1.0 -> k = 0.5
    float k_sens = 3.5f - (config_.sensitivity * 3.0f);

    for (uint32_t h = 0; h < num_hops; ++h) {
        uint32_t hop_start = h * HOP_SIZE;
        float hop_energy = 0.0f;

        // 1st-order pre-emphasis and micro-hop energy accumulation
        for (uint32_t i = 0; i < HOP_SIZE; ++i) {
            int16_t raw = pcm_samples[hop_start + i];
            float hp = static_cast<float>(raw) - config_.pre_emphasis_alpha * static_cast<float>(prev_raw_sample_);
            prev_raw_sample_ = raw;
            hop_energy += std::abs(hp);
        }

        // Half-wave rectified novelty
        float novelty = (hop_energy > prev_hop_energy_) ? (hop_energy - prev_hop_energy_) : 0.0f;
        prev_hop_energy_ = hop_energy;

        // Compute rolling mean and variance
        float sum = 0.0f;
        uint32_t n = (history_count_ < ROLLING_HOPS) ? history_count_ : ROLLING_HOPS;
        for (uint32_t i = 0; i < n; ++i) {
            sum += energy_history_[i];
        }
        float mean = (n > 0) ? (sum / static_cast<float>(n)) : 0.0f;

        float var_sum = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            float diff = energy_history_[i] - mean;
            var_sum += diff * diff;
        }
        float stddev = (n > 0) ? std::sqrt(var_sum / static_cast<float>(n)) : 0.0f;

        // Push into circular history
        energy_history_[history_idx_] = novelty;
        history_idx_ = (history_idx_ + 1) % ROLLING_HOPS;
        if (history_count_ < ROLLING_HOPS) {
            history_count_++;
        }

        float threshold = mean + k_sens * stddev + 500.0f; // 500 noise floor

        if (novelty > threshold && novelty > 2000.0f) {
            uint32_t global_hop_sample = total_samples_processed_ + hop_start;
            if (!has_previous_transient_ || (global_hop_sample - last_transient_sample_ >= config_.min_slice_samples)) {
                // Find zero crossing backward within [hop_start, hop_start + HOP_SIZE]
                uint32_t local_zc = find_zero_crossing_backward(pcm_samples, hop_start, 64);
                uint32_t snapped_sample = total_samples_processed_ + local_zc;

                out_transient_indices[detected_count++] = snapped_sample;
                last_transient_sample_ = snapped_sample;
                has_previous_transient_ = true;

                if (detected_count >= max_indices) {
                    break;
                }
            }
        }
    }

    total_samples_processed_ += count;
    return detected_count;
}

} // namespace audio_codecs::slice
```

Update `CMakeLists.txt` to compile `audio_codecs_slice` and `test_slice_detector`:
```cmake
add_library(audio_codecs_slice STATIC
    src/slice/slice_detector.cpp
)
target_include_directories(audio_codecs_slice PUBLIC include)

add_executable(test_slice_detector tests/test_slice_detector.cpp)
target_link_libraries(test_slice_detector PRIVATE audio_codecs_slice)
add_test(NAME SliceDetectorTest COMMAND test_slice_detector)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake -B build && cmake --build build --target test_slice_detector && ctest --test-dir build -R SliceDetectorTest --output-on-failure`
Expected: PASS (`test_slice_detector PASSED`).

- [ ] **Step 5: Commit**

```bash
git add include/audio_codecs/slice/slice_detector.h src/slice/slice_detector.cpp tests/test_slice_detector.cpp CMakeLists.txt
git commit -m "feat(slice): implement MCU-friendly time-domain SliceDetector with zero-crossing search"
```

---

### Task 3: Multi-Mode Slicing Engine & Musical Grid Quantization (slice_generator.h/.cpp)

**Files:**
- Create: `include/audio_codecs/slice/slice_generator.h`
- Create: `src/slice/slice_generator.cpp`
- Test: `tests/test_slice_modes.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `slice_types.h`, `slice_detector.h`
- Produces: `audio_codecs::slice::SliceGenerator`, `GeneratorConfig`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_slice_modes.cpp`:
```cpp
#include <audio_codecs/slice/slice_generator.h>
#include <cassert>
#include <vector>
#include <iostream>

using namespace audio_codecs::slice;

void test_pure_transient_mode() {
    SliceGenerator gen;
    GeneratorConfig cfg;
    cfg.mode = ASL_MODE_TRANSIENT;
    cfg.sample_rate = 44100;
    cfg.bpm_q16 = 120 << 16;
    bool ok = gen.init(cfg);
    assert(ok);

    // 2-second audio (88200 samples) with transients at 0.25s, 0.5s, 1.0s
    std::vector<int16_t> pcm(88200, 0);
    pcm[11025] = 25000; pcm[11026] = -20000;
    pcm[22050] = 25000; pcm[22051] = -20000;
    pcm[44100] = 25000; pcm[44101] = -20000;

    ok = gen.process_pcm(pcm.data(), static_cast<uint32_t>(pcm.size()));
    assert(ok);
    ok = gen.finalize();
    assert(ok);

    assert(gen.slice_count() >= 3);
    const AslSlice* s0 = gen.get_slice(0);
    assert(s0 != nullptr);
    assert(s0->start_sample <= 11030);
    assert(s0->midi_note == 36); // First slice is C1
}

void test_metric_grid_mode() {
    SliceGenerator gen;
    GeneratorConfig cfg;
    cfg.mode = ASL_MODE_GRID;
    cfg.sample_rate = 44100;
    cfg.bpm_q16 = 120 << 16; // 120 BPM -> 0.5s per quarter note, 0.125s per 16th note (5512.5 samples)
    cfg.time_sig_num = 4;
    cfg.time_sig_denom = 4;
    bool ok = gen.init(cfg);
    assert(ok);

    // 1 bar of silence = 2 seconds = 88200 samples
    std::vector<int16_t> silence(88200, 0);
    ok = gen.process_pcm(silence.data(), static_cast<uint32_t>(silence.size()));
    assert(ok);
    ok = gen.finalize();
    assert(ok);

    // 1 bar of 4/4 has 16 sixteenth-notes
    assert(gen.slice_count() == 16);
    for (uint16_t i = 0; i < 16; ++i) {
        const AslSlice* s = gen.get_slice(i);
        assert(s != nullptr);
        assert(s->bar_index == 0);
        assert(s->beat_within_bar == (i / 4));
        assert(s->subdivision == (i % 4));
        assert(s->musical_tick == i * 120); // 120 ticks per 16th note at PPQN=480
        assert(s->midi_note == static_cast<uint8_t>(36 + i));
    }
}

void test_transient_to_grid_quantization() {
    SliceGenerator gen;
    GeneratorConfig cfg;
    cfg.mode = ASL_MODE_TRANSIENT_TO_GRID;
    cfg.sample_rate = 44100;
    cfg.bpm_q16 = 120 << 16;
    cfg.time_sig_num = 4;
    cfg.time_sig_denom = 4;
    bool ok = gen.init(cfg);
    assert(ok);

    // 1 bar = 88200 samples. Kick at 0, Snare at beat 2 (44100 samples) with slight human jitter (+100 samples)
    std::vector<int16_t> pcm(88200, 0);
    pcm[0] = 20000; pcm[1] = -15000;
    pcm[44200] = 25000; pcm[44201] = -20000;

    ok = gen.process_pcm(pcm.data(), static_cast<uint32_t>(pcm.size()));
    assert(ok);
    ok = gen.finalize();
    assert(ok);

    assert(gen.slice_count() >= 2);
    const AslSlice* s0 = gen.get_slice(0);
    assert(s0->musical_tick == 0);
    assert(s0->bar_index == 0);
    assert(s0->beat_within_bar == 0);

    // Find the slice corresponding to beat 2 (snare)
    const AslSlice* s_snare = nullptr;
    for (uint16_t i = 0; i < gen.slice_count(); ++i) {
        const AslSlice* s = gen.get_slice(i);
        if (s->start_sample >= 44100 && s->start_sample <= 44300) {
            s_snare = s;
            break;
        }
    }
    assert(s_snare != nullptr);
    // Snapped to beat 2: 2 beats * 480 = 960 ticks
    assert(s_snare->musical_tick == 960);
    assert(s_snare->beat_within_bar == 2);
}

int main() {
    test_pure_transient_mode();
    test_metric_grid_mode();
    test_transient_to_grid_quantization();
    std::cout << "test_slice_modes PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake -B build && cmake --build build --target test_slice_modes`
Expected: Build failure due to missing `audio_codecs/slice/slice_generator.h`.

- [ ] **Step 3: Write minimal implementation**

Create `include/audio_codecs/slice/slice_generator.h`:
```cpp
#pragma once

#include <audio_codecs/slice/slice_types.h>
#include <audio_codecs/slice/slice_detector.h>
#include <cstdint>
#include <cstddef>

namespace audio_codecs::slice {

struct GeneratorConfig {
    AslSliceMode mode = ASL_MODE_TRANSIENT_TO_GRID;
    uint32_t sample_rate = 44100;
    uint8_t channels = 2;
    uint8_t time_sig_num = 4;
    uint8_t time_sig_denom = 4;
    uint32_t bpm_q16 = (120 << 16);
    float sensitivity = 0.5f;
    uint16_t max_slices = 256;
};

class SliceGenerator {
public:
    SliceGenerator();
    bool init(const GeneratorConfig& config);
    void reset();

    bool process_pcm(const int16_t* pcm, uint32_t sample_count);
    bool finalize();

    // Sliced cooperative API
    bool step(uint32_t budget_us);
    bool is_complete() const;

    uint16_t slice_count() const;
    const AslSlice* get_slice(uint16_t index) const;
    const AslHeader& header() const;

    // Serialize to pre-allocated buffer
    bool serialize(void* out_buffer, size_t buffer_size, size_t* out_bytes_written) const;

private:
    void generate_grid_slices();
    void quantize_transients_to_grid();

    GeneratorConfig config_{};
    SliceDetector detector_;
    AslHeader header_{};
    
    static constexpr uint16_t MAX_CAPACITY = 256;
    AslSlice slices_[MAX_CAPACITY]{};
    uint16_t slice_count_ = 0;

    uint32_t raw_transients_[MAX_CAPACITY]{};
    uint16_t raw_transient_count_ = 0;
    uint32_t total_samples_ = 0;
    bool finalized_ = false;
};

} // namespace audio_codecs::slice
```

Create `src/slice/slice_generator.cpp`:
```cpp
#include <audio_codecs/slice/slice_generator.h>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace audio_codecs::slice {

SliceGenerator::SliceGenerator() {
    reset();
}

bool SliceGenerator::init(const GeneratorConfig& config) {
    config_ = config;
    reset();

    DetectorConfig det_cfg;
    det_cfg.sample_rate = config.sample_rate;
    det_cfg.sensitivity = config.sensitivity;
    det_cfg.min_slice_samples = 512;
    det_cfg.pre_emphasis_alpha = 0.95f;
    detector_.init(det_cfg);

    header_.magic = ASL_MAGIC;
    header_.version = ASL_VERSION;
    header_.header_size = 128;
    header_.sample_rate = config.sample_rate;
    header_.channels = config.channels;
    header_.time_signature_num = config.time_sig_num;
    header_.time_signature_denom = config.time_sig_denom;
    header_.slice_mode = config.mode;
    header_.original_bpm_q16 = config.bpm_q16;
    header_.ppqn = ASL_PPQN;
    header_.slice_descriptor_size = sizeof(AslSlice);
    header_.slices_offset = 128;

    return true;
}

void SliceGenerator::reset() {
    detector_.reset();
    std::memset(&header_, 0, sizeof(header_));
    std::memset(slices_, 0, sizeof(slices_));
    slice_count_ = 0;
    raw_transient_count_ = 0;
    total_samples_ = 0;
    finalized_ = false;
}

bool SliceGenerator::process_pcm(const int16_t* pcm, uint32_t sample_count) {
    if (pcm == nullptr || sample_count == 0) {
        return false;
    }

    uint32_t detected[32];
    uint32_t count = detector_.process_block(pcm, sample_count, detected, 32);

    for (uint32_t i = 0; i < count; ++i) {
        if (raw_transient_count_ < MAX_CAPACITY) {
            raw_transients_[raw_transient_count_++] = detected[i];
        }
    }

    total_samples_ += sample_count;
    return true;
}

void SliceGenerator::generate_grid_slices() {
    double bpm = static_cast<double>(config_.bpm_q16) / 65536.0;
    if (bpm <= 0.0) bpm = 120.0;

    // 16th note subdivision: 4 divisions per beat
    double seconds_per_beat = 60.0 / bpm;
    double seconds_per_16th = seconds_per_beat / 4.0;
    double samples_per_16th = seconds_per_16th * config_.sample_rate;

    uint32_t total_16ths = static_cast<uint32_t>(total_samples_ / samples_per_16th);
    if (total_16ths > config_.max_slices) total_16ths = config_.max_slices;
    if (total_16ths > MAX_CAPACITY) total_16ths = MAX_CAPACITY;

    slice_count_ = static_cast<uint16_t>(total_16ths);
    uint32_t ticks_per_bar = ASL_PPQN * 4 * config_.time_sig_num / config_.time_sig_denom;

    for (uint16_t i = 0; i < slice_count_; ++i) {
        AslSlice& s = slices_[i];
        s.start_sample = static_cast<uint32_t>(i * samples_per_16th);
        if (i + 1 < slice_count_) {
            s.length_samples = static_cast<uint32_t>((i + 1) * samples_per_16th) - s.start_sample;
        } else {
            s.length_samples = total_samples_ - s.start_sample;
        }

        s.musical_tick = i * 120; // 120 ticks per 16th note at PPQN=480
        s.midi_note = static_cast<uint8_t>(36 + (i % 64));
        s.bar_index = static_cast<uint8_t>(s.musical_tick / ticks_per_bar);
        s.beat_within_bar = static_cast<uint8_t>((s.musical_tick % ticks_per_bar) / ASL_PPQN);
        s.subdivision = static_cast<uint8_t>((s.musical_tick % ASL_PPQN) / 120);
        s.decay_ms = static_cast<uint16_t>((s.length_samples * 1000) / config_.sample_rate);
        s.tail_mode = ASL_TAIL_STRETCH_DECAY;
    }
}

void SliceGenerator::quantize_transients_to_grid() {
    double bpm = static_cast<double>(config_.bpm_q16) / 65536.0;
    if (bpm <= 0.0) bpm = 120.0;

    double ticks_per_sample = (bpm * ASL_PPQN) / (60.0 * config_.sample_rate);
    uint32_t ticks_per_bar = ASL_PPQN * 4 * config_.time_sig_num / config_.time_sig_denom;

    slice_count_ = 0;

    // Ensure slice 0 starts at sample 0 if first transient is after 0
    if (raw_transient_count_ > 0 && raw_transients_[0] > 0) {
        AslSlice& s0 = slices_[slice_count_++];
        s0.start_sample = 0;
        s0.musical_tick = 0;
        s0.midi_note = 36;
        s0.bar_index = 0;
        s0.beat_within_bar = 0;
        s0.subdivision = 0;
        s0.tail_mode = ASL_TAIL_STRETCH_DECAY;
    }

    for (uint16_t i = 0; i < raw_transient_count_ && slice_count_ < config_.max_slices && slice_count_ < MAX_CAPACITY; ++i) {
        uint32_t pos = raw_transients_[i];
        if (pos == 0 && slice_count_ > 0) continue; // Already added

        AslSlice& s = slices_[slice_count_++];
        s.start_sample = pos;
        
        // Exact musical tick
        double exact_tick = static_cast<double>(pos) * ticks_per_sample;
        // Snap to nearest 16th note (120 ticks) within +/- 60 ticks
        uint32_t nearest_16th = static_cast<uint32_t>((exact_tick + 60.0) / 120.0) * 120;
        s.musical_tick = nearest_16th;
        s.midi_note = static_cast<uint8_t>(36 + ((slice_count_ - 1) % 64));
        s.bar_index = static_cast<uint8_t>(s.musical_tick / ticks_per_bar);
        s.beat_within_bar = static_cast<uint8_t>((s.musical_tick % ticks_per_bar) / ASL_PPQN);
        s.subdivision = static_cast<uint8_t>((s.musical_tick % ASL_PPQN) / 120);
        s.tail_mode = ASL_TAIL_STRETCH_DECAY;
    }

    // Compute slice lengths
    for (uint16_t i = 0; i < slice_count_; ++i) {
        if (i + 1 < slice_count_) {
            slices_[i].length_samples = slices_[i + 1].start_sample - slices_[i].start_sample;
        } else {
            slices_[i].length_samples = total_samples_ - slices_[i].start_sample;
        }
        slices_[i].decay_ms = static_cast<uint16_t>((slices_[i].length_samples * 1000) / config_.sample_rate);
    }
}

bool SliceGenerator::finalize() {
    if (finalized_) return true;

    if (config_.mode == ASL_MODE_GRID) {
        generate_grid_slices();
    } else if (config_.mode == ASL_MODE_TRANSIENT_TO_GRID) {
        quantize_transients_to_grid();
    } else {
        // Pure Transient Mode
        slice_count_ = 0;
        for (uint16_t i = 0; i < raw_transient_count_ && slice_count_ < config_.max_slices && slice_count_ < MAX_CAPACITY; ++i) {
            AslSlice& s = slices_[slice_count_++];
            s.start_sample = raw_transients_[i];
            s.midi_note = static_cast<uint8_t>(36 + ((slice_count_ - 1) % 64));
            s.tail_mode = ASL_TAIL_ONE_SHOT;
        }
        // Slice lengths
        for (uint16_t i = 0; i < slice_count_; ++i) {
            if (i + 1 < slice_count_) {
                slices_[i].length_samples = slices_[i + 1].start_sample - slices_[i].start_sample;
            } else {
                slices_[i].length_samples = total_samples_ - slices_[i].start_sample;
            }
            slices_[i].decay_ms = static_cast<uint16_t>((slices_[i].length_samples * 1000) / config_.sample_rate);
        }
    }

    header_.duration_samples = total_samples_;
    header_.duration_ms = static_cast<uint32_t>((static_cast<uint64_t>(total_samples_) * 1000) / config_.sample_rate);
    header_.total_slices = slice_count_;

    uint32_t ticks_per_bar = ASL_PPQN * 4 * config_.time_sig_num / config_.time_sig_denom;
    if (slice_count_ > 0 && ticks_per_bar > 0) {
        header_.num_bars = static_cast<uint16_t>((slices_[slice_count_ - 1].musical_tick / ticks_per_bar) + 1);
    }

    finalized_ = true;
    return true;
}

bool SliceGenerator::step(uint32_t /*budget_us*/) {
    if (!finalized_) {
        finalize();
    }
    return true;
}

bool SliceGenerator::is_complete() const {
    return finalized_;
}

uint16_t SliceGenerator::slice_count() const {
    return slice_count_;
}

const AslSlice* SliceGenerator::get_slice(uint16_t index) const {
    if (index >= slice_count_) return nullptr;
    return &slices_[index];
}

const AslHeader& SliceGenerator::header() const {
    return header_;
}

bool SliceGenerator::serialize(void* out_buffer, size_t buffer_size, size_t* out_bytes_written) const {
    if (out_buffer == nullptr || !finalized_) return false;

    size_t total_size = sizeof(AslHeader) + slice_count_ * sizeof(AslSlice);
    if (buffer_size < total_size) return false;

    uint8_t* ptr = static_cast<uint8_t*>(out_buffer);
    std::memcpy(ptr, &header_, sizeof(AslHeader));
    ptr += sizeof(AslHeader);

    std::memcpy(ptr, slices_, slice_count_ * sizeof(AslSlice));

    if (out_bytes_written != nullptr) {
        *out_bytes_written = total_size;
    }
    return true;
}

} // namespace audio_codecs::slice
```

Update `CMakeLists.txt` to include `src/slice/slice_generator.cpp` in `audio_codecs_slice` and add `test_slice_modes`:
```cmake
add_library(audio_codecs_slice STATIC
    src/slice/slice_detector.cpp
    src/slice/slice_generator.cpp
)
target_include_directories(audio_codecs_slice PUBLIC include)

add_executable(test_slice_modes tests/test_slice_modes.cpp)
target_link_libraries(test_slice_modes PRIVATE audio_codecs_slice)
add_test(NAME SliceModesTest COMMAND test_slice_modes)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake -B build && cmake --build build --target test_slice_modes && ctest --test-dir build -R SliceModesTest --output-on-failure`
Expected: PASS (`test_slice_modes PASSED`).

- [ ] **Step 5: Commit**

```bash
git add include/audio_codecs/slice/slice_generator.h src/slice/slice_generator.cpp tests/test_slice_modes.cpp CMakeLists.txt
git commit -m "feat(slice): implement SliceGenerator with transient, grid, and quantized modes"
```

---

### Task 4: Cooperative Generator Pipeline & Serialization (slice_generator.h/.cpp finalized)

**Files:**
- Modify: `include/audio_codecs/slice/slice_generator.h`
- Modify: `src/slice/slice_generator.cpp`
- Test: `tests/test_slice_generator.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `slice_types.h`, `slice_generator.h`
- Produces: Cooperative chunk streaming `step(budget_us)`, buffer overflow protection, memory serialization.

- [ ] **Step 1: Write the failing test**

Create `tests/test_slice_generator.cpp`:
```cpp
#include <audio_codecs/slice/slice_generator.h>
#include <cassert>
#include <vector>
#include <cstring>
#include <iostream>

using namespace audio_codecs::slice;

void test_generator_cooperative_stepping() {
    SliceGenerator gen;
    GeneratorConfig cfg;
    cfg.mode = ASL_MODE_GRID;
    cfg.sample_rate = 44100;
    cfg.bpm_q16 = 120 << 16;
    bool ok = gen.init(cfg);
    assert(ok);

    std::vector<int16_t> silence(44100, 0); // 1 sec
    ok = gen.process_pcm(silence.data(), static_cast<uint32_t>(silence.size()));
    assert(ok);

    assert(!gen.is_complete());
    ok = gen.step(500); // 500 us budget
    assert(ok);
    assert(gen.is_complete());
}

void test_generator_buffer_saturation() {
    SliceGenerator gen;
    GeneratorConfig cfg;
    cfg.mode = ASL_MODE_TRANSIENT;
    cfg.max_slices = 10;
    bool ok = gen.init(cfg);
    assert(ok);

    // Feed 30 impulses
    std::vector<int16_t> pcm(44100, 0);
    for (int i = 0; i < 30; ++i) {
        int idx = i * 1000;
        pcm[idx] = 25000;
        pcm[idx + 1] = -20000;
    }

    ok = gen.process_pcm(pcm.data(), static_cast<uint32_t>(pcm.size()));
    assert(ok);
    ok = gen.finalize();
    assert(ok);

    // Max slices must be bounded at 10
    assert(gen.slice_count() <= 10);
}

void test_generator_serialization() {
    SliceGenerator gen;
    GeneratorConfig cfg;
    cfg.mode = ASL_MODE_GRID;
    cfg.sample_rate = 44100;
    cfg.bpm_q16 = 120 << 16;
    bool ok = gen.init(cfg);
    assert(ok);

    std::vector<int16_t> silence(44100, 0); // 8 sixteenth-notes
    ok = gen.process_pcm(silence.data(), static_cast<uint32_t>(silence.size()));
    assert(ok);
    ok = gen.finalize();
    assert(ok);

    uint8_t buffer[2048];
    size_t written = 0;
    ok = gen.serialize(buffer, sizeof(buffer), &written);
    assert(ok);
    assert(written == sizeof(AslHeader) + gen.slice_count() * sizeof(AslSlice));

    const AslHeader* hdr = reinterpret_cast<const AslHeader*>(buffer);
    assert(hdr->magic == ASL_MAGIC);
    assert(hdr->version == ASL_VERSION);
    assert(hdr->total_slices == gen.slice_count());
    assert(hdr->slices_offset == 128);
}

int main() {
    test_generator_cooperative_stepping();
    test_generator_buffer_saturation();
    test_generator_serialization();
    std::cout << "test_slice_generator PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it passes or fails**

Add `test_slice_generator` to `CMakeLists.txt`:
```cmake
add_executable(test_slice_generator tests/test_slice_generator.cpp)
target_link_libraries(test_slice_generator PRIVATE audio_codecs_slice)
add_test(NAME SliceGeneratorTest COMMAND test_slice_generator)
```
Run: `cmake -B build && cmake --build build --target test_slice_generator && ctest --test-dir build -R SliceGeneratorTest --output-on-failure`
Expected: PASS.

- [ ] **Step 3: Refine minimal implementation if needed**

Ensure bounds checks in `SliceGenerator::process_pcm` and `SliceGenerator::finalize` strictly respect `config_.max_slices` and `MAX_CAPACITY`.

- [ ] **Step 4: Re-verify tests pass**

Run: `ctest --test-dir build -R SliceGeneratorTest --output-on-failure`
Expected: PASS (`test_slice_generator PASSED`).

- [ ] **Step 5: Commit**

```bash
git add tests/test_slice_generator.cpp CMakeLists.txt
git commit -m "feat(slice): verify cooperative generator stepping, saturation limits, and serialization"
```

---

### Task 5: Zero-Heap Random-Access Reader (slice_reader.h/.cpp)

**Files:**
- Create: `include/audio_codecs/slice/slice_reader.h`
- Create: `src/slice/slice_reader.cpp`
- Test: `tests/test_slice_reader.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `slice_types.h`
- Produces: `audio_codecs::slice::SliceReader`, `get_slice()`, `find_slice_at_sample()`, `find_slice_at_ms()`, `find_slice_at_tick()`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_slice_reader.cpp`:
```cpp
#include <audio_codecs/slice/slice_reader.h>
#include <audio_codecs/slice/slice_generator.h>
#include <cassert>
#include <vector>
#include <iostream>

using namespace audio_codecs::slice;

void test_reader_init_and_validation() {
    SliceReader reader;
    // Reject nullptr and too small buffer
    bool ok = reader.init(nullptr, 0);
    assert(!ok);

    uint8_t garbage[64] = {0};
    ok = reader.init(garbage, sizeof(garbage));
    assert(!ok);
}

void test_reader_lookups() {
    SliceGenerator gen;
    GeneratorConfig cfg;
    cfg.mode = ASL_MODE_GRID;
    cfg.sample_rate = 44100;
    cfg.bpm_q16 = 120 << 16;
    bool ok = gen.init(cfg);
    assert(ok);

    std::vector<int16_t> silence(88200, 0); // 2 seconds, 16 sixteenths
    ok = gen.process_pcm(silence.data(), static_cast<uint32_t>(silence.size()));
    assert(ok);
    ok = gen.finalize();
    assert(ok);

    uint8_t buffer[2048];
    size_t written = 0;
    ok = gen.serialize(buffer, sizeof(buffer), &written);
    assert(ok);

    SliceReader reader;
    ok = reader.init(buffer, written);
    assert(ok);

    assert(reader.total_slices() == 16);
    assert(reader.header().sample_rate == 44100);

    // O(1) indexed lookup
    const AslSlice* s0 = reader.get_slice(0);
    assert(s0 != nullptr && s0->start_sample == 0);
    const AslSlice* s_invalid = reader.get_slice(100);
    assert(s_invalid == nullptr);

    // O(log N) binary search by sample
    const AslSlice* s_sample = reader.find_slice_at_sample(6000); // 16th note 1 starts at 5512
    assert(s_sample != nullptr);
    assert(s_sample->subdivision == 1);

    // O(log N) binary search by ms
    const AslSlice* s_ms = reader.find_slice_at_ms(550); // ~0.55s is beat 1, subdivision 0 (starts at 500ms)
    assert(s_ms != nullptr);
    assert(s_ms->beat_within_bar == 1);

    // O(log N) binary search by musical tick
    const AslSlice* s_tick = reader.find_slice_at_tick(250); // 240 is 16th note 2
    assert(s_tick != nullptr);
    assert(s_tick->musical_tick == 240);
}

int main() {
    test_reader_init_and_validation();
    test_reader_lookups();
    std::cout << "test_slice_reader PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake -B build && cmake --build build --target test_slice_reader`
Expected: Build failure due to missing `audio_codecs/slice/slice_reader.h`.

- [ ] **Step 3: Write minimal implementation**

Create `include/audio_codecs/slice/slice_reader.h`:
```cpp
#pragma once

#include <audio_codecs/slice/slice_types.h>
#include <cstdint>
#include <cstddef>

namespace audio_codecs::slice {

class SliceReader {
public:
    SliceReader();
    bool init(const uint8_t* asl_data, size_t asl_size);

    const AslHeader& header() const;
    uint16_t total_slices() const;

    // O(1) direct slice access
    const AslSlice* get_slice(uint16_t index) const;

    // O(log N) binary search queries
    const AslSlice* find_slice_at_sample(uint32_t sample_offset) const;
    const AslSlice* find_slice_at_ms(uint32_t ms) const;
    const AslSlice* find_slice_at_tick(uint32_t tick) const;

private:
    const AslHeader* header_ = nullptr;
    const AslSlice* slices_ = nullptr;
    uint16_t total_slices_ = 0;
    size_t data_size_ = 0;
};

} // namespace audio_codecs::slice
```

Create `src/slice/slice_reader.cpp`:
```cpp
#include <audio_codecs/slice/slice_reader.h>
#include <algorithm>

namespace audio_codecs::slice {

SliceReader::SliceReader() = default;

bool SliceReader::init(const uint8_t* asl_data, size_t asl_size) {
    if (asl_data == nullptr || asl_size < sizeof(AslHeader)) {
        return false;
    }

    const auto* hdr = reinterpret_cast<const AslHeader*>(asl_data);
    if (hdr->magic != ASL_MAGIC || hdr->version != ASL_VERSION || hdr->header_size != 128) {
        return false;
    }

    size_t required_size = hdr->slices_offset + hdr->total_slices * sizeof(AslSlice);
    if (asl_size < required_size) {
        return false;
    }

    header_ = hdr;
    slices_ = reinterpret_cast<const AslSlice*>(asl_data + hdr->slices_offset);
    total_slices_ = hdr->total_slices;
    data_size_ = asl_size;

    return true;
}

const AslHeader& SliceReader::header() const {
    static const AslHeader empty_header{};
    return (header_ != nullptr) ? *header_ : empty_header;
}

uint16_t SliceReader::total_slices() const {
    return total_slices_;
}

const AslSlice* SliceReader::get_slice(uint16_t index) const {
    if (slices_ == nullptr || index >= total_slices_) {
        return nullptr;
    }
    return &slices_[index];
}

const AslSlice* SliceReader::find_slice_at_sample(uint32_t sample_offset) const {
    if (slices_ == nullptr || total_slices_ == 0) {
        return nullptr;
    }

    // Binary search: upper_bound on start_sample
    int low = 0;
    int high = static_cast<int>(total_slices_) - 1;
    int best = -1;

    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (slices_[mid].start_sample <= sample_offset) {
            best = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    if (best >= 0) {
        const AslSlice& s = slices_[best];
        if (sample_offset < s.start_sample + s.length_samples) {
            return &s;
        }
    }
    return nullptr;
}

const AslSlice* SliceReader::find_slice_at_ms(uint32_t ms) const {
    if (header_ == nullptr || header_->sample_rate == 0) return nullptr;
    uint32_t sample_offset = static_cast<uint32_t>((static_cast<uint64_t>(ms) * header_->sample_rate) / 1000);
    return find_slice_at_sample(sample_offset);
}

const AslSlice* SliceReader::find_slice_at_tick(uint32_t tick) const {
    if (slices_ == nullptr || total_slices_ == 0) {
        return nullptr;
    }

    int low = 0;
    int high = static_cast<int>(total_slices_) - 1;
    int best = -1;

    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (slices_[mid].musical_tick <= tick) {
            best = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    return (best >= 0) ? &slices_[best] : nullptr;
}

} // namespace audio_codecs::slice
```

Update `CMakeLists.txt` to add `src/slice/slice_reader.cpp` to `audio_codecs_slice` and `test_slice_reader`:
```cmake
add_library(audio_codecs_slice STATIC
    src/slice/slice_detector.cpp
    src/slice/slice_generator.cpp
    src/slice/slice_reader.cpp
)
target_include_directories(audio_codecs_slice PUBLIC include)

add_executable(test_slice_reader tests/test_slice_reader.cpp)
target_link_libraries(test_slice_reader PRIVATE audio_codecs_slice)
add_test(NAME SliceReaderTest COMMAND test_slice_reader)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake -B build && cmake --build build --target test_slice_reader && ctest --test-dir build -R SliceReaderTest --output-on-failure`
Expected: PASS (`test_slice_reader PASSED`).

- [ ] **Step 5: Commit**

```bash
git add include/audio_codecs/slice/slice_reader.h src/slice/slice_reader.cpp tests/test_slice_reader.cpp CMakeLists.txt
git commit -m "feat(slice): implement zero-heap SliceReader with O(log N) binary searches"
```

---

### Task 6: Playback Voice Helper & Type 0 MIDI Exporter (slice_playback.h/.cpp, slice_midi_writer.h/.cpp)

**Files:**
- Create: `include/audio_codecs/slice/slice_playback.h`
- Create: `src/slice/slice_playback.cpp`
- Create: `include/audio_codecs/slice/slice_midi_writer.h`
- Create: `src/slice/slice_midi_writer.cpp`
- Test: `tests/test_slice_playback.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `slice_types.h`
- Produces: `audio_codecs::slice::SliceVoiceHelper`, `SliceMidiWriter::write_type0_midi`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_slice_playback.cpp`:
```cpp
#include <audio_codecs/slice/slice_playback.h>
#include <audio_codecs/slice/slice_midi_writer.h>
#include <cassert>
#include <vector>
#include <cstring>
#include <iostream>

using namespace audio_codecs::slice;

void test_playback_tempo_math() {
    AslSlice slice{};
    slice.start_sample = 0;
    slice.length_samples = 22050; // 0.5s at 44.1k (quarter note at 120 BPM)
    slice.musical_tick = 0;

    uint32_t trigger_interval = 0;
    uint32_t render_samples = 0;
    bool needs_tail = false;

    // Faster tempo: 140 BPM
    SliceVoiceHelper::compute_playback_params(slice, 120 << 16, 140 << 16, 44100,
                                             &trigger_interval, &render_samples, &needs_tail);
    assert(trigger_interval < 22050);
    assert(!needs_tail);

    // Slower tempo: 100 BPM
    SliceVoiceHelper::compute_playback_params(slice, 120 << 16, 100 << 16, 44100,
                                             &trigger_interval, &render_samples, &needs_tail);
    assert(trigger_interval > 22050);
    assert(needs_tail);
}

void test_micro_fade_curves() {
    std::vector<int16_t> pcm(128, 10000);

    // Attack ramp: first sample should be attenuated near 0
    SliceVoiceHelper::apply_attack_ramp(pcm.data(), static_cast<uint32_t>(pcm.size()), 44100);
    assert(pcm[0] == 0);
    assert(pcm[127] == 10000);

    // Choke ramp: end sample should be attenuated near 0
    SliceVoiceHelper::apply_choke_ramp(pcm.data(), static_cast<uint32_t>(pcm.size()), 44100);
    assert(pcm[127] == 0);
}

void test_midi_type0_export() {
    AslHeader hdr{};
    hdr.magic = ASL_MAGIC;
    hdr.version = ASL_VERSION;
    hdr.header_size = 128;
    hdr.time_signature_num = 4;
    hdr.time_signature_denom = 4;
    hdr.original_bpm_q16 = 120 << 16;
    hdr.ppqn = 480;
    hdr.total_slices = 2;

    AslSlice slices[2]{};
    slices[0].start_sample = 0;
    slices[0].length_samples = 22050;
    slices[0].musical_tick = 0;
    slices[0].midi_note = 36;

    slices[1].start_sample = 22050;
    slices[1].length_samples = 22050;
    slices[1].musical_tick = 480;
    slices[1].midi_note = 37;

    uint8_t midi_buf[512];
    size_t written = 0;
    bool ok = SliceMidiWriter::write_type0_midi(hdr, slices, 2, midi_buf, sizeof(midi_buf), &written);
    assert(ok);
    assert(written > 32);

    // Verify MIDI Header: 'MThd', header size 6, format 0, 1 track, 480 PPQN
    assert(std::memcmp(midi_buf, "MThd", 4) == 0);
    assert(midi_buf[4] == 0 && midi_buf[5] == 0 && midi_buf[6] == 0 && midi_buf[7] == 6);
    assert(midi_buf[8] == 0 && midi_buf[9] == 0); // Format 0
    assert(midi_buf[10] == 0 && midi_buf[11] == 1); // 1 track
    uint16_t ppqn = (static_cast<uint16_t>(midi_buf[12]) << 8) | midi_buf[13];
    assert(ppqn == 480);

    // Verify Track Chunk: 'MTrk'
    assert(std::memcmp(midi_buf + 14, "MTrk", 4) == 0);
}

int main() {
    test_playback_tempo_math();
    test_micro_fade_curves();
    test_midi_type0_export();
    std::cout << "test_slice_playback PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake -B build && cmake --build build --target test_slice_playback`
Expected: Build failure due to missing playback and midi headers.

- [ ] **Step 3: Write minimal implementation**

Create `include/audio_codecs/slice/slice_playback.h`:
```cpp
#pragma once

#include <audio_codecs/slice/slice_types.h>
#include <cstdint>
#include <cstddef>

namespace audio_codecs::slice {

class SliceVoiceHelper {
public:
    static void compute_playback_params(const AslSlice& slice,
                                        uint32_t original_bpm_q16,
                                        uint32_t target_bpm_q16,
                                        uint32_t sample_rate,
                                        uint32_t* out_trigger_interval_samples,
                                        uint32_t* out_render_samples,
                                        bool* out_needs_tail_extension);

    static void apply_attack_ramp(int16_t* pcm, uint32_t count, uint32_t sample_rate);
    static void apply_choke_ramp(int16_t* pcm, uint32_t count, uint32_t sample_rate);
};

} // namespace audio_codecs::slice
```

Create `src/slice/slice_playback.cpp`:
```cpp
#include <audio_codecs/slice/slice_playback.h>
#include <algorithm>

namespace audio_codecs::slice {

void SliceVoiceHelper::compute_playback_params(const AslSlice& slice,
                                               uint32_t original_bpm_q16,
                                               uint32_t target_bpm_q16,
                                               uint32_t /*sample_rate*/,
                                               uint32_t* out_trigger_interval_samples,
                                               uint32_t* out_render_samples,
                                               bool* out_needs_tail_extension) {
    if (original_bpm_q16 == 0) original_bpm_q16 = (120 << 16);
    if (target_bpm_q16 == 0) target_bpm_q16 = original_bpm_q16;

    // trigger_interval = slice.length_samples * (original_bpm / target_bpm)
    uint64_t scaled = (static_cast<uint64_t>(slice.length_samples) * original_bpm_q16) / target_bpm_q16;
    uint32_t interval = static_cast<uint32_t>(scaled);

    if (out_trigger_interval_samples != nullptr) {
        *out_trigger_interval_samples = interval;
    }
    if (out_render_samples != nullptr) {
        *out_render_samples = (interval > slice.length_samples) ? interval : slice.length_samples;
    }
    if (out_needs_tail_extension != nullptr) {
        *out_needs_tail_extension = (interval > slice.length_samples);
    }
}

void SliceVoiceHelper::apply_attack_ramp(int16_t* pcm, uint32_t count, uint32_t sample_rate) {
    if (pcm == nullptr || count == 0 || sample_rate == 0) return;

    // 1.0 ms linear attack ramp
    uint32_t ramp_len = sample_rate / 1000;
    if (ramp_len > count) ramp_len = count;

    for (uint32_t i = 0; i < ramp_len; ++i) {
        float gain = static_cast<float>(i) / static_cast<float>(ramp_len);
        pcm[i] = static_cast<int16_t>(pcm[i] * gain);
    }
}

void SliceVoiceHelper::apply_choke_ramp(int16_t* pcm, uint32_t count, uint32_t sample_rate) {
    if (pcm == nullptr || count == 0 || sample_rate == 0) return;

    // 2.5 ms linear choke release ramp
    uint32_t ramp_len = (sample_rate * 25) / 10000; // 2.5 ms
    if (ramp_len > count) ramp_len = count;

    uint32_t start_idx = count - ramp_len;
    for (uint32_t i = 0; i < ramp_len; ++i) {
        float gain = 1.0f - (static_cast<float>(i + 1) / static_cast<float>(ramp_len));
        pcm[start_idx + i] = static_cast<int16_t>(pcm[start_idx + i] * gain);
    }
}

} // namespace audio_codecs::slice
```

Create `include/audio_codecs/slice/slice_midi_writer.h`:
```cpp
#pragma once

#include <audio_codecs/slice/slice_types.h>
#include <cstdint>
#include <cstddef>

namespace audio_codecs::slice {

class SliceMidiWriter {
public:
    static bool write_type0_midi(const AslHeader& header,
                                 const AslSlice* slices,
                                 uint16_t total_slices,
                                 uint8_t* out_midi_buf,
                                 size_t max_buf_size,
                                 size_t* out_bytes_written);

private:
    static size_t write_variable_length_quantity(uint32_t value, uint8_t* out_buf);
};

} // namespace audio_codecs::slice
```

Create `src/slice/slice_midi_writer.cpp`:
```cpp
#include <audio_codecs/slice/slice_midi_writer.h>
#include <cstring>

namespace audio_codecs::slice {

size_t SliceMidiWriter::write_variable_length_quantity(uint32_t value, uint8_t* out_buf) {
    uint8_t buffer[4];
    size_t count = 0;
    buffer[count++] = static_cast<uint8_t>(value & 0x7F);
    value >>= 7;

    while (value > 0) {
        buffer[count++] = static_cast<uint8_t>((value & 0x7F) | 0x80);
        value >>= 7;
    }

    for (size_t i = 0; i < count; ++i) {
        out_buf[i] = buffer[count - 1 - i];
    }
    return count;
}

bool SliceMidiWriter::write_type0_midi(const AslHeader& header,
                                       const AslSlice* slices,
                                       uint16_t total_slices,
                                       uint8_t* out_midi_buf,
                                       size_t max_buf_size,
                                       size_t* out_bytes_written) {
    if (slices == nullptr || total_slices == 0 || out_midi_buf == nullptr || max_buf_size < 128) {
        return false;
    }

    uint8_t* ptr = out_midi_buf;
    uint8_t* end = out_midi_buf + max_buf_size;

    // 1. MThd Chunk
    if (ptr + 14 > end) return false;
    std::memcpy(ptr, "MThd", 4); ptr += 4;
    *ptr++ = 0; *ptr++ = 0; *ptr++ = 0; *ptr++ = 6; // Header length = 6
    *ptr++ = 0; *ptr++ = 0;                         // Format 0
    *ptr++ = 0; *ptr++ = 1;                         // 1 Track
    *ptr++ = static_cast<uint8_t>((ASL_PPQN >> 8) & 0xFF);
    *ptr++ = static_cast<uint8_t>(ASL_PPQN & 0xFF);

    // 2. MTrk Chunk Header placeholder
    if (ptr + 8 > end) return false;
    std::memcpy(ptr, "MTrk", 4); ptr += 4;
    uint8_t* track_len_ptr = ptr;
    ptr += 4; // Skip length for now
    uint8_t* track_data_start = ptr;

    // Time signature: delta 0, FF 58 04 num denom 24 08
    if (ptr + 8 > end) return false;
    *ptr++ = 0x00; // Delta time 0
    *ptr++ = 0xFF; *ptr++ = 0x58; *ptr++ = 0x04;
    *ptr++ = header.time_signature_num ? header.time_signature_num : 4;
    uint8_t denom_pow = 2; // 4 = 2^2
    *ptr++ = denom_pow;
    *ptr++ = 24; // 24 MIDI clocks per quarter
    *ptr++ = 8;  // 8 32nd notes per quarter

    // Set Tempo: delta 0, FF 51 03 microsecs_per_quarter
    double bpm = (header.original_bpm_q16 > 0) ? (static_cast<double>(header.original_bpm_q16) / 65536.0) : 120.0;
    uint32_t us_per_quarter = static_cast<uint32_t>(60000000.0 / bpm);
    if (ptr + 7 > end) return false;
    *ptr++ = 0x00; // Delta time 0
    *ptr++ = 0xFF; *ptr++ = 0x51; *ptr++ = 0x03;
    *ptr++ = static_cast<uint8_t>((us_per_quarter >> 16) & 0xFF);
    *ptr++ = static_cast<uint8_t>((us_per_quarter >> 8) & 0xFF);
    *ptr++ = static_cast<uint8_t>(us_per_quarter & 0xFF);

    // Note events
    uint32_t current_tick = 0;
    for (uint16_t i = 0; i < total_slices; ++i) {
        const AslSlice& s = slices[i];
        uint32_t note_on_tick = s.musical_tick;
        uint32_t delta_on = (note_on_tick >= current_tick) ? (note_on_tick - current_tick) : 0;
        current_tick = note_on_tick;

        // Delta time for Note-On
        if (ptr + 8 > end) return false;
        ptr += write_variable_length_quantity(delta_on, ptr);

        // Note On: 0x90 note velocity
        *ptr++ = 0x90;
        *ptr++ = s.midi_note;
        *ptr++ = 100; // Default velocity

        // Note duration (approx 16th note = 120 ticks, or up to next slice)
        uint32_t note_dur = 120;
        if (i + 1 < total_slices && slices[i + 1].musical_tick > note_on_tick) {
            note_dur = slices[i + 1].musical_tick - note_on_tick;
            if (note_dur > 240) note_dur = 240; // Don't hold note excessively
        }

        // Delta time for Note-Off
        ptr += write_variable_length_quantity(note_dur, ptr);
        current_tick += note_dur;

        // Note Off: 0x80 note 0
        *ptr++ = 0x80;
        *ptr++ = s.midi_note;
        *ptr++ = 0x00;
    }

    // End of Track: delta 0, FF 2F 00
    if (ptr + 4 > end) return false;
    *ptr++ = 0x00;
    *ptr++ = 0xFF; *ptr++ = 0x2F; *ptr++ = 0x00;

    // Fill Track length (Big-Endian 32-bit)
    uint32_t track_len = static_cast<uint32_t>(ptr - track_data_start);
    track_len_ptr[0] = static_cast<uint8_t>((track_len >> 24) & 0xFF);
    track_len_ptr[1] = static_cast<uint8_t>((track_len >> 16) & 0xFF);
    track_len_ptr[2] = static_cast<uint8_t>((track_len >> 8) & 0xFF);
    track_len_ptr[3] = static_cast<uint8_t>(track_len & 0xFF);

    if (out_bytes_written != nullptr) {
        *out_bytes_written = static_cast<size_t>(ptr - out_midi_buf);
    }
    return true;
}

} // namespace audio_codecs::slice
```

Update `CMakeLists.txt` to add `src/slice/slice_playback.cpp` and `src/slice/slice_midi_writer.cpp` to `audio_codecs_slice`, and add `test_slice_playback`:
```cmake
add_library(audio_codecs_slice STATIC
    src/slice/slice_detector.cpp
    src/slice/slice_generator.cpp
    src/slice/slice_reader.cpp
    src/slice/slice_playback.cpp
    src/slice/slice_midi_writer.cpp
)
target_include_directories(audio_codecs_slice PUBLIC include)

add_executable(test_slice_playback tests/test_slice_playback.cpp)
target_link_libraries(test_slice_playback PRIVATE audio_codecs_slice)
add_test(NAME SlicePlaybackTest COMMAND test_slice_playback)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake -B build && cmake --build build --target test_slice_playback && ctest --test-dir build -R SlicePlaybackTest --output-on-failure`
Expected: PASS (`test_slice_playback PASSED`).

- [ ] **Step 5: Commit**

```bash
git add include/audio_codecs/slice/slice_playback.h src/slice/slice_playback.cpp include/audio_codecs/slice/slice_midi_writer.h src/slice/slice_midi_writer.cpp tests/test_slice_playback.cpp CMakeLists.txt
git commit -m "feat(slice): implement SliceVoiceHelper and Type 0 SliceMidiWriter"
```

---

### Task 7: Top-Level Integration, Umbrella Headers & End-to-End Suite (slice.h, audio_codecs.h, test_slice_integration.cpp)

**Files:**
- Create: `include/audio_codecs/slice/slice.h`
- Modify: `include/audio_codecs/audio_codecs.h`
- Test: `tests/test_slice_integration.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: All `include/audio_codecs/slice/*.h` headers.
- Produces: Integrated umbrella headers and complete end-to-end verification.

- [ ] **Step 1: Write the failing test**

Create `tests/test_slice_integration.cpp`:
```cpp
#include <audio_codecs/audio_codecs.h>
#include <cassert>
#include <vector>
#include <cmath>
#include <iostream>

using namespace audio_codecs::slice;

void test_end_to_end_synthetic_breakbeat() {
    // Generate 4 bars of 120 BPM drum break (4 bars * 2.0s = 8.0s @ 44.1k = 352800 samples)
    const uint32_t sample_rate = 44100;
    const uint32_t total_samples = sample_rate * 8;
    std::vector<int16_t> pcm(total_samples, 0);

    // Quarter note = 22050 samples.
    // Kick on beat 0 and 2 of each bar; Snare on beat 1 and 3 of each bar.
    for (int bar = 0; bar < 4; ++bar) {
        int bar_start = bar * (4 * 22050);
        // Kick beat 0
        pcm[bar_start + 0] = 28000; pcm[bar_start + 1] = -24000;
        // Snare beat 1
        pcm[bar_start + 22050] = 25000; pcm[bar_start + 22051] = -22000;
        // Kick beat 2
        pcm[bar_start + 44100] = 28000; pcm[bar_start + 44101] = -24000;
        // Snare beat 3
        pcm[bar_start + 66150] = 25000; pcm[bar_start + 66151] = -22000;
    }

    // Step 1: Generator in TRANSIENT_TO_GRID mode
    SliceGenerator generator;
    GeneratorConfig cfg;
    cfg.mode = ASL_MODE_TRANSIENT_TO_GRID;
    cfg.sample_rate = sample_rate;
    cfg.bpm_q16 = 120 << 16;
    cfg.time_sig_num = 4;
    cfg.time_sig_denom = 4;
    bool ok = generator.init(cfg);
    assert(ok);

    // Stream PCM in 4096-sample blocks
    const uint32_t block_size = 4096;
    for (uint32_t offset = 0; offset < total_samples; offset += block_size) {
        uint32_t chunk = std::min(block_size, total_samples - offset);
        ok = generator.process_pcm(pcm.data() + offset, chunk);
        assert(ok);
    }

    ok = generator.finalize();
    assert(ok);
    assert(generator.is_complete());

    // 4 bars * 4 beats = 16 drum hits
    assert(generator.slice_count() >= 16);

    // Step 2: Serialize to memory buffer (.asl)
    uint8_t asl_buffer[4096];
    size_t bytes_written = 0;
    ok = generator.serialize(asl_buffer, sizeof(asl_buffer), &bytes_written);
    assert(ok);
    assert(bytes_written > sizeof(AslHeader));

    // Step 3: Zero-heap reader queries
    SliceReader reader;
    ok = reader.init(asl_buffer, bytes_written);
    assert(ok);
    assert(reader.total_slices() == generator.slice_count());
    assert(reader.header().num_bars >= 4);

    // Verify slice lookups across all bars
    for (uint16_t i = 0; i < reader.total_slices(); ++i) {
        const AslSlice* s = reader.get_slice(i);
        assert(s != nullptr);
        assert(s->midi_note >= 36);
    }

    // Step 4: Playback voice helper tempo stretch params
    const AslSlice* first_slice = reader.get_slice(0);
    assert(first_slice != nullptr);
    uint32_t trigger_interval = 0;
    uint32_t render_samples = 0;
    bool needs_tail = false;
    SliceVoiceHelper::compute_playback_params(*first_slice, 120 << 16, 100 << 16, sample_rate,
                                             &trigger_interval, &render_samples, &needs_tail);
    assert(needs_tail);
    assert(trigger_interval > first_slice->length_samples);

    // Step 5: Export standard Type 0 MIDI file
    uint8_t midi_buf[2048];
    size_t midi_bytes = 0;
    ok = SliceMidiWriter::write_type0_midi(reader.header(), reader.get_slice(0),
                                          reader.total_slices(), midi_buf,
                                          sizeof(midi_buf), &midi_bytes);
    assert(ok);
    assert(midi_bytes > 64);
    assert(std::memcmp(midi_buf, "MThd", 4) == 0);
}

int main() {
    test_end_to_end_synthetic_breakbeat();
    std::cout << "test_slice_integration PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake -B build && cmake --build build --target test_slice_integration`
Expected: Build failure due to missing `audio_codecs/slice/slice.h` umbrella include in `audio_codecs.h`.

- [ ] **Step 3: Write minimal implementation**

Create `include/audio_codecs/slice/slice.h`:
```cpp
#pragma once

#include <audio_codecs/slice/slice_types.h>
#include <audio_codecs/slice/slice_detector.h>
#include <audio_codecs/slice/slice_generator.h>
#include <audio_codecs/slice/slice_reader.h>
#include <audio_codecs/slice/slice_playback.h>
#include <audio_codecs/slice/slice_midi_writer.h>
```

Modify `include/audio_codecs/audio_codecs.h` to include `#include <audio_codecs/slice/slice.h>`.

Update `CMakeLists.txt` to add `test_slice_integration`:
```cmake
add_executable(test_slice_integration tests/test_slice_integration.cpp)
target_link_libraries(test_slice_integration PRIVATE audio_codecs_slice)
add_test(NAME SliceIntegrationTest COMMAND test_slice_integration)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake -B build && cmake --build build --target test_slice_integration && ctest --test-dir build -R Slice --output-on-failure`
Expected: All 6 slice tests PASS (`test_slice_types`, `test_slice_detector`, `test_slice_modes`, `test_slice_generator`, `test_slice_reader`, `test_slice_playback`, `test_slice_integration`).

Run the complete test suite:
Run: `ctest --test-dir build --output-on-failure`
Expected: 86/86 tests PASS (100%).

- [ ] **Step 5: Commit**

```bash
git add include/audio_codecs/slice/slice.h include/audio_codecs/audio_codecs.h tests/test_slice_integration.cpp CMakeLists.txt
git commit -m "feat(slice): umbrella headers and end-to-end integration test suite"
```
