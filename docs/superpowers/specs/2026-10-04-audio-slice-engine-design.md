# Audio Slice Engine & ASL File Format Specification

## Executive Summary

This document specifies the architecture, algorithmic pipeline, and binary file format (**ASL1** / `.asl` — Audio Slice List) for cross-platform audio slice detection, beat-quantized loop slicing, tempo-independent playback, and MIDI file export in `audio-codecs`.

Targeting both desktop systems and resource-constrained microcontrollers (such as ARM Cortex-M7 on NXP i.MX RT1176 / Teensy 4.x), the audio slice engine provides an open, modern, MCU-friendly alternative to legacy proprietary slice formats (such as Propellerhead REX / REX2 `.rex` / `.rx2`):
1. **Companion Sidecar Architecture (`.asl`)**: Accompanying existing standard audio containers (`.wav`, `.flac`, `.mp3`), matching the design pattern of `.apv` (peaks), `.asv` (spectrum), and `.att` (tempo).
2. **Fixed-Size Per-Slice Descriptor (`AslSlice`, 32 bytes packed)**: Stores sample boundaries (`start_sample`, `length_samples`), musical position (`musical_tick` at $PPQN = 480$, `bar_index`, `beat_within_bar`, `subdivision`), MIDI trigger note (`midi_note`, starting at 36 / C1), gain/velocity, transient energy, and playback envelope hints (`decay_ms`, `tail_mode`).
3. **MCU-Optimized Time-Domain Transient Detector**: Operates without FFTs using 1st-order pre-emphasis filtering ($y[n] = x[n] - 0.95 x[n-1]$), micro-hop absolute energy novelty ($H = 32$ samples, $\sim 0.725\text{ ms}$ @ $44.1\text{ kHz}$), rolling adaptive dynamic thresholding with sensitivity control ($0\text{--}100\%$), and backward zero-crossing snapping (32–64 samples) to guarantee click-free attacks while retaining transient punch. Operates in $< 200\text{ bytes}$ RAM.
4. **Multi-Mode Slicing Engine**:
   - **Pure Transient Mode (`ASL_MODE_TRANSIENT`)**: Detects freeform percussive and acoustic hits.
   - **Metric Grid Mode (`ASL_MODE_GRID`)**: Divides audio into strict uniform subdivisions (1/4, 1/8, 1/16, 1/32).
   - **Transient-to-Grid Quantization (`ASL_MODE_TRANSIENT_TO_GRID`)**: Snaps detected transients to the nearest musical metric ticks ($PPQN = 480$), calculating bars, beats, and subdivisions, resolving gaps and micro-timing jitter (ReCycle style).
5. **Interactive Sampler Playback & Tempo-Stretching**: Slices can be played at any target BPM without time-stretching pitch artifacts:
   - Slices re-trigger at the new tempo's musical intervals.
   - Faster tempo: 2.5 ms choke/crossfade envelope smoothly truncates tails without clicking.
   - Slower tempo: Synthetic exponential decay tails or sustain micro-loops bridge gaps between beats.
   - Micro-fades: 1.0 ms linear/cosine attack curve prevents DC offset pops.
6. **Standard MIDI Type 0 File Export (`.mid`)**: Produces chromatic trigger sequences at $PPQN = 480$, enabling drag-and-drop into DAWs and hardware sequencers.
7. **Guarantees Zero Dynamic Allocations**: Constant memory footprint during steady-state analysis and playback; cooperative sliced non-blocking `step()` API for RTOS audio and UI threads.

---

## 1. System Architecture & Data Flow

