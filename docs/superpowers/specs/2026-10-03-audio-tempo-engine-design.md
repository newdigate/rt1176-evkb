# Audio Tempo Engine & ATT File Format Specification

## Executive Summary

This document specifies the architecture, algorithmic pipeline, and binary file format (**ATT1** / `.att`) for cross-platform audio tempo detection, tempo timeline generation, and beat grid tracking in `audio-codecs`.

Targeting both desktop systems and resource-constrained microcontrollers (such as ARM Cortex-M7 on NXP i.MX RT1176 / Teensy 4.x), the tempo engine:
1. **Consumes Pre-Computed Spectrum Data (`.asv`)**: Reads the 64 log-spaced frequency bands from the `audio_codecs::spectrum` engine (LOD 0, $\sim 11.61\text{ ms}$ hop rate), avoiding any redundant FFT computations or audio re-decoding.
2. **Decomposes Sub-Band Novelty**: Separates transients into **Bass Flux** ($20\text{--}160\text{ Hz}$, kick drums/808s), **Snare/Mid Flux** ($160\text{--}4000\text{ Hz}$, snares/claps), and **High Flux** ($4000\text{--}20000\text{ Hz}$, hi-hats/cymbals) to form an Onset Detection Function (ODF) with adaptive local thresholding.
3. **Induces Tempo via Comb Filter Resonator**: Evaluates periodicities across a $58\text{--}240\text{ BPM}$ range over a rolling $512$-frame window ($\sim 5.94\text{ s}$), weighted by a human perceptual prior centered at $120\text{ BPM}$ to eliminate octave/half-tempo ambiguities.
4. **Tracks Beats & Downbeats**: Employs short-horizon sliced dynamic phase alignment to detect beat timestamps, identifies downbeats (Beat 1 of each bar) via kick-snare metric contrast in 4/4 time, and interpolates through breakdowns or silent passages without phase drift.
5. **Serializes to Sector-Aligned Format (`.att`)**: Stores a 128-byte header, a piecewise linear tempo curve, and a packed beat grid in under $10\text{ KB}$ per typical song.
6. **Guarantees Zero Dynamic Allocations**: Operates with a static working state of under $3\text{ KB}$ RAM in steady state, executing cooperatively via a non-blocking `step()` API.

---

## 1. System Architecture & Data Flow

```
                                  +---------------------------+
                                  |   Decoded PCM Audio Stream|
                                  +-------------+-------------+
                                                |
                                                v
                                  +---------------------------+
                                  |     SpectrumGenerator     |
                                  |    (audio_codecs::asv)    |
                                  +-------------+-------------+
                                                |
                                                v
                                  +---------------------------+
                                  | .asv (ASV1) Spectrum File |
                                  |  64 log bands @ 86.13 Hz  |
                                  +-------------+-------------+
                                                |
                                                v
 +----------------------------------------------------------------------------------------+
 | TempoGenerator (audio_codecs::tempo)                                                   |
 |                                                                                        |
 |  [Sub-Band Flux]           [Adaptive Threshold]          [Comb Resonator & Prior]      |
 |  O_bass (0..15)    --+     O[t] - mu[t]           -->    R[tau] + 0.5*R[2tau]          |
 |  O_snare (16..42)  --+-->  (Local transient spikes)      * W_prior (120 BPM Gaussian)  |
 |  O_high (43..63)   --+                                   --> Local BPM estimate        |
 |                                                                  |                     |
 |  [Sliced Phase Alignment]  <-------------------------------------+                     |
 |  Argmax ODF with period penalty                                                        |
 |  --> Beat Timestamps (t_0, t_1, t_2...)                                                |
 |                                                                                        |
 |  [Metric Downbeat Classification]                                                      |
 |  Kick vs Snare phase correlation (phi in 0..3)                                         |
 |  --> Bar numbers, Beat 1 downbeat flag                                                 |
 +----------------------------------------------------------------------------------------+
                                                |
                                                v
                                  +---------------------------+
                                  | .att (ATT1) Timeline File |
                                  |  Header (128 B)           |
                                  |  Tempo Curve (8 B / node) |
                                  |  Beat Grid (16 B / beat)  |
                                  +-------------+-------------+
                                                |
                                                v
                                  +---------------------------+
                                  |        TempoReader        |
                                  |  - get_bpm_at(time_ms)    |
                                  |  - get_nearest_beat(...)  |
                                  |  - time_to_beat / beat... |
                                  +---------------------------+
```

