# Audio Preview Engine & APV File Format Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a zero-allocation, embedded-friendly audio preview engine and binary file format (`.apv` / `APV1`) within `audio-codecs` (`audio_codecs::preview`), targeting RT1176 / Teensy microcontrollers and desktop platforms.

**Architecture:** The engine segments decoded audio streams into 512-byte PCM sections (128 sample frames, $\approx 2.9\text{ ms}$ at 44.1 kHz) to compute signed 8-bit bipolar peak envelopes (`min`, `max` per channel). It writes a 128-byte sector-aligned header followed by a multi-resolution hierarchical Level-of-Detail pyramid (LOD 0 = $1\times$, LOD 1 = $16\times$, LOD 2 = $256\times$) using zero extra RAM by reading back LOD 0 off storage to generate overview tiers. The reader automatically selects the optimal LOD and applies peak-preserving decimation for zoomed-out views and linear interpolation for zoomed-in views.

**Tech Stack:** Pure C++17 (zero heap allocations, zero external runtime dependencies), CMake, CTest, Teensy SD/USBHost template adapter.

**Spec:** `docs/superpowers/specs/2026-09-08-audio-preview-engine-design.md`  
**Linear Issue:** [NEW-40](https://linear.app/newdigate/issue/NEW-40/audio-preview-engine-and-apv-file-format-for-rt1176-teensy)

## Global Constraints

- Pure C++17; no dynamic heap allocations (`malloc`, `new`, `std::vector`) during steady-state preview generation or reading.
- No Arduino or platform headers inside core headers; storage I/O must use abstract `SeekableReader` / `SeekableWriter`.
- `sizeof(ApvHeader)` must be exactly 128 bytes (asserted via compile-time `static_assert`).
- All integers stored in Little-Endian byte order.
- Base chunk frame size is fixed at 128 sample frames (corresponding to 512 bytes of 16-bit stereo PCM).

---

### Task 1: Core Preview Data Structures & Format Header (`preview_types.h`)

**Files:**
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/preview/preview_types.h`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_preview_types.cpp`
- Modify: `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`

**Interfaces:**
- Consumes: Standard `<cstdint>`, `<cstddef>`
- Produces:
  - `APV_MAGIC = 0x31565041`
  - `APV_VERSION = 1`
  - `BASE_CHUNK_FRAMES = 128`
  - `enum ApvFlags : uint16_t { APV_FLAG_STEREO = 1, APV_FLAG_HAS_LODS = 2 }`
  - `struct ApvLodDescriptor { uint32_t downsample_ratio; uint32_t chunk_count; uint64_t file_offset; }`
  - `struct ApvHeader` (128 bytes, packed)
  - `struct WaveformPointMono { int8_t min; int8_t max; }`
  - `struct WaveformPointStereo { int8_t left_min; int8_t left_max; int8_t right_min; int8_t right_max; }`

- [ ] **Step 1: Write the failing test**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_preview_types.cpp`:
```cpp
#include "audio_codecs/preview/preview_types.h"
#include <cassert>
#include <cstring>
#include <iostream>

using namespace audio_codecs::preview;

int main() {
    static_assert(sizeof(ApvLodDescriptor) == 16, "ApvLodDescriptor must be 16 bytes");
    static_assert(sizeof(ApvHeader) == 128, "ApvHeader must be exactly 128 bytes");
    static_assert(sizeof(WaveformPointMono) == 2, "WaveformPointMono must be 2 bytes");
    static_assert(sizeof(WaveformPointStereo) == 4, "WaveformPointStereo must be 4 bytes");

    ApvHeader hdr{};
    hdr.magic = APV_MAGIC;
    hdr.version = APV_VERSION;
    hdr.flags = APV_FLAG_STEREO | APV_FLAG_HAS_LODS;
    hdr.sample_rate = 44100;
    hdr.channels = 2;
    hdr.bytes_per_chunk = 4;
    hdr.samples_per_base_chunk = BASE_CHUNK_FRAMES;
    hdr.total_pcm_frames = 44100 * 60;
    hdr.duration_ms = 60000;
    hdr.lod_count = 3;

    assert(hdr.magic == 0x31565041);
    assert(hdr.version == 1);
    assert(hdr.channels == 2);
    assert(hdr.bytes_per_chunk == 4);
    assert(hdr.samples_per_base_chunk == 128);
    assert(sizeof(hdr) == 128);

    std::cout << "test_preview_types PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake -B /Users/moolet/Development/github/newdigate/audio-codecs/build -S /Users/moolet/Development/github/newdigate/audio-codecs
```
Expected: FAIL (missing header `audio_codecs/preview/preview_types.h`).

- [ ] **Step 3: Write minimal implementation**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/preview/preview_types.h`:
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
    uint32_t downsample_ratio{1}; // 1 for LOD 0, 16 for LOD 1, 256 for LOD 2
    uint32_t chunk_count{0};      // Total chunks stored in this LOD tier
    uint64_t file_offset{0};      // Byte offset from start of file to chunk data
};

struct ApvHeader {
    uint32_t magic{APV_MAGIC};         // 0x31565041 ("APV1")
    uint16_t version{APV_VERSION};     // Format version (1)
    uint16_t flags{0};                 // ApvFlags bitmask
    uint32_t sample_rate{44100};       // Audio sample rate (e.g., 44100, 48000)
    uint8_t  channels{2};              // 1 (Mono) or 2 (Stereo)
    uint8_t  bytes_per_chunk{4};       // 2 (Mono) or 4 (Stereo)
    uint16_t samples_per_base_chunk{BASE_CHUNK_FRAMES}; // 128
    uint64_t total_pcm_frames{0};      // Total sample frames in original audio track
    uint32_t duration_ms{0};           // Total track duration in milliseconds
    uint32_t source_file_size{0};      // Original audio file size (for cache validation)
    uint32_t source_header_crc32{0};   // CRC32 of first 4 KB of audio file
    uint8_t  lod_count{0};             // Number of LOD tiers stored (typically 3)
    uint8_t  reserved[7]{0};           // Reserved padding to align LOD descriptors
    ApvLodDescriptor lods[4]{};        // Up to 4 LOD descriptors (64 bytes total)
    uint8_t  padding[16]{0};           // Pads struct to exactly 128 bytes
};

static_assert(sizeof(ApvHeader) == 128, "ApvHeader must be exactly 128 bytes");
static_assert(sizeof(ApvLodDescriptor) == 16, "ApvLodDescriptor must be exactly 16 bytes");

struct WaveformPointMono {
    int8_t min{0}; // Negative trough [-128..0]
    int8_t max{0}; // Positive peak   [0..127]
};

struct WaveformPointStereo {
    int8_t left_min{0};
    int8_t left_max{0};
    int8_t right_min{0};
    int8_t right_max{0};
};

#pragma pack(pop)

} // namespace audio_codecs::preview
```

Add test executable to `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`:
```cmake
add_executable(test_preview_types tests/test_preview_types.cpp)
add_test(NAME PreviewTypesTest COMMAND test_preview_types)
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cmake -B /Users/moolet/Development/github/newdigate/audio-codecs/build -S /Users/moolet/Development/github/newdigate/audio-codecs && cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_preview_types && /Users/moolet/Development/github/newdigate/audio-codecs/build/test_preview_types
```
Expected: PASS (`test_preview_types PASSED`).

- [ ] **Step 5: Commit**

```bash
git -C /Users/moolet/Development/github/newdigate/audio-codecs add include/audio_codecs/preview/preview_types.h tests/test_preview_types.cpp CMakeLists.txt
git -C /Users/moolet/Development/github/newdigate/audio-codecs commit -m "feat(preview): define APV file format header and chunk types"
```

---

### Task 2: Storage Stream Abstractions & Adapters (`preview_stream.h`, `teensy_stream_adapter.h`)

**Files:**
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/preview/preview_stream.h`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/preview/teensy_stream_adapter.h`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/src/preview/preview_stream.cpp`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_preview_stream.cpp`
- Modify: `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`

**Interfaces:**
- Consumes: `preview_types.h`
- Produces:
  - `class SeekableReader` (virtual `read`, `seek`, `position`, `size`)
  - `class SeekableWriter` (virtual `write`, `seek`, `position`, `size`, `flush`)
  - `class MemoryReader : public SeekableReader`
  - `class MemoryWriter : public SeekableWriter`
  - `class FileStreamReader : public SeekableReader`
  - `class FileStreamWriter : public SeekableWriter`
  - `template <typename TFile> class TeensyFileStream`

- [ ] **Step 1: Write the failing test**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_preview_stream.cpp`:
```cpp
#include "audio_codecs/preview/preview_stream.h"
#include "audio_codecs/preview/teensy_stream_adapter.h"
#include <cassert>
#include <cstring>
#include <iostream>

using namespace audio_codecs::preview;

// Mock Teensy File class
struct MockTeensyFile {
    uint8_t buffer[1024]{0};
    uint64_t pos{0};
    uint64_t len{0};

    size_t read(uint8_t* dest, size_t bytes) {
        size_t available = (pos < len) ? static_cast<size_t>(len - pos) : 0;
        size_t to_read = (bytes < available) ? bytes : available;
        if (to_read > 0) {
            std::memcpy(dest, buffer + pos, to_read);
            pos += to_read;
        }
        return to_read;
    }

    size_t write(const uint8_t* src, size_t bytes) {
        size_t available = (pos < sizeof(buffer)) ? static_cast<size_t>(sizeof(buffer) - pos) : 0;
        size_t to_write = (bytes < available) ? bytes : available;
        if (to_write > 0) {
            std::memcpy(buffer + pos, src, to_write);
            pos += to_write;
            if (pos > len) len = pos;
        }
        return to_write;
    }

    bool seek(uint64_t p) {
        if (p <= len) { pos = p; return true; }
        return false;
    }

    uint64_t position() const { return pos; }
    uint64_t size() const { return len; }
    void flush() {}
};

int main() {
    // 1. MemoryWriter and MemoryReader
    uint8_t mem[512]{0};
    MemoryWriter mw(mem, sizeof(mem));
    const uint8_t data[] = "HelloAPVStream!";
    size_t written = mw.write(data, sizeof(data));
    assert(written == sizeof(data));
    assert(mw.position() == sizeof(data));
    assert(mw.size() == sizeof(data));

    MemoryReader mr(mem, mw.size());
    assert(mr.size() == sizeof(data));
    uint8_t read_buf[32]{0};
    size_t r = mr.read(read_buf, sizeof(data));
    assert(r == sizeof(data));
    assert(std::strcmp(reinterpret_cast<char*>(read_buf), "HelloAPVStream!") == 0);

    // Seek test
    assert(mr.seek(5));
    assert(mr.position() == 5);
    r = mr.read(read_buf, 3);
    assert(r == 3);
    assert(std::memcmp(read_buf, "APV", 3) == 0);

    // 2. TeensyFileStream adapter test
    MockTeensyFile mock_file;
    TeensyFileStream<MockTeensyFile> stream(mock_file);
    written = stream.write(data, sizeof(data));
    assert(written == sizeof(data));
    assert(stream.position() == sizeof(data));
    assert(stream.seek(0));
    std::memset(read_buf, 0, sizeof(read_buf));
    r = stream.read(read_buf, sizeof(data));
    assert(r == sizeof(data));
    assert(std::strcmp(reinterpret_cast<char*>(read_buf), "HelloAPVStream!") == 0);

    std::cout << "test_preview_stream PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_preview_stream
```
Expected: FAIL (headers and classes not found).