```
                                  +---------------------------+
                                  |   Decoded PCM Audio Stream|
                                  +-------------+-------------+
                                                |
                                                v
                                  +---------------------------+
                                  |     SliceDetector         |
                                  |   (audio_codecs::slice)   |
                                  |  - Pre-emphasis (1st-ord) |
                                  |  - 32-sample Micro-Hops   |
                                  |  - Adaptive Threshold     |
                                  |  - Zero-Crossing Snapping |
                                  +-------------+-------------+
                                                |
                                                v (Raw Onset Markers)
+---------------------------+     +---------------------------+
| .att (ATT1) Tempo File    | --> |     SliceGenerator        |
| (Optional Tempo & Grid)   |     |  - Mode Selection         |
+---------------------------+     |  - Metric Quantization    |
                                  |  - Musical Tick Alignment |
                                  |  - Envelope Profiling     |
                                  +-------------+-------------+
                                                |
                                                v
                                  +---------------------------+
                                  | .asl (ASL1) Slice File    |
                                  |  Header (128 B)           |
                                  |  Slice Descriptors (32 B) |
                                  +-------------+-------------+
                                                |
                        +-----------------------+-----------------------+
                        |                                               |
                        v                                               v
          +---------------------------+                   +---------------------------+
          |     SliceReader           |                   |    SliceMidiWriter        |
          |  - O(1) Slice Index Lookup|                   |  - Standard Type 0 .mid   |
          |  - O(log N) Time / Tick   |                   |  - PPQN = 480             |
          |  - Non-allocating queries |                   |  - Chromatic Note-On/Off  |
          +-------------+-------------+                   +---------------------------+
                        |
                        v
          +---------------------------+
          |     SliceVoiceHelper      |
          |  - Tempo-Stretched Trigger|
          |  - Anti-Click Micro-Fades |
          |  - Tail Loop / Synth Decay|
          +---------------------------+
```

---

## 2. Binary File Format Specification (`.asl` / ASL1)

The file format uses a fixed 128-byte header followed immediately by an array of 32-byte slice descriptors. All multi-byte numerical fields are serialized in Little-Endian byte order.

### 2.1 File Layout Overview

```
+-------------------------------------------------------------+
| Header: AslHeader (128 bytes)                               |
| Magic: 'ASL1' (0x314C5341), Version: 1                      |
| Slices Offset: 128, Total Slices: N                         |
+-------------------------------------------------------------+
| Slice 0: AslSlice (32 bytes)                                |
| (start_sample, length, tick, midi_note, bar, beat, ...)     |
+-------------------------------------------------------------+
| Slice 1: AslSlice (32 bytes)                                |
+-------------------------------------------------------------+
| ...                                                         |
+-------------------------------------------------------------+
| Slice N-1: AslSlice (32 bytes)                              |
+-------------------------------------------------------------+
```

### 2.2 `AslHeader` (128 Bytes Packed)

```cpp
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
    uint8_t  slice_mode;            // 0 = Transient, 1 = Grid, 2 = Quantized
    uint32_t original_bpm_q16;      // Original tempo in Q16 fixed-point (e.g. 120.0 << 16)
    uint16_t ppqn;                  // Pulses Per Quarter Note (standard: 480)
    uint16_t num_bars;              // Total bars spanned by loop
    uint16_t total_slices;          // Total number of slices in file
    uint16_t slice_descriptor_size; // Size of each descriptor: 32 bytes
    uint32_t slices_offset;         // File offset to slice descriptors: 128
    uint16_t flags;                 // Bit 0: loopable, Bit 1: has_sustain_loops
    uint8_t  reserved[80];          // Zero-padded future expansion
};
static_assert(sizeof(AslHeader) == 128, "AslHeader must be exactly 128 bytes");
#pragma pack(pop)
```

### 2.3 `AslSlice` (32 Bytes Packed)

```cpp
#pragma pack(push, 1)
struct AslSlice {
    uint32_t start_sample;          // Sample offset from start of audio
    uint32_t length_samples;        // Length of slice in PCM frames
    uint32_t musical_tick;          // Musical position in PPQN ticks from start
    uint8_t  midi_note;             // Chromatic MIDI note trigger (36 = C1, 37 = C#1...)
    uint8_t  bar_index;             // Bar index (0-indexed)
    uint8_t  beat_within_bar;       // Beat index within bar (0-indexed: 0..num-1)
    uint8_t  subdivision;           // 16th/32nd subdivision within beat
    int16_t  gain_db_q8;            // Recommended gain trim in Q8 dB (0 = 0 dB)
    uint16_t transient_energy;      // Peak transient energy novelty (0..65535)
    uint16_t decay_ms;              // Natural decay time in milliseconds
    uint8_t  tail_mode;             // 0 = OneShot, 1 = StretchDecay, 2 = SustainLoop
    uint8_t  flags;                 // Bit 0: locked, Bit 1: reversed, Bit 2: muted
    uint8_t  reserved[8];           // Zero-padded alignment
};
static_assert(sizeof(AslSlice) == 32, "AslSlice must be exactly 32 bytes");
#pragma pack(pop)
```