---

## 2. Binary Format Specification (`.att` / `ATT1`)

The **Audio Tempo Timeline** format (`.att`) is designed for fast random access and streaming playback on embedded platforms.

### 2.1 File Layout
```
+--------------------------------------------------------------+
| Byte 0..127: AttHeader (128 bytes, sector-aligned)           |
+--------------------------------------------------------------+
| Byte 128..128 + M*8 - 1: AttTempoPoint[] (Tempo Curve)       |
| Piecewise linear tempo anchor nodes (8 bytes each)           |
+--------------------------------------------------------------+
| Byte offset (beat_grid_offset): AttBeatMarker[] (Beat Grid)  |
| Discrete musical beat events (16 bytes each)                 |
+--------------------------------------------------------------+
```

### 2.2 Header Structure (`AttHeader`, 128 bytes)
All multi-byte numeric fields are stored in Little-Endian byte order.

```cpp
#pragma pack(push, 1)

struct AttHeader {
    uint8_t  magic[4];             // Magic bytes: "ATT1" (0x31545441)
    uint16_t version;              // Format version (1)
    uint16_t header_size;          // Total header size in bytes (128)
    uint32_t duration_ms;          // Total audio duration in milliseconds
    uint32_t sample_rate;          // Audio sample rate (e.g. 44100, 48000)
    uint32_t global_bpm_q16;       // Global average BPM (16.16 fixed-point)
    uint8_t  confidence;           // Global confidence score (0 = uncertain, 255 = certain)
    uint8_t  time_signature_num;   // Meter numerator (default: 4 for 4/4)
    uint8_t  time_signature_denom; // Meter denominator (default: 4 for 4/4)
    uint8_t  flags;                // Bit 0: CONSTANT_TEMPO, Bit 1: HAS_DOWNBEATS
    uint32_t first_beat_ms;        // Millisecond timestamp of first detected beat
    uint32_t first_downbeat_ms;    // Millisecond timestamp of first Bar 1 Beat 1 downbeat
    uint32_t total_beats;          // Total number of beat markers in the file
    uint32_t total_bars;           // Total musical measures/bars detected

    // Chunk 1: Tempo Curve (piecewise linear nodes)
    uint64_t tempo_curve_offset;   // Byte offset from start of file to AttTempoPoint array
    uint32_t tempo_curve_count;    // Number of AttTempoPoint nodes
    uint16_t tempo_point_size;     // sizeof(AttTempoPoint) = 8 bytes
    uint16_t reserved0;            // Zero padding / alignment

    // Chunk 2: Beat Grid (discrete beat markers)
    uint64_t beat_grid_offset;     // Byte offset from start of file to AttBeatMarker array
    uint32_t beat_grid_count;      // Number of AttBeatMarker entries (= total_beats)
    uint16_t beat_marker_size;     // sizeof(AttBeatMarker) = 16 bytes
    uint16_t reserved1;            // Zero padding / alignment

    uint8_t  reserved[60];         // Zero-padded reserved block to reach 128 bytes
};
static_assert(sizeof(AttHeader) == 128, "AttHeader must be exactly 128 bytes");

#pragma pack(pop)
```

#### Header Flags
| Bit Mask | Name | Description |
| :--- | :--- | :--- |
| `0x01` | `ATT_FLAG_CONSTANT_TEMPO` | Track has steady tempo with minimal drift ($< 0.5\text{ BPM}$). |
| `0x02` | `ATT_FLAG_HAS_DOWNBEATS` | Downbeats and bar boundaries were successfully detected. |
| `0x04` | `ATT_FLAG_USER_MODIFIED` | File was edited or re-anchored manually. |

---

### 2.3 Tempo Curve Node (`AttTempoPoint`, 8 bytes)
Represents a control point on the continuous tempo vs. time graph:

```cpp
#pragma pack(push, 1)

struct AttTempoPoint {
    uint32_t time_ms;              // Millisecond timestamp of tempo anchor
    uint32_t bpm_q16;              // Local tempo in 16.16 fixed-point (e.g. 128.0 = 0x00800000)
};
static_assert(sizeof(AttTempoPoint) == 8, "AttTempoPoint must be exactly 8 bytes");

#pragma pack(pop)
```

* **Constant Tempo Tracks**: Contain only 1 node at `time_ms = 0`.
* **Drifting or Transition Tracks**: Contain nodes wherever instantaneous tempo changes by $\ge 0.25\text{ BPM}$. Instantaneous tempo between nodes is evaluated via linear interpolation.

---

### 2.4 Beat Marker (`AttBeatMarker`, 16 bytes)
Represents an individual musical beat:

```cpp
enum AttBeatFlags : uint16_t {
    ATT_BEAT_FLAG_DOWNBEAT     = 0x0001, // Beat 1 of a measure/bar
    ATT_BEAT_FLAG_INTERPOLATED = 0x0002, // Inferred during breakdown/silence
    ATT_BEAT_FLAG_USER_EDITED  = 0x0004, // User manually moved or placed
    ATT_BEAT_FLAG_LOW_CONF     = 0x0008  // Rhythmic ambiguity at this beat
};

#pragma pack(push, 1)

struct AttBeatMarker {
    uint32_t time_ms;              // Millisecond timestamp of the beat
    uint32_t bar_index;            // 1-indexed bar/measure count (1, 2, 3...)
    uint16_t beat_within_bar;      // 1-indexed beat in bar (1..4 in 4/4)
    uint16_t flags;                // Bitmask of AttBeatFlags
    uint32_t local_bpm_q16;        // Instantaneous local tempo at this beat (16.16)
};
static_assert(sizeof(AttBeatMarker) == 16, "AttBeatMarker must be exactly 16 bytes");

#pragma pack(pop)
```

---

## 3. Sub-Band Novelty & Onset Detection

### 3.1 Input Characteristics
The engine reads LOD 0 of `.asv` files:
* Hop size: $512\text{ samples}$
* Audio sample rate $f_s$: typically $44100\text{ Hz}$ or $48000\text{ Hz}$
* Spectral frame rate: $F_r = \frac{f_s}{512} \approx 86.133\text{ Hz}$ ($\sim 11.61\text{ ms}$ per frame).
* Band format: $64$ log-spaced bands ($20\text{ Hz to } 20\text{ kHz}$), quantized to unsigned 8-bit dB energy ($[0, 255] \implies [-96, 0]\text{ dBFS}$).

### 3.2 Sub-Band Partitioning
The 64 bands are partitioned into three acoustic functional ranges:
1. **Bass Flux ($O_{\text{bass}}$)** (Bands 0–15, $20\text{--}160\text{ Hz}$): Captures kick drum fundamentals, 808 sub hits, and bass note attacks.
2. **Snare/Mid Flux ($O_{\text{snare}}$)** (Bands 16–42, $160\text{--}4000\text{ Hz}$): Captures snares, claps, rimshots, guitar plucks, and vocal consonants.
3. **High Flux ($O_{\text{high}}$)** (Bands 43–63, $4000\text{--}20000\text{ Hz}$): Captures hi-hats, shakers, and cymbal strikes.

### 3.3 Novelty Calculation
For frame $t$ and band $b$:
1. **Half-Wave Rectification**:
   $$\Delta E[b, t] = \max(0, E[b, t] - E[b, t-1])$$
2. **Band Aggregation**:
   $$O_{\text{bass}}[t] = \sum_{b=0}^{15} \Delta E[b, t], \quad O_{\text{snare}}[t] = \sum_{b=16}^{42} \Delta E[b, t], \quad O_{\text{high}}[t] = \sum_{b=43}^{63} \Delta E[b, t]$$
3. **Weighted Master Novelty**:
   $$O[t] = 1.0 \cdot O_{\text{bass}}[t] + 0.8 \cdot O_{\text{snare}}[t] + 0.2 \cdot O_{\text{high}}[t]$$