- [ ] **Step 3: Write minimal implementation**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/preview/preview_stream.h`:
```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

class MemoryReader : public SeekableReader {
public:
    MemoryReader(const uint8_t* data, size_t size);
    size_t read(uint8_t* dest, size_t bytes) override;
    bool seek(uint64_t position) override;
    uint64_t position() const override;
    uint64_t size() const override;

private:
    const uint8_t* data_{nullptr};
    size_t size_{0};
    size_t pos_{0};
};

class MemoryWriter : public SeekableWriter {
public:
    MemoryWriter(uint8_t* buffer, size_t capacity);
    size_t write(const uint8_t* src, size_t bytes) override;
    bool seek(uint64_t position) override;
    uint64_t position() const override;
    uint64_t size() const override;
    void flush() override {}

private:
    uint8_t* buffer_{nullptr};
    size_t capacity_{0};
    size_t pos_{0};
    size_t length_{0};
};

class FileStreamReader : public SeekableReader {
public:
    explicit FileStreamReader(std::FILE* fp);
    size_t read(uint8_t* dest, size_t bytes) override;
    bool seek(uint64_t position) override;
    uint64_t position() const override;
    uint64_t size() const override;

private:
    std::FILE* fp_{nullptr};
    uint64_t size_{0};
};

class FileStreamWriter : public SeekableWriter {
public:
    explicit FileStreamWriter(std::FILE* fp);
    size_t write(const uint8_t* src, size_t bytes) override;
    bool seek(uint64_t position) override;
    uint64_t position() const override;
    uint64_t size() const override;
    void flush() override;

private:
    std::FILE* fp_{nullptr};
};

} // namespace audio_codecs::preview
```

Create `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/preview/teensy_stream_adapter.h`:
```cpp
#pragma once
#include "audio_codecs/preview/preview_stream.h"

namespace audio_codecs::preview {

template <typename TFile>
class TeensyFileStream : public SeekableReader, public SeekableWriter {
public:
    explicit TeensyFileStream(TFile& file) : file_(file) {}

    size_t read(uint8_t* dest, size_t bytes) override {
        return static_cast<size_t>(file_.read(dest, bytes));
    }

    size_t write(const uint8_t* src, size_t bytes) override {
        return static_cast<size_t>(file_.write(src, bytes));
    }

    bool seek(uint64_t position) override {
        return file_.seek(position);
    }

    uint64_t position() const override {
        return file_.position();
    }

    uint64_t size() const override {
        return file_.size();
    }

    void flush() override {
        file_.flush();
    }

private:
    TFile& file_;
};

} // namespace audio_codecs::preview
```

Create `/Users/moolet/Development/github/newdigate/audio-codecs/src/preview/preview_stream.cpp`:
```cpp
#include "audio_codecs/preview/preview_stream.h"
#include <algorithm>