---

## 3. MCU Transient Analysis & Zero-Crossing Engine

Transient detection runs directly in the time domain without requiring an FFT, maximizing execution speed on microcontrollers ($< 200\text{ bytes}$ of RAM state, zero heap allocations).

### 3.1 Pre-Emphasis High-Pass Filter

Transients (drum stick hits, pick attacks, cymbal pings) are predominantly high-frequency energy. Low-frequency energy (basslines, kick boom) can artificially mask sharp transients.
A simple first-order high-pass difference filter pre-emphasizes the high frequencies:

$$y[n] = x[n] - \alpha \cdot x[n-1], \quad \alpha \approx 0.95$$

In 16-bit integer or float arithmetic:
```cpp
float hp = sample - 0.95f * prev_sample;
prev_sample = sample;
```

### 3.2 Micro-Hop Absolute Energy Novelty

The audio is accumulated into tiny non-overlapping micro-hops of $H = 32\text{ samples}$ ($\approx 0.725\text{ ms}$ at $44.1\text{ kHz}$):

$$E[m] = \sum_{k=0}^{H-1} |y[m \cdot H + k]|$$

The micro-hop energy novelty $N[m]$ is the half-wave rectified forward difference:

$$N[m] = \max(0, E[m] - E[m-1])$$

### 3.3 Rolling Adaptive Dynamic Thresholding

To reliably isolate true transients from sustained high energy or noisy backgrounds, a rolling local average $\mu[m]$ and variance $\sigma[m]$ are maintained across a window of $W = 16\text{ hops}$ ($\sim 11.6\text{ ms}$):

$$T[m] = \mu[m] + k_{\text{sens}} \cdot \sigma[m]$$

Where $k_{\text{sens}}$ is mapped from the user sensitivity setting ($0\text{--}100\%$, default $50\%$).
A transient candidate is flagged whenever $N[m] > T[m]$.

### 3.4 Minimum Slice Distance

To prevent double-triggering or "chattering" on complex multi-stage drum hits (e.g. flam snares or cymbal buzzes), a configurable lockout refractory period is enforced:
* Default: $16\text{ micro-hops}$ ($\sim 11.6\text{ ms}$, or $512\text{ samples}$ at $44.1\text{ kHz}$).
* Candidates occurring within this lockout window are discarded unless their novelty exceeds the previous peak by $> 6\text{ dB}$.

### 3.5 Backward Zero-Crossing Snapping

A naive transient detector marks the boundary at or near the energy peak, which:
1. Truncates the initial attack transient (dulling the punch).
2. Starts playback at a non-zero sample value, causing an audible DC click.

The engine searches backward from the detected micro-hop peak by up to $32\text{--}64\text{ samples}$ to locate the last zero-crossing:

$$x[n-1] \cdot x[n] \le 0$$

The slice's `start_sample` is snapped precisely to this zero-crossing index.

---

## 4. Slicing Modes & Musical Grid Quantization

### 4.1 Slicing Modes

```cpp
enum AslSliceMode : uint8_t {
    ASL_MODE_TRANSIENT        = 0,  // Pure transient detection (sound fx, freeform hits)
    ASL_MODE_GRID             = 1,  // Strict uniform musical division (1/4, 1/8, 1/16, 1/32)
    ASL_MODE_TRANSIENT_TO_GRID= 2   // Transients snapped to metric musical grid (ReCycle style)
};
```

### 4.2 Musical Grid & PPQN Alignment

* **Standard PPQN**: $480\text{ pulses per quarter note}$.
  * Whole Note ($4\text{ beats}$): $1920\text{ ticks}$.
  * Quarter Note ($1\text{ beat}$): $480\text{ ticks}$.
  * 8th Note: $240\text{ ticks}$.
  * 16th Note: $120\text{ ticks}$.
  * 32nd Note: $60\text{ ticks}$.
  * 8th Triplet: $160\text{ ticks}$.
  * 16th Triplet: $80\text{ ticks}$.