4. **Adaptive Local Thresholding**:
   A moving average $\mu[t]$ of $O[t]$ over $9\text{ frames}$ ($\sim 104\text{ ms}$) is subtracted to produce a clean onset pulse train:
   $$\tilde{O}[t] = \max(0, O[t] - \mu[t])$$

---

## 4. Tempo Induction & Comb Resonator

### 4.1 Lag Period Bounds
Supporting tempos from $58\text{ to } 240\text{ BPM}$ at frame rate $F_r \approx 86.133\text{ Hz}$:
$$\tau_{\min} = \left\lfloor \frac{60 \cdot F_r}{240} \right\rfloor = 21\text{ frames}, \quad \tau_{\max} = \left\lceil \frac{60 \cdot F_r}{58} \right\rceil = 89\text{ frames}$$

### 4.2 Multi-Harmonic Comb Resonance
Within a rolling analysis window of $W = 512\text{ frames}$ ($\sim 5.94\text{ seconds}$):
1. **Autocorrelation**:
   $$R[\tau] = \sum_{k=0}^{W - 1 - \tau} \tilde{O}[k] \cdot \tilde{O}[k + \tau]$$
2. **Harmonic Comb Sum**:
   Accounts for rhythmic sub-harmonics ($2\tau$ and $4\tau$):
   $$C[\tau] = R[\tau] + 0.5 \cdot R[2\tau] + 0.25 \cdot R[4\tau]$$
3. **Perceptual Log-Gaussian Prior**:
   Weights lag values according to the human preference for tempos around $120\text{ BPM}$:
   $$W_{\text{prior}}[\tau] = \exp\left( -\frac{(\log_2(60 \cdot F_r / \tau) - \log_2(120))^2}{2 \cdot (0.8)^2} \right)$$
   $$S[\tau] = C[\tau] \cdot W_{\text{prior}}[\tau]$$

### 4.3 Sub-Frame Lag Interpolation
Given integer peak $\tau^* = \arg\max_\tau S[\tau]$:
$$\delta = \frac{S[\tau^* - 1] - S[\tau^* + 1]}{2(S[\tau^* - 1] - 2S[\tau^*] + S[\tau^* + 1])}, \quad \tau_{\text{sub}} = \tau^* + \delta$$
$$\text{BPM}_{\text{est}} = \frac{60 \cdot F_r}{\tau_{\text{sub}}}$$

---

## 5. Sliced Beat & Downbeat Tracking

### 5.1 Beat Peak Search
For each beat following previous beat $t_{\text{prev}}$:
* Target expected frame: $t_{\text{target}} = t_{\text{prev}} + \tau$.
* Search corridor: $[t_{\text{prev}} + 0.75\tau, \, t_{\text{prev}} + 1.25\tau]$.
* Objective function:
  $$\text{Score}(t) = \tilde{O}[t] - \lambda \cdot \left(\frac{t - t_{\text{target}}}{\tau}\right)^2$$
* Peak frame $t^* = \arg\max \text{Score}(t)$.
* If $\max \tilde{O}[t] < \text{threshold}$, set $t_{\text{next}} = t_{\text{target}}$ and tag with `ATT_BEAT_FLAG_INTERPOLATED`.

### 5.2 Downbeat Classification (4/4 Meter)
To establish musical bar boundaries:
* For each candidate phase offset $\phi \in \{0, 1, 2, 3\}$:
  $$\text{MetricScore}[\phi] = \sum_{\text{bar } k} \left( O_{\text{bass}}[t_{4k + \phi}] + O_{\text{snare}}[t_{4k + \phi + 1}] + O_{\text{bass}}[t_{4k + \phi + 2}] + O_{\text{snare}}[t_{4k + \phi + 3}] \right)$$
* The phase $\phi^*$ maximizing $\text{MetricScore}[\phi]$ designates Beat 1 of every bar.
* Beat markers are labeled with `bar_index` ($1, 2, 3\dots$), `beat_within_bar` ($1, 2, 3, 4$), and `ATT_BEAT_FLAG_DOWNBEAT` on beat 1.

---

## 6. Component & API Architecture