namespace audio_codecs::preview {

// --- MemoryReader ---
MemoryReader::MemoryReader(const uint8_t* data, size_t size)
    : data_(data), size_(size), pos_(0) {}

size_t MemoryReader::read(uint8_t* dest, size_t bytes) {
    if (!data_ || pos_ >= size_ || bytes == 0) return 0;
    size_t to_read = std::min(bytes, size_ - pos_);
    std::memcpy(dest, data_ + pos_, to_read);
    pos_ += to_read;
    return to_read;
}

bool MemoryReader::seek(uint64_t position) {
    if (position > size_) return false;
    pos_ = static_cast<size_t>(position);
    return true;
}

uint64_t MemoryReader::position() const { return pos_; }
uint64_t MemoryReader::size() const { return size_; }

// --- MemoryWriter ---
MemoryWriter::MemoryWriter(uint8_t* buffer, size_t capacity)
    : buffer_(buffer), capacity_(capacity), pos_(0), length_(0) {}

size_t MemoryWriter::write(const uint8_t* src, size_t bytes) {
    if (!buffer_ || pos_ >= capacity_ || bytes == 0) return 0;
    size_t to_write = std::min(bytes, capacity_ - pos_);
    std::memcpy(buffer_ + pos_, src, to_write);
    pos_ += to_write;
    if (pos_ > length_) length_ = pos_;
    return to_write;
}

bool MemoryWriter::seek(uint64_t position) {
    if (position > capacity_) return false;
    pos_ = static_cast<size_t>(position);
    if (pos_ > length_) length_ = pos_;
    return true;
}

uint64_t MemoryWriter::position() const { return pos_; }
uint64_t MemoryWriter::size() const { return length_; }

// --- FileStreamReader ---
FileStreamReader::FileStreamReader(std::FILE* fp) : fp_(fp) {
    if (fp_) {
        std::fseek(fp_, 0, SEEK_END);
        size_ = static_cast<uint64_t>(std::ftell(fp_));
        std::fseek(fp_, 0, SEEK_SET);
    }
}

size_t FileStreamReader::read(uint8_t* dest, size_t bytes) {
    if (!fp_) return 0;
    return std::fread(dest, 1, bytes, fp_);
}

bool FileStreamReader::seek(uint64_t position) {
    if (!fp_) return false;
    return std::fseek(fp_, static_cast<long>(position), SEEK_SET) == 0;
}

uint64_t FileStreamReader::position() const {
    if (!fp_) return 0;
    return static_cast<uint64_t>(std::ftell(fp_));
}

uint64_t FileStreamReader::size() const { return size_; }

// --- FileStreamWriter ---
FileStreamWriter::FileStreamWriter(std::FILE* fp) : fp_(fp) {}

size_t FileStreamWriter::write(const uint8_t* src, size_t bytes) {
    if (!fp_) return 0;
    return std::fwrite(src, 1, bytes, fp_);
}

bool FileStreamWriter::seek(uint64_t position) {
    if (!fp_) return false;
    return std::fseek(fp_, static_cast<long>(position), SEEK_SET) == 0;
}

uint64_t FileStreamWriter::position() const {
    if (!fp_) return 0;
    return static_cast<uint64_t>(std::ftell(fp_));
}

uint64_t FileStreamWriter::size() const {
    if (!fp_) return 0;
    long curr = std::ftell(fp_);
    std::fseek(fp_, 0, SEEK_END);
    long sz = std::ftell(fp_);
    std::fseek(fp_, curr, SEEK_SET);
    return static_cast<uint64_t>(sz);
}

void FileStreamWriter::flush() {
    if (fp_) std::fflush(fp_);
}

} // namespace audio_codecs::preview
```

Update `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`:
```cmake
# Preview library
add_library(audio_codecs_preview STATIC
    src/preview/preview_stream.cpp
)
target_link_libraries(audio_codecs_preview PUBLIC audio_codecs_core)

add_executable(test_preview_stream tests/test_preview_stream.cpp)
target_link_libraries(test_preview_stream PRIVATE audio_codecs_preview)
add_test(NAME PreviewStreamTest COMMAND test_preview_stream)
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cmake -B /Users/moolet/Development/github/newdigate/audio-codecs/build -S /Users/moolet/Development/github/newdigate/audio-codecs && cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_preview_stream && /Users/moolet/Development/github/newdigate/audio-codecs/build/test_preview_stream
```
Expected: PASS (`test_preview_stream PASSED`).

- [ ] **Step 5: Commit**

```bash
git -C /Users/moolet/Development/github/newdigate/audio-codecs add include/audio_codecs/preview/preview_stream.h include/audio_codecs/preview/teensy_stream_adapter.h src/preview/preview_stream.cpp tests/test_preview_stream.cpp CMakeLists.txt
git -C /Users/moolet/Development/github/newdigate/audio-codecs commit -m "feat(preview): add SeekableReader/Writer stream abstractions and Teensy adapter"
```

---

### Task 3: Preview Generator Engine (`preview_generator.h`, `preview_generator.cpp`)

**Files:**
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/preview/preview_generator.h`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/src/preview/preview_generator.cpp`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_preview_generator.cpp`
- Modify: `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`

**Interfaces:**
- Consumes:
  - `preview_types.h` (`ApvHeader`, `BASE_CHUNK_FRAMES`)
  - `preview_stream.h` (`SeekableReader`, `SeekableWriter`)
  - `audio_codecs/core/decoder_interface.h` (`AudioDecoder`)
- Produces:
  - `class PreviewGenerator`:
    - `bool init(SeekableReader& audio_source, SeekableWriter& preview_dest, AudioDecoder* decoder = nullptr, uint32_t sample_rate = 44100, uint8_t channels = 2, bool stereo = true);`
    - `GeneratorStatus step(size_t chunk_budget = 64);`
    - `bool generate_all();`
    - `float progress() const;`
    - `const ApvHeader& header() const;`

- [ ] **Step 1: Write the failing test**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_preview_generator.cpp`:
```cpp
#include "audio_codecs/preview/preview_generator.h"
#include "audio_codecs/preview/preview_stream.h"
#include <cassert>
#include <vector>
#include <iostream>

using namespace audio_codecs::preview;