### 4.3 Transient-to-Grid Quantization Algorithm

1. **Calculate Musical Grid Ticks**: Given loop duration, tempo ($BPM$), and time signature (default 4/4), compute exact tick intervals for the desired target resolution (e.g. 1/16 note = 120 ticks).
2. **Nearest Metric Match**: For each detected transient at sample $S_i$, convert sample position to musical tick $T_i$:
   $$T_i = \left\lfloor \frac{S_i \cdot \text{sample\_rate}^{-1} \cdot BPM \cdot PPQN}{60.0} + 0.5 \right\rfloor$$
3. **Snap Window**: If $T_i$ is within a tolerance window (default $\pm 50\%$ of grid subdivision) of a metric grid tick $G_k$, snap slice to $G_k$.
4. **Duplicate & Gap Resolution**:
   * If two transients snap to the same grid tick, keep the stronger transient on the grid tick and assign the second to an unquantized sub-tick (or preserve as a 32nd/64th ghost note).
   * Any metric grid ticks lacking a detected transient can be filled with a slice starting at the interpolated sample position to preserve continuous sequence playback.
5. **Bar and Beat Computation**:
   $$\text{ticks\_per\_bar} = PPQN \cdot 4 \cdot \frac{\text{num}}{\text{denom}}$$
   $$\text{bar\_index} = \frac{\text{tick}}{\text{ticks\_per\_bar}}, \quad \text{beat\_within\_bar} = \frac{\text{tick} \pmod{\text{ticks\_per\_bar}}}{PPQN}$$

---

## 5. Sampler Playback & Tempo-Stretching Model

### 5.1 Playback Principles

When playing an audio loop via slices:
* **Original BPM**: Slices trigger sequentially at their original sample intervals, perfectly reconstructing the source loop.
* **Faster BPM ($BPM_{\text{target}} > BPM_{\text{original}}$)**: Slice trigger intervals are compressed. Slices overlap in time. The previous slice is cleanly choked by the new slice.
* **Slower BPM ($BPM_{\text{target}} < BPM_{\text{original}}$)**: Slice trigger intervals are expanded. Slices finish before the next one is triggered, creating silent gaps. The engine bridges these gaps using envelope extensions.

```
Original BPM (Slices tile seamlessly):
| Slice 0 (Kick)        | Slice 1 (Hat) | Slice 2 (Snare)       |

Faster BPM (Next slice chokes previous tail early):
| Slice 0 (Kick)   | Slice 1 (Hat)  | Slice 2 (Snare)   |
                  [Choke crossfade: 2.5 ms]

Slower BPM (Tail extension bridges the gap):
| Slice 0 (Kick)        |...Tail...| Slice 1 (Hat) |...Tail...| Slice 2 (Snare)
                        [Decay/Loop]               [Decay/Loop]
```

### 5.2 Anti-Click Micro-Fades & Choke Envelope

* **Attack Envelope**: $1.0\text{ ms}$ linear or half-cosine attack ramp at the beginning of each slice to ensure zero DC offset pop when triggered.
* **Choke / Release Envelope**: $2.5\text{ ms}$ crossfade fade-out when a previous voice is interrupted by a new slice on the same playback group.

### 5.3 Tail Extension Modes (`tail_mode`)

```cpp
enum AslTailMode : uint8_t {
    ASL_TAIL_ONE_SHOT      = 0,  // Natural sample end (no synthetic extension)
    ASL_TAIL_STRETCH_DECAY = 1,  // Synthetic exponential decay tail
    ASL_TAIL_SUSTAIN_LOOP  = 2   // Seamless micro-loop crossfade in body/tail
};
```

1. **`ASL_TAIL_ONE_SHOT`**: Default for tight percussion or when user desires silence between beats (gated/staccato feel).
2. **`ASL_TAIL_STRETCH_DECAY`**: The final $30\text{--}50\text{ ms}$ of the slice is extended with an exponential decay curve to fill the time gap naturally without jarring cutoffs.
3. **`ASL_TAIL_SUSTAIN_LOOP`**: For tonal or ambient slices, a $10\text{--}20\text{ ms}$ sustain loop with alternating ping-pong or crossfade boundaries loops continuously until the next slice triggers.