### 6.1 Configuration (`TempoConfig`)
```cpp
namespace audio_codecs::tempo {

struct TempoConfig {
    float    min_bpm                   = 60.0f;
    float    max_bpm                   = 200.0f;
    uint8_t  time_signature_num        = 4;     // Default 4/4 meter
    uint8_t  time_signature_denom      = 4;
    float    tempo_drift_threshold_bpm = 0.25f; // Threshold to insert tempo curve node
};

} // namespace audio_codecs::tempo
```

### 6.2 Generator (`TempoGenerator`)
```cpp
namespace audio_codecs::tempo {

class TempoGenerator {
public:
    TempoGenerator();
    ~TempoGenerator();

    bool init(preview::SeekableReader* asv_reader,
              preview::SeekableWriter* att_writer,
              const TempoConfig& config = {});

    // Cooperative sliced execution
    bool step(size_t max_frames = 128);

    float progress() const;
    bool is_complete() const;
    bool has_error() const;
    const AttHeader& header() const;

private:
    // Internal static buffers:
    // 512 novelty frames (2 KB), comb scores (350 B), beat tracking ring buffer (256 B)
};

} // namespace audio_codecs::tempo
```

### 6.3 Reader (`TempoReader`)
```cpp
namespace audio_codecs::tempo {

class TempoReader {
public:
    TempoReader();
    ~TempoReader();

    bool init(preview::SeekableReader* att_reader);
    const AttHeader& header() const;

    // Continuous tempo query (linear interpolation between curve nodes)
    float get_bpm_at(uint32_t time_ms) const;

    // Beat grid queries
    bool get_beat_at_index(uint32_t beat_index, AttBeatMarker* out_beat) const;
    bool get_nearest_beat(uint32_t time_ms, AttBeatMarker* out_beat) const;

    // Musical time conversions
    float time_to_beat(uint32_t time_ms) const;
    uint32_t beat_to_time(float beat_number) const;

    // Windowed retrieval
    size_t read_beats(uint32_t start_ms, uint32_t duration_ms,
                      AttBeatMarker* out_beats, size_t max_count) const;
};

} // namespace audio_codecs::tempo
```

---

## 7. Memory & Performance Specifications

| Metric | Target Specification |
| :--- | :--- |
| **Steady-State Heap Allocations** | **0 bytes** (zero dynamic allocation during `step()` or queries) |
| **Working Static RAM** | **$< 3\text{ KB}$** (fits in Cortex-M7 DTCM) |
| **CPU Utilization (Cortex-M7 @ 600 MHz)** | **$< 2\%$** of a single core when processing 128-frame slices |
| **Output File Footprint** | **$\sim 8\text{--}12\text{ KB}$** for a typical 4-minute audio track |
| **Tempo Estimation Accuracy** | **$\pm 0.2\text{ BPM}$** on steady rhythm tracks |

---

## 8. Verification Strategy

1. **Format & Serialization (`test_tempo_types.cpp`)**:
   * Compile-time size assertion: `sizeof(AttHeader) == 128`, `sizeof(AttTempoPoint) == 8`, `sizeof(AttBeatMarker) == 16`.
   * Roundtrip read/write test verifying byte-exact preservation.
2. **Tempo Induction & Detection (`test_tempo_generator.cpp`)**:
   * Constant tempo pulses ($120\text{ BPM}$ and $130\text{ BPM}$) with kick/snare separation.
   * Downbeat phase identification in 4/4 time.
   * Interpolation test across 4 bars of silence.
   * Accelerando ramp test ($100 \to 140\text{ BPM}$) verifying multiple tempo curve nodes.
3. **Random Access & Conversions (`test_tempo_reader.cpp`)**:
   * Linear interpolation queries across tempo curve segments.
   * `time_to_beat` and `beat_to_time` identity verification.
   * Windowed `read_beats` slice boundary retrieval.
4. **End-to-End Pipeline (`test_tempo_integration.cpp`)**:
   * Synthetic audio $\to$ `SpectrumGenerator` $\to$ `.asv` $\to$ `TempoGenerator` $\to$ `.att` $\to$ `TempoReader`.