int main() {
    // Generate 512 chunks of stereo audio (512 * 128 = 65,536 sample frames)
    // Put an isolated full-scale spike at frame 200 on Left channel (+32767)
    constexpr size_t total_frames = 512 * 128;
    std::vector<int16_t> pcm(total_frames * 2, 0);
    pcm[200 * 2] = 32767;     // Left spike at frame 200 (in chunk 1: 128..255)
    pcm[200 * 2 + 1] = -32768;// Right trough at frame 200

    MemoryReader pcm_reader(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * sizeof(int16_t));
    std::vector<uint8_t> out_buf(128 + 512 * 4 + 32 * 4 + 2 * 4 + 512, 0);
    MemoryWriter out_writer(out_buf.data(), out_buf.size());

    PreviewGenerator generator;
    bool ok = generator.init(pcm_reader, out_writer, nullptr, 44100, 2, true);
    assert(ok);

    bool done = generator.generate_all();
    assert(done);

    const ApvHeader& hdr = generator.header();
    assert(hdr.magic == APV_MAGIC);
    assert(hdr.total_pcm_frames == total_frames);
    assert(hdr.lods[0].chunk_count == 512);
    assert(hdr.lods[1].chunk_count == 32);  // 512 / 16 = 32
    assert(hdr.lods[2].chunk_count == 2);   // 32 / 16 = 2

    // Check LOD 0 spike in chunk 1
    const uint8_t* p = out_buf.data() + hdr.lods[0].file_offset;
    const WaveformPointStereo* lod0 = reinterpret_cast<const WaveformPointStereo*>(p);
    assert(lod0[0].left_max == 0 && lod0[0].right_min == 0); // chunk 0 silence
    assert(lod0[1].left_max == 127);  // chunk 1 has spike
    assert(lod0[1].right_min == -128); // chunk 1 has negative trough

    // Check LOD 1 spike (chunk 1 falls into LOD 1 chunk 0)
    const WaveformPointStereo* lod1 = reinterpret_cast<const WaveformPointStereo*>(out_buf.data() + hdr.lods[1].file_offset);
    assert(lod1[0].left_max == 127);
    assert(lod1[0].right_min == -128);

    // Check LOD 2 spike (chunk 1 falls into LOD 2 chunk 0)
    const WaveformPointStereo* lod2 = reinterpret_cast<const WaveformPointStereo*>(out_buf.data() + hdr.lods[2].file_offset);
    assert(lod2[0].left_max == 127);
    assert(lod2[0].right_min == -128);

    std::cout << "test_preview_generator PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_preview_generator
```
Expected: FAIL (headers and class missing).

- [ ] **Step 3: Write minimal implementation**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/preview/preview_generator.h`:
```cpp
#pragma once
#include "audio_codecs/preview/preview_types.h"
#include "audio_codecs/preview/preview_stream.h"
#include "audio_codecs/core/decoder_interface.h"
#include <algorithm>

namespace audio_codecs::preview {

enum class GeneratorStatus {
    Working,
    Complete,
    ErrorSource,
    ErrorDest
};

class PreviewGenerator {
public:
    PreviewGenerator();
    ~PreviewGenerator() = default;

    bool init(SeekableReader& audio_source,
              SeekableWriter& preview_dest,
              AudioDecoder* decoder = nullptr,
              uint32_t sample_rate = 44100,
              uint8_t channels = 2,
              bool stereo = true);

    GeneratorStatus step(size_t chunk_budget = 64);
    bool generate_all();
    float progress() const;
    const ApvHeader& header() const;

private:
    enum class State {
        Init,
        DecodeLOD0,
        GenerateLOD1,
        GenerateLOD2,
        FinalizeHeader,
        Done,
        Error
    };

    int8_t scale_pcm(int16_t sample) const {
        int v = sample >> 8;
        return static_cast<int8_t>(std::clamp(v, -128, 127));
    }

    SeekableReader* source_{nullptr};
    SeekableWriter* dest_{nullptr};
    AudioDecoder* decoder_{nullptr};

    ApvHeader header_{};
    State state_{State::Init};

    bool stereo_{true};
    uint32_t sample_rate_{44100};
    uint8_t channels_{2};

    uint64_t total_frames_processed_{0};
    uint32_t lod0_chunk_count_{0};
    uint32_t lod1_chunk_count_{0};
    uint32_t lod2_chunk_count_{0};

    // Buffer for 1 base chunk (128 frames)
    int16_t frame_buf_[BASE_CHUNK_FRAMES * 2]{0};
};

} // namespace audio_codecs::preview
```

Create `/Users/moolet/Development/github/newdigate/audio-codecs/src/preview/preview_generator.cpp`:
```cpp
#include "audio_codecs/preview/preview_generator.h"
#include <cstring>

namespace audio_codecs::preview {

PreviewGenerator::PreviewGenerator() = default;

bool PreviewGenerator::init(SeekableReader& audio_source,
                           SeekableWriter& preview_dest,
                           AudioDecoder* decoder,
                           uint32_t sample_rate,
                           uint8_t channels,
                           bool stereo) {
    source_ = &audio_source;
    dest_ = &preview_dest;
    decoder_ = decoder;
    sample_rate_ = sample_rate;
    channels_ = channels;
    stereo_ = stereo && (channels > 1);

    std::memset(&header_, 0, sizeof(header_));
    header_.magic = APV_MAGIC;
    header_.version = APV_VERSION;
    header_.flags = (stereo_ ? APV_FLAG_STEREO : 0) | APV_FLAG_HAS_LODS;
    header_.sample_rate = sample_rate_;
    header_.channels = stereo_ ? 2 : 1;
    header_.bytes_per_chunk = stereo_ ? 4 : 2;
    header_.samples_per_base_chunk = BASE_CHUNK_FRAMES;
    header_.source_file_size = static_cast<uint32_t>(source_->size());

    total_frames_processed_ = 0;
    lod0_chunk_count_ = 0;
    lod1_chunk_count_ = 0;
    lod2_chunk_count_ = 0;
    state_ = State::Init;
    return true;
}

GeneratorStatus PreviewGenerator::step(size_t chunk_budget) {
    switch (state_) {
        case State::Init: {
            if (!dest_->seek(0)) return GeneratorStatus::ErrorDest;
            uint8_t zero_hdr[128]{0};
            if (dest_->write(zero_hdr, sizeof(zero_hdr)) != sizeof(zero_hdr)) {
                return GeneratorStatus::ErrorDest;
            }
            header_.lods[0].file_offset = 128;
            header_.lods[0].downsample_ratio = 1;
            state_ = State::DecodeLOD0;
            return GeneratorStatus::Working;
        }

        case State::DecodeLOD0: {
            size_t chunks_done = 0;
            while (chunks_done < chunk_budget) {
                // Read 128 frames (512 bytes if 16-bit stereo)
                size_t frames_to_read = BASE_CHUNK_FRAMES;
                size_t bytes_needed = frames_to_read * channels_ * sizeof(int16_t);
                size_t bytes_read = source_->read(reinterpret_cast<uint8_t*>(frame_buf_), bytes_needed);
                if (bytes_read == 0) {
                    // EOF reached
                    header_.lods[0].chunk_count = lod0_chunk_count_;
                    state_ = State::GenerateLOD1;
                    return GeneratorStatus::Working;
                }

                size_t frames_read = bytes_read / (channels_ * sizeof(int16_t));
                if (frames_read == 0) {
                    header_.lods[0].chunk_count = lod0_chunk_count_;
                    state_ = State::GenerateLOD1;
                    return GeneratorStatus::Working;
                }

                // Compute peaks
                if (stereo_) {
                    int8_t l_min = 0, l_max = 0, r_min = 0, r_max = 0;
                    for (size_t f = 0; f < frames_read; ++f) {
                        int8_t l = scale_pcm(frame_buf_[f * channels_]);
                        int8_t r = scale_pcm(frame_buf_[f * channels_ + 1]);
                        if (l < l_min) l_min = l;
                        if (l > l_max) l_max = l;
                        if (r < r_min) r_min = r;
                        if (r > r_max) r_max = r;
                    }
                    WaveformPointStereo pt{l_min, l_max, r_min, r_max};
                    if (dest_->write(reinterpret_cast<const uint8_t*>(&pt), sizeof(pt)) != sizeof(pt)) {
                        return GeneratorStatus::ErrorDest;
                    }
                } else {
                    int8_t m_min = 0, m_max = 0;
                    for (size_t f = 0; f < frames_read; ++f) {
                        int16_t samp = frame_buf_[f * channels_];
                        if (channels_ > 1) {
                            samp = static_cast<int16_t>((samp + frame_buf_[f * channels_ + 1]) / 2);
                        }
                        int8_t m = scale_pcm(samp);
                        if (m < m_min) m_min = m;
                        if (m > m_max) m_max = m;
                    }
                    WaveformPointMono pt{m_min, m_max};
                    if (dest_->write(reinterpret_cast<const uint8_t*>(&pt), sizeof(pt)) != sizeof(pt)) {
                        return GeneratorStatus::ErrorDest;
                    }
                }

                total_frames_processed_ += frames_read;
                lod0_chunk_count_++;
                chunks_done++;
            }
            return GeneratorStatus::Working;
        }

        case State::GenerateLOD1: {
            // LOD 1: 16x downsample of LOD 0
            uint64_t lod1_offset = header_.lods[0].file_offset + (static_cast<uint64_t>(lod0_chunk_count_) * header_.bytes_per_chunk);
            header_.lods[1].file_offset = lod1_offset;
            header_.lods[1].downsample_ratio = 16;

            if (lod0_chunk_count_ == 0) {
                header_.lods[1].chunk_count = 0;
                state_ = State::GenerateLOD2;
                return GeneratorStatus::Working;
            }

            // Read LOD 0 back from storage 16 chunks at a time
            uint32_t full_groups = lod0_chunk_count_ / 16;
            uint32_t remainder = lod0_chunk_count_ % 16;
            uint32_t total_lod1 = full_groups + (remainder ? 1 : 0);

            for (uint32_t g = 0; g < total_lod1; ++g) {
                uint32_t chunks_in_group = (g < full_groups) ? 16 : remainder;
                uint64_t read_offset = header_.lods[0].file_offset + (static_cast<uint64_t>(g) * 16 * header_.bytes_per_chunk);
                if (!dest_->seek(read_offset)) return GeneratorStatus::ErrorDest;

                if (stereo_) {
                    WaveformPointStereo group_chunks[16];
                    size_t bytes_to_read = chunks_in_group * sizeof(WaveformPointStereo);
                    // We need a reader on dest
                    // We can read via dynamic seekable view or memory
                    // Since dest_ is SeekableWriter, if it also implements SeekableReader:
                    SeekableReader* reader = dynamic_cast<SeekableReader*>(dest_);
                    if (!reader) return GeneratorStatus::ErrorDest;
                    if (reader->read(reinterpret_cast<uint8_t*>(group_chunks), bytes_to_read) != bytes_to_read) {
                        return GeneratorStatus::ErrorDest;
                    }

                    int8_t l_min = 0, l_max = 0, r_min = 0, r_max = 0;
                    for (uint32_t i = 0; i < chunks_in_group; ++i) {
                        if (group_chunks[i].left_min < l_min) l_min = group_chunks[i].left_min;
                        if (group_chunks[i].left_max > l_max) l_max = group_chunks[i].left_max;
                        if (group_chunks[i].right_min < r_min) r_min = group_chunks[i].right_min;
                        if (group_chunks[i].right_max > r_max) r_max = group_chunks[i].right_max;
                    }
                    WaveformPointStereo pt{l_min, l_max, r_min, r_max};
                    uint64_t write_offset = lod1_offset + (static_cast<uint64_t>(g) * sizeof(WaveformPointStereo));
                    if (!dest_->seek(write_offset)) return GeneratorStatus::ErrorDest;
                    if (dest_->write(reinterpret_cast<const uint8_t*>(&pt), sizeof(pt)) != sizeof(pt)) {
                        return GeneratorStatus::ErrorDest;
                    }
                } else {
                    WaveformPointMono group_chunks[16];
                    size_t bytes_to_read = chunks_in_group * sizeof(WaveformPointMono);
                    SeekableReader* reader = dynamic_cast<SeekableReader*>(dest_);
                    if (!reader) return GeneratorStatus::ErrorDest;
                    if (reader->read(reinterpret_cast<uint8_t*>(group_chunks), bytes_to_read) != bytes_to_read) {
                        return GeneratorStatus::ErrorDest;
                    }

                    int8_t m_min = 0, m_max = 0;
                    for (uint32_t i = 0; i < chunks_in_group; ++i) {
                        if (group_chunks[i].min < m_min) m_min = group_chunks[i].min;
                        if (group_chunks[i].max > m_max) m_max = group_chunks[i].max;
                    }
                    WaveformPointMono pt{m_min, m_max};
                    uint64_t write_offset = lod1_offset + (static_cast<uint64_t>(g) * sizeof(WaveformPointMono));
                    if (!dest_->seek(write_offset)) return GeneratorStatus::ErrorDest;
                    if (dest_->write(reinterpret_cast<const uint8_t*>(&pt), sizeof(pt)) != sizeof(pt)) {
                        return GeneratorStatus::ErrorDest;
                    }
                }
            }

            lod1_chunk_count_ = total_lod1;
            header_.lods[1].chunk_count = lod1_chunk_count_;
            state_ = State::GenerateLOD2;
            return GeneratorStatus::Working;
        }

        case State::GenerateLOD2: {
            // LOD 2: 16x downsample of LOD 1 (256x overall)
            uint64_t lod2_offset = header_.lods[1].file_offset + (static_cast<uint64_t>(lod1_chunk_count_) * header_.bytes_per_chunk);
            header_.lods[2].file_offset = lod2_offset;
            header_.lods[2].downsample_ratio = 256;

            if (lod1_chunk_count_ == 0) {
                header_.lods[2].chunk_count = 0;
                state_ = State::FinalizeHeader;
                return GeneratorStatus::Working;
            }

            uint32_t full_groups = lod1_chunk_count_ / 16;
            uint32_t remainder = lod1_chunk_count_ % 16;
            uint32_t total_lod2 = full_groups + (remainder ? 1 : 0);

            for (uint32_t g = 0; g < total_lod2; ++g) {
                uint32_t chunks_in_group = (g < full_groups) ? 16 : remainder;
                uint64_t read_offset = header_.lods[1].file_offset + (static_cast<uint64_t>(g) * 16 * header_.bytes_per_chunk);
                if (!dest_->seek(read_offset)) return GeneratorStatus::ErrorDest;

                if (stereo_) {
                    WaveformPointStereo group_chunks[16];
                    size_t bytes_to_read = chunks_in_group * sizeof(WaveformPointStereo);
                    SeekableReader* reader = dynamic_cast<SeekableReader*>(dest_);
                    if (!reader) return GeneratorStatus::ErrorDest;
                    if (reader->read(reinterpret_cast<uint8_t*>(group_chunks), bytes_to_read) != bytes_to_read) {
                        return GeneratorStatus::ErrorDest;
                    }

                    int8_t l_min = 0, l_max = 0, r_min = 0, r_max = 0;
                    for (uint32_t i = 0; i < chunks_in_group; ++i) {
                        if (group_chunks[i].left_min < l_min) l_min = group_chunks[i].left_min;
                        if (group_chunks[i].left_max > l_max) l_max = group_chunks[i].left_max;
                        if (group_chunks[i].right_min < r_min) r_min = group_chunks[i].right_min;
                        if (group_chunks[i].right_max > r_max) r_max = group_chunks[i].right_max;
                    }
                    WaveformPointStereo pt{l_min, l_max, r_min, r_max};
                    uint64_t write_offset = lod2_offset + (static_cast<uint64_t>(g) * sizeof(WaveformPointStereo));
                    if (!dest_->seek(write_offset)) return GeneratorStatus::ErrorDest;
                    if (dest_->write(reinterpret_cast<const uint8_t*>(&pt), sizeof(pt)) != sizeof(pt)) {
                        return GeneratorStatus::ErrorDest;
                    }
                } else {
                    WaveformPointMono group_chunks[16];
                    size_t bytes_to_read = chunks_in_group * sizeof(WaveformPointMono);
                    SeekableReader* reader = dynamic_cast<SeekableReader*>(dest_);
                    if (!reader) return GeneratorStatus::ErrorDest;
                    if (reader->read(reinterpret_cast<uint8_t*>(group_chunks), bytes_to_read) != bytes_to_read) {
                        return GeneratorStatus::ErrorDest;
                    }

                    int8_t m_min = 0, m_max = 0;
                    for (uint32_t i = 0; i < chunks_in_group; ++i) {
                        if (group_chunks[i].min < m_min) m_min = group_chunks[i].min;
                        if (group_chunks[i].max > m_max) m_max = group_chunks[i].max;
                    }
                    WaveformPointMono pt{m_min, m_max};
                    uint64_t write_offset = lod2_offset + (static_cast<uint64_t>(g) * sizeof(WaveformPointMono));
                    if (!dest_->seek(write_offset)) return GeneratorStatus::ErrorDest;
                    if (dest_->write(reinterpret_cast<const uint8_t*>(&pt), sizeof(pt)) != sizeof(pt)) {
                        return GeneratorStatus::ErrorDest;
                    }
                }
            }

            lod2_chunk_count_ = total_lod2;
            header_.lods[2].chunk_count = lod2_chunk_count_;
            state_ = State::FinalizeHeader;
            return GeneratorStatus::Working;
        }

        case State::FinalizeHeader: {
            header_.total_pcm_frames = total_frames_processed_;
            if (sample_rate_ > 0) {
                header_.duration_ms = static_cast<uint32_t>((total_frames_processed_ * 1000ULL) / sample_rate_);
            }
            header_.lod_count = 3;

            if (!dest_->seek(0)) return GeneratorStatus::ErrorDest;
            if (dest_->write(reinterpret_cast<const uint8_t*>(&header_), sizeof(header_)) != sizeof(header_)) {
                return GeneratorStatus::ErrorDest;
            }
            dest_->flush();
            state_ = State::Done;
            return GeneratorStatus::Complete;
        }

        case State::Done:
            return GeneratorStatus::Complete;

        case State::Error:
        default:
            return GeneratorStatus::ErrorDest;
    }
}

bool PreviewGenerator::generate_all() {
    while (true) {
        GeneratorStatus s = step(256);
        if (s == GeneratorStatus::Complete) return true;
        if (s != GeneratorStatus::Working) return false;
    }
}

float PreviewGenerator::progress() const {
    if (state_ == State::Done) return 100.0f;
    if (state_ == State::Init) return 0.0f;
    if (source_ && source_->size() > 0) {
        return (static_cast<float>(source_->position()) / static_cast<float>(source_->size())) * 85.0f;
    }
    return 50.0f;
}

const ApvHeader& PreviewGenerator::header() const {
    return header_;
}

} // namespace audio_codecs::preview
```

Update `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`:
```cmake
# Preview library
add_library(audio_codecs_preview STATIC
    src/preview/preview_stream.cpp
    src/preview/preview_generator.cpp
)
target_link_libraries(audio_codecs_preview PUBLIC audio_codecs_core)

add_executable(test_preview_generator tests/test_preview_generator.cpp)
target_link_libraries(test_preview_generator PRIVATE audio_codecs_preview)
add_test(NAME PreviewGeneratorTest COMMAND test_preview_generator)
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cmake -B /Users/moolet/Development/github/newdigate/audio-codecs/build -S /Users/moolet/Development/github/newdigate/audio-codecs && cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_preview_generator && /Users/moolet/Development/github/newdigate/audio-codecs/build/test_preview_generator
```
Expected: PASS (`test_preview_generator PASSED`).

- [ ] **Step 5: Commit**

```bash
git -C /Users/moolet/Development/github/newdigate/audio-codecs add include/audio_codecs/preview/preview_generator.h src/preview/preview_generator.cpp tests/test_preview_generator.cpp CMakeLists.txt
git -C /Users/moolet/Development/github/newdigate/audio-codecs commit -m "feat(preview): implement PreviewGenerator with multi-LOD pyramid and cooperative step"
```

---

### Task 4: Preview Reader, Decimation & Interpolation Engine (`preview_reader.h`, `preview_reader.cpp`)

**Files:**
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/preview/preview_reader.h`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/src/preview/preview_reader.cpp`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_preview_reader.cpp`
- Modify: `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`

**Interfaces:**
- Consumes:
  - `preview_types.h` (`ApvHeader`, `WaveformPointMono`, `WaveformPointStereo`)
  - `preview_stream.h` (`SeekableReader`)
- Produces:
  - `class PreviewReader`:
    - `bool init(SeekableReader& preview_file);`
    - `uint32_t duration_ms() const;`
    - `uint32_t sample_rate() const;`
    - `uint8_t channels() const;`
    - `uint64_t total_frames() const;`
    - `const ApvHeader& header() const;`
    - `size_t read_preview(uint32_t start_ms, uint32_t duration_ms, WaveformPointMono* out_points, size_t num_points);`
    - `size_t read_preview_stereo(uint32_t start_ms, uint32_t duration_ms, WaveformPointStereo* out_points, size_t num_points);`

- [ ] **Step 1: Write the failing test**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_preview_reader.cpp`:
```cpp
#include "audio_codecs/preview/preview_reader.h"
#include "audio_codecs/preview/preview_generator.h"
#include "audio_codecs/preview/preview_stream.h"
#include <cassert>
#include <vector>
#include <iostream>

using namespace audio_codecs::preview;

int main() {
    // Generate preview with 512 chunks (65,536 frames, ~1486 ms at 44100 Hz)
    // Left channel has single spike at frame 200 (chunk 1)
    constexpr size_t total_frames = 512 * 128;
    std::vector<int16_t> pcm(total_frames * 2, 0);
    pcm[200 * 2] = 32767;      // spike
    pcm[200 * 2 + 1] = -32768;

    MemoryReader pcm_reader(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * sizeof(int16_t));
    std::vector<uint8_t> apv_file(1024 * 16, 0);
    MemoryWriter writer(apv_file.data(), apv_file.size());

    PreviewGenerator generator;
    generator.init(pcm_reader, writer, nullptr, 44100, 2, true);
    generator.generate_all();

    // Now test PreviewReader
    MemoryReader reader(apv_file.data(), writer.size());
    PreviewReader pr;
    bool ok = pr.init(reader);
    assert(ok);
    assert(pr.sample_rate() == 44100);
    assert(pr.channels() == 2);
    assert(pr.duration_ms() > 1400);

    // 1. Zoomed out query: read entire track into 32 points
    WaveformPointStereo overview[32]{};
    size_t pts = pr.read_preview_stereo(0, pr.duration_ms(), overview, 32);
    assert(pts == 32);
    // Spike must be preserved in overview[0] (frame 200 is near start)
    assert(overview[0].left_max == 127);
    assert(overview[0].right_min == -128);

    // 2. Zoomed in query: read 20 ms around spike into 50 points (interpolated)
    WaveformPointStereo zoomed[50]{};
    pts = pr.read_preview_stereo(2, 10, zoomed, 50);
    assert(pts == 50);
    // Interpolated values must be smooth (no uninitialized memory)
    for (size_t i = 0; i < 50; ++i) {
        assert(zoomed[i].left_min <= zoomed[i].left_max);
    }

    std::cout << "test_preview_reader PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_preview_reader
```
Expected: FAIL (PreviewReader not implemented).

- [ ] **Step 3: Write minimal implementation**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/preview/preview_reader.h`:
```cpp
#pragma once
#include "audio_codecs/preview/preview_types.h"
#include "audio_codecs/preview/preview_stream.h"

namespace audio_codecs::preview {

class PreviewReader {
public:
    PreviewReader();
    ~PreviewReader() = default;

    bool init(SeekableReader& preview_file);

    uint32_t duration_ms() const;
    uint32_t sample_rate() const;
    uint8_t  channels() const;
    uint64_t total_frames() const;
    const ApvHeader& header() const;

    size_t read_preview(uint32_t start_ms, uint32_t duration_ms,
                        WaveformPointMono* out_points, size_t num_points);

    size_t read_preview_stereo(uint32_t start_ms, uint32_t duration_ms,
                               WaveformPointStereo* out_points, size_t num_points);

private:
    SeekableReader* file_{nullptr};
    ApvHeader header_{};

    uint8_t select_lod(float ms_per_point) const;
};

} // namespace audio_codecs::preview
```

Create `/Users/moolet/Development/github/newdigate/audio-codecs/src/preview/preview_reader.cpp`:
```cpp
#include "audio_codecs/preview/preview_reader.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace audio_codecs::preview {

PreviewReader::PreviewReader() = default;

bool PreviewReader::init(SeekableReader& preview_file) {
    file_ = &preview_file;
    if (!file_ || !file_->seek(0)) return false;

    if (file_->read(reinterpret_cast<uint8_t*>(&header_), sizeof(header_)) != sizeof(header_)) {
        return false;
    }

    if (header_.magic != APV_MAGIC || header_.version != APV_VERSION) {
        return false;
    }

    return true;
}

uint32_t PreviewReader::duration_ms() const { return header_.duration_ms; }
uint32_t PreviewReader::sample_rate() const { return header_.sample_rate; }
uint8_t  PreviewReader::channels() const { return header_.channels; }
uint64_t PreviewReader::total_frames() const { return header_.total_pcm_frames; }
const ApvHeader& PreviewReader::header() const { return header_; }

uint8_t PreviewReader::select_lod(float ms_per_point) const {
    if (header_.lod_count < 2) return 0;

    // ms per chunk at each LOD
    float ms_lod0 = (static_cast<float>(header_.samples_per_base_chunk) * 1000.0f) / header_.sample_rate;
    float ms_lod1 = ms_lod0 * 16.0f;
    float ms_lod2 = ms_lod0 * 256.0f;

    if (header_.lod_count >= 3 && ms_per_point >= ms_lod2 && header_.lods[2].chunk_count > 0) {
        return 2;
    }
    if (ms_per_point >= ms_lod1 && header_.lods[1].chunk_count > 0) {
        return 1;
    }
    return 0;
}

size_t PreviewReader::read_preview_stereo(uint32_t start_ms, uint32_t duration_ms,
                                         WaveformPointStereo* out_points, size_t num_points) {
    if (!file_ || !out_points || num_points == 0 || header_.duration_ms == 0) return 0;

    float ms_per_point = static_cast<float>(duration_ms) / static_cast<float>(num_points);
    uint8_t lod_idx = select_lod(ms_per_point);
    const ApvLodDescriptor& lod = header_.lods[lod_idx];
    if (lod.chunk_count == 0) return 0;

    float frames_per_chunk = static_cast<float>(header_.samples_per_base_chunk) * lod.downsample_ratio;
    float ms_per_chunk = (frames_per_chunk * 1000.0f) / header_.sample_rate;

    float start_chunk_f = static_cast<float>(start_ms) / ms_per_chunk;
    float chunk_span = static_cast<float>(duration_ms) / ms_per_chunk;
    float chunks_per_point = chunk_span / static_cast<float>(num_points);

    for (size_t i = 0; i < num_points; ++i) {
        if (chunks_per_point >= 1.0f) {
            // Decimation (Peak Preservation)
            uint32_t c_start = static_cast<uint32_t>(start_chunk_f + i * chunks_per_point);
            uint32_t c_end = static_cast<uint32_t>(start_chunk_f + (i + 1) * chunks_per_point);
            if (c_start >= lod.chunk_count) c_start = lod.chunk_count - 1;
            if (c_end >= lod.chunk_count) c_end = lod.chunk_count - 1;
            if (c_end < c_start) c_end = c_start;

            int8_t l_min = 0, l_max = 0, r_min = 0, r_max = 0;
            for (uint32_t c = c_start; c <= c_end; ++c) {
                uint64_t off = lod.file_offset + (static_cast<uint64_t>(c) * header_.bytes_per_chunk);
                file_->seek(off);
                WaveformPointStereo pt{};
                file_->read(reinterpret_cast<uint8_t*>(&pt), sizeof(pt));
                if (pt.left_min < l_min) l_min = pt.left_min;
                if (pt.left_max > l_max) l_max = pt.left_max;
                if (pt.right_min < r_min) r_min = pt.right_min;
                if (pt.right_max > r_max) r_max = pt.right_max;
            }
            out_points[i] = {l_min, l_max, r_min, r_max};
        } else {
            // Linear Interpolation
            float c_f = start_chunk_f + i * chunks_per_point;
            uint32_t c0 = static_cast<uint32_t>(std::floor(c_f));
            uint32_t c1 = c0 + 1;
            float alpha = c_f - static_cast<float>(c0);

            if (c0 >= lod.chunk_count) c0 = lod.chunk_count - 1;
            if (c1 >= lod.chunk_count) c1 = lod.chunk_count - 1;

            uint64_t off0 = lod.file_offset + (static_cast<uint64_t>(c0) * header_.bytes_per_chunk);
            uint64_t off1 = lod.file_offset + (static_cast<uint64_t>(c1) * header_.bytes_per_chunk);

            WaveformPointStereo pt0{}, pt1{};
            file_->seek(off0);
            file_->read(reinterpret_cast<uint8_t*>(&pt0), sizeof(pt0));
            file_->seek(off1);
            file_->read(reinterpret_cast<uint8_t*>(&pt1), sizeof(pt1));

            int8_t l_min = static_cast<int8_t>(std::round((1.0f - alpha) * pt0.left_min + alpha * pt1.left_min));
            int8_t l_max = static_cast<int8_t>(std::round((1.0f - alpha) * pt0.left_max + alpha * pt1.left_max));
            int8_t r_min = static_cast<int8_t>(std::round((1.0f - alpha) * pt0.right_min + alpha * pt1.right_min));
            int8_t r_max = static_cast<int8_t>(std::round((1.0f - alpha) * pt0.right_max + alpha * pt1.right_max));

            out_points[i] = {l_min, l_max, r_min, r_max};
        }
    }
    return num_points;
}

size_t PreviewReader::read_preview(uint32_t start_ms, uint32_t duration_ms,
                                   WaveformPointMono* out_points, size_t num_points) {
    if (!out_points || num_points == 0) return 0;
    if (header_.channels > 1) {
        // Query stereo and downmix to mono
        for (size_t i = 0; i < num_points; ++i) {
            WaveformPointStereo s_pt{};
            read_preview_stereo(start_ms + static_cast<uint32_t>((static_cast<float>(i) * duration_ms) / num_points),
                                static_cast<uint32_t>(static_cast<float>(duration_ms) / num_points),
                                &s_pt, 1);
            out_points[i].min = std::min(s_pt.left_min, s_pt.right_min);
            out_points[i].max = std::max(s_pt.left_max, s_pt.right_max);
        }
        return num_points;
    }

    // Direct mono read
    float ms_per_point = static_cast<float>(duration_ms) / static_cast<float>(num_points);
    uint8_t lod_idx = select_lod(ms_per_point);
    const ApvLodDescriptor& lod = header_.lods[lod_idx];
    if (lod.chunk_count == 0) return 0;

    float frames_per_chunk = static_cast<float>(header_.samples_per_base_chunk) * lod.downsample_ratio;
    float ms_per_chunk = (frames_per_chunk * 1000.0f) / header_.sample_rate;
    float start_chunk_f = static_cast<float>(start_ms) / ms_per_chunk;
    float chunk_span = static_cast<float>(duration_ms) / ms_per_chunk;
    float chunks_per_point = chunk_span / static_cast<float>(num_points);

    for (size_t i = 0; i < num_points; ++i) {
        if (chunks_per_point >= 1.0f) {
            uint32_t c_start = static_cast<uint32_t>(start_chunk_f + i * chunks_per_point);
            uint32_t c_end = static_cast<uint32_t>(start_chunk_f + (i + 1) * chunks_per_point);
            if (c_start >= lod.chunk_count) c_start = lod.chunk_count - 1;
            if (c_end >= lod.chunk_count) c_end = lod.chunk_count - 1;
            if (c_end < c_start) c_end = c_start;

            int8_t m_min = 0, m_max = 0;
            for (uint32_t c = c_start; c <= c_end; ++c) {
                uint64_t off = lod.file_offset + (static_cast<uint64_t>(c) * header_.bytes_per_chunk);
                file_->seek(off);
                WaveformPointMono pt{};
                file_->read(reinterpret_cast<uint8_t*>(&pt), sizeof(pt));
                if (pt.min < m_min) m_min = pt.min;
                if (pt.max > m_max) m_max = pt.max;
            }
            out_points[i] = {m_min, m_max};
        } else {
            float c_f = start_chunk_f + i * chunks_per_point;
            uint32_t c0 = static_cast<uint32_t>(std::floor(c_f));
            uint32_t c1 = c0 + 1;
            float alpha = c_f - static_cast<float>(c0);

            if (c0 >= lod.chunk_count) c0 = lod.chunk_count - 1;
            if (c1 >= lod.chunk_count) c1 = lod.chunk_count - 1;

            uint64_t off0 = lod.file_offset + (static_cast<uint64_t>(c0) * header_.bytes_per_chunk);
            uint64_t off1 = lod.file_offset + (static_cast<uint64_t>(c1) * header_.bytes_per_chunk);

            WaveformPointMono pt0{}, pt1{};
            file_->seek(off0);
            file_->read(reinterpret_cast<uint8_t*>(&pt0), sizeof(pt0));
            file_->seek(off1);
            file_->read(reinterpret_cast<uint8_t*>(&pt1), sizeof(pt1));

            int8_t m_min = static_cast<int8_t>(std::round((1.0f - alpha) * pt0.min + alpha * pt1.min));
            int8_t m_max = static_cast<int8_t>(std::round((1.0f - alpha) * pt0.max + alpha * pt1.max));
            out_points[i] = {m_min, m_max};
        }
    }
    return num_points;
}

} // namespace audio_codecs::preview
```

Update `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`:
```cmake
# Preview library
add_library(audio_codecs_preview STATIC
    src/preview/preview_stream.cpp
    src/preview/preview_generator.cpp
    src/preview/preview_reader.cpp
)
target_link_libraries(audio_codecs_preview PUBLIC audio_codecs_core)