### 5.4 Standard MIDI Type 0 File Export (`.mid`)

For every `.asl` slice list, standard MIDI Type 0 files can be generated:
* Format: Standard MIDI File (SMF) Type 0 (single multi-channel track).
* Header: $PPQN = 480$.
* Initial Meta Events:
  * Time Signature Meta Event (`0xFF 0x58 0x04 num denom 24 8`)
  * Set Tempo Meta Event (`0xFF 0x51 0x03 microseconds_per_quarter`)
* Track Events:
  * Chromatic Note-On and Note-Off events starting at MIDI Note 36 (C1), ascending sequentially (36, 37, 38...).
  * Note-On velocity computed from `gain_db_q8` and `transient_energy`.
  * Note duration matches slice length in ticks (or up to the start of the next slice).
  * End of Track Meta Event (`0xFF 0x2F 0x00`).

---

## 6. API & Component Architecture

All components reside in namespace `audio_codecs::slice`.

### 6.1 Directory & File Layout

```
include/audio_codecs/slice/
  slice_types.h        // Structs, enums, constants, static_asserts
  slice_detector.h     // Time-domain transient detector
  slice_generator.h    // Sliced generation pipeline (.asl writer)
  slice_reader.h       // Zero-heap random access reader & query interface
  slice_playback.h     // Voice playback helper (choke, tail, anti-click)
  slice_midi_writer.h  // Standard Type 0 MIDI file exporter
  slice.h              // Umbrella include for audio_codecs::slice

src/slice/
  slice_detector.cpp
  slice_generator.cpp
  slice_reader.cpp
  slice_playback.cpp
  slice_midi_writer.cpp

tests/
  test_slice_types.cpp
  test_slice_detector.cpp
  test_slice_modes.cpp
  test_slice_generator.cpp
  test_slice_reader.cpp
  test_slice_playback.cpp
  test_slice_integration.cpp
```

### 6.2 Key Classes and Method Signatures

#### `SliceDetector`
```cpp
namespace audio_codecs::slice {

struct DetectorConfig {
    float sensitivity = 0.5f;       // 0.0f (least sensitive) to 1.0f (most sensitive)
    uint32_t min_slice_samples = 512; // Minimum distance between slices (~11.6 ms @ 44.1k)
    float pre_emphasis_alpha = 0.95f;
    uint32_t sample_rate = 44100;
};

class SliceDetector {
public:
    SliceDetector();
    bool init(const DetectorConfig& config);
    void reset();
    
    // Process block of PCM samples (16-bit mono or left channel)
    // Returns number of transients detected in this block
    uint32_t process_block(const int16_t* pcm_samples, uint32_t count,
                           uint32_t* out_transient_indices, uint32_t max_indices);

    // Backward zero-crossing search helper
    static uint32_t find_zero_crossing_backward(const int16_t* pcm_samples,
                                                uint32_t peak_index,
                                                uint32_t max_search_samples);
};

} // namespace audio_codecs::slice
```

#### `SliceGenerator`
```cpp
namespace audio_codecs::slice {

struct GeneratorConfig {
    AslSliceMode mode = ASL_MODE_TRANSIENT_TO_GRID;
    uint32_t sample_rate = 44100;
    uint8_t channels = 2;
    uint8_t time_sig_num = 4;
    uint8_t time_sig_denom = 4;
    uint32_t bpm_q16 = 120 << 16;
    float sensitivity = 0.5f;
    uint16_t max_slices = 256;
};

class SliceGenerator {
public:
    SliceGenerator();
    bool init(const GeneratorConfig& config);
    
    // Feed audio in chunks
    bool process_pcm(const int16_t* pcm, uint32_t sample_count);
    
    // Finalize slicing, snap to grid (if mode != TRANSIENT), and write to stream
    bool finalize();
    
    // Cooperative step API for RTOS
    bool step(uint32_t budget_us);
    bool is_complete() const;

    // Serialize to writer stream or memory buffer
    bool serialize(void* out_buffer, size_t buffer_size, size_t* out_bytes_written) const;
};

} // namespace audio_codecs::slice
```

#### `SliceReader`
```cpp
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
};

} // namespace audio_codecs::slice
```

#### `SliceVoiceHelper`
```cpp
namespace audio_codecs::slice {

class SliceVoiceHelper {
public:
    // Compute render parameters for playing a slice at target BPM
    static void compute_playback_params(const AslSlice& slice,
                                        uint32_t original_bpm_q16,
                                        uint32_t target_bpm_q16,
                                        uint32_t sample_rate,
                                        uint32_t* out_trigger_interval_samples,
                                        uint32_t* out_render_samples,
                                        bool* out_needs_tail_extension);

    // Apply 1.0 ms attack anti-click fade in-place
    static void apply_attack_ramp(int16_t* pcm, uint32_t count, uint32_t sample_rate);

    // Apply 2.5 ms choke crossfade in-place
    static void apply_choke_ramp(int16_t* pcm, uint32_t count, uint32_t sample_rate);
};

} // namespace audio_codecs::slice
```

#### `SliceMidiWriter`
```cpp
namespace audio_codecs::slice {

class SliceMidiWriter {
public:
    // Write standard Type 0 MIDI file from slice table
    static bool write_type0_midi(const AslHeader& header,
                                 const AslSlice* slices,
                                 uint16_t total_slices,
                                 uint8_t* out_midi_buf,
                                 size_t max_buf_size,
                                 size_t* out_bytes_written);
};

} // namespace audio_codecs::slice
```

---

## 7. Testing & Verification Plan

### 7.1 Unit Tests
* **`test_slice_types.cpp`**:
  * Verify `sizeof(AslHeader) == 128` and `sizeof(AslSlice) == 32` via compile-time `static_assert` and runtime validation.
  * Test header serialization, magic number `'ASL1'`, endianness, and corrupt/truncated header rejection.
* **`test_slice_detector.cpp`**:
  * Verify pre-emphasis high-pass filter ($y[n] = x[n] - 0.95 x[n-1]$) and 32-sample micro-hop energy novelty.
  * Verify backward zero-crossing snapping on synthetic sharp impulses with DC offsets ($x[n-1] \cdot x[n] \le 0$).
  * Test dynamic thresholding across synthetic impulse trains, silence, continuous sines, and noise bursts under varying sensitivities ($0\text{--}100\%$).
* **`test_slice_modes.cpp`**:
  * Verify `ASL_MODE_TRANSIENT` (freeform transients).
  * Verify `ASL_MODE_GRID` (uniform 1/4, 1/8, 1/16, 1/32 slicing).
  * Verify `ASL_MODE_TRANSIENT_TO_GRID` (metric tick snapping, bar/beat assignment, duplicate resolution).
* **`test_slice_generator.cpp`**:
  * Sliced cooperative execution across multiple simulated time-budget slices.
  * Deterministic binary output matching byte-for-byte expected layouts.
  * Buffer saturation limits (ensure graceful limit handling without crash).
* **`test_slice_reader.cpp`**:
  * Random-access slice lookup by index ($O(1)$).
  * Binary search lookups: `find_slice_at_sample()`, `find_slice_at_ms()`, and `find_slice_at_tick()` ($O(\log N)$).
  * Corrupted data and bounds checks.
* **`test_slice_playback.cpp`**:
  * Micro-fade curve validation (1.0 ms attack ramp, 2.5 ms choke ramp).
  * Tail stretching calculations for tempo deceleration ($BPM_{\text{target}} < BPM_{\text{original}}$).
  * MIDI export validation: verify standard Type 0 `.mid` format, delta times at $PPQN = 480$, tempo meta-events, and chromatic note triggers starting at C1 (36).

### 7.2 Integration & Embedded Verification
* **`test_slice_integration.cpp`**:
  * Synthetic 4-bar drum break (kick, snare, hi-hats).
  * Feed PCM through `SliceGenerator` $\rightarrow$ generate `.asl`.
  * Read back via `SliceReader` $\rightarrow$ verify exact slice boundaries and MIDI note sequence.
  * Export Type 0 MIDI $\rightarrow$ verify tick alignment against original slices.
  * Strictly enforce **no side-effects inside `assert(...)`** (`bool ok = ...; assert(ok);`).
  * Verify zero heap allocations during the steady-state `step()` loop.