add_executable(test_preview_reader tests/test_preview_reader.cpp)
target_link_libraries(test_preview_reader PRIVATE audio_codecs_preview)
add_test(NAME PreviewReaderTest COMMAND test_preview_reader)
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cmake -B /Users/moolet/Development/github/newdigate/audio-codecs/build -S /Users/moolet/Development/github/newdigate/audio-codecs && cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_preview_reader && /Users/moolet/Development/github/newdigate/audio-codecs/build/test_preview_reader
```
Expected: PASS (`test_preview_reader PASSED`).

- [ ] **Step 5: Commit**

```bash
git -C /Users/moolet/Development/github/newdigate/audio-codecs add include/audio_codecs/preview/preview_reader.h src/preview/preview_reader.cpp tests/test_preview_reader.cpp CMakeLists.txt
git -C /Users/moolet/Development/github/newdigate/audio-codecs commit -m "feat(preview): implement PreviewReader with adaptive LOD selection and peak decimation"
```

---

### Task 5: End-to-End Integration, Umbrella Header & Hardware Verification (`preview.h`, `test_preview_integration.cpp`)

**Files:**
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/preview.h`
- Modify: `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/audio_codecs.h`
- Create: `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_preview_integration.cpp`
- Modify: `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`

**Interfaces:**
- Consumes:
  - `audio_codecs/wav/wav_decoder.h`
  - `audio_codecs/wav/wav_encoder.h`
  - `audio_codecs/preview.h`
- Produces:
  - Umbrella header `#include "audio_codecs/preview.h"` exposed in `audio_codecs.h`
  - Integration test verifying audio decode $\to$ preview generation $\to$ query roundtrip

- [ ] **Step 1: Write the failing test**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/tests/test_preview_integration.cpp`:
```cpp
#include "audio_codecs/audio_codecs.h"
#include <cassert>
#include <cmath>
#include <vector>
#include <iostream>

using namespace audio_codecs;
using namespace audio_codecs::preview;

int main() {
    // 1. Synthesize 5 seconds of 44.1 kHz stereo sine wave with decay
    constexpr uint32_t sample_rate = 44100;
    constexpr size_t num_frames = sample_rate * 5;
    std::vector<int16_t> pcm(num_frames * 2);
    for (size_t f = 0; f < num_frames; ++f) {
        float t = static_cast<float>(f) / sample_rate;
        float env = 1.0f - (static_cast<float>(f) / num_frames); // linear decay
        int16_t val = static_cast<int16_t>(std::sin(2.0f * 3.14159f * 440.0f * t) * 30000.0f * env);
        pcm[f * 2] = val;     // left
        pcm[f * 2 + 1] = -val;// right
    }

    // 2. Generate .apv preview
    MemoryReader pcm_reader(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * sizeof(int16_t));
    std::vector<uint8_t> apv_storage(1024 * 64, 0);
    MemoryWriter apv_writer(apv_storage.data(), apv_storage.size());

    PreviewGenerator gen;
    bool ok = gen.init(pcm_reader, apv_writer, nullptr, sample_rate, 2, true);
    assert(ok);
    assert(gen.generate_all());

    // 3. Read back preview
    MemoryReader apv_reader(apv_storage.data(), apv_writer.size());
    PreviewReader pr;
    assert(pr.init(apv_reader));
    assert(pr.duration_ms() == 5000);

    // Verify decay envelope across 10 query bins
    WaveformPointStereo points[10];
    assert(pr.read_preview_stereo(0, 5000, points, 10) == 10);
    // As time progresses, peak amplitude must decrease
    for (size_t i = 1; i < 10; ++i) {
        assert(points[i].left_max <= points[i - 1].left_max);
    }

    std::cout << "test_preview_integration PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_preview_integration
```
Expected: FAIL (missing `preview.h` in `audio_codecs.h`).

- [ ] **Step 3: Write minimal implementation**

Create `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/preview.h`:
```cpp
#pragma once
#include "audio_codecs/preview/preview_types.h"
#include "audio_codecs/preview/preview_stream.h"
#include "audio_codecs/preview/preview_generator.h"
#include "audio_codecs/preview/preview_reader.h"
#include "audio_codecs/preview/teensy_stream_adapter.h"
```

Add `#include "audio_codecs/preview.h"` to `/Users/moolet/Development/github/newdigate/audio-codecs/include/audio_codecs/audio_codecs.h`.

Update `/Users/moolet/Development/github/newdigate/audio-codecs/CMakeLists.txt`:
```cmake
add_executable(test_preview_integration tests/test_preview_integration.cpp)
target_link_libraries(test_preview_integration PRIVATE audio_codecs_preview audio_codecs_wav)
add_test(NAME PreviewIntegrationTest COMMAND test_preview_integration)
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cmake -B /Users/moolet/Development/github/newdigate/audio-codecs/build -S /Users/moolet/Development/github/newdigate/audio-codecs && cmake --build /Users/moolet/Development/github/newdigate/audio-codecs/build --target test_preview_integration && ctest --test-dir /Users/moolet/Development/github/newdigate/audio-codecs/build --output-on-failure
```
Expected: PASS (100% tests passed including all 4 new preview tests).

- [ ] **Step 5: Commit**

```bash
git -C /Users/moolet/Development/github/newdigate/audio-codecs add include/audio_codecs/preview.h include/audio_codecs/audio_codecs.h tests/test_preview_integration.cpp CMakeLists.txt
git -C /Users/moolet/Development/github/newdigate/audio-codecs commit -m "feat(preview): expose umbrella preview.h and add end-to-end integration test"
```
