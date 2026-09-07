# SynthUI LevelMeter — tear-free 30 fps widget — design

Date: 2026-09-07  
Status: approved (brainstorm 2026-09-07)  
Tracking: Linear **NEW-26** ("SynthUI LevelMeter: tear-free 30 fps widget").  
Related: NEW-20 (RotaryKnob), NEW-23 (Fader), NEW-24 (Lamp), NEW-27 (PanelButton), NEW-29 (SevenSegment) — the clean-room provenance, pure host unit test, delta rendering discipline, double-buffered pipeline, and gate guards this design inherits.

## 1. Goal & Scope

Implement the **LevelMeter** primitive from the DC reference set (`SynthUI/reference/dc/LevelMeter.dc.html`) as a SynthUI LVGL 9 custom widget (`src/synthui_level_meter`), written in modern C++ with LVGL-free C++ header math (`src/synthui_level_meter_math.h`, `src/synthui_level_meter_types.h`), verified by host tests in `SynthUI/tests/level_meter_test.cpp` and a new double-buffered evkb display example `examples/display/synthui_level_meter_test` running under QEMU targeting the i.MX RT1176 / MIMXRT1170-EVKB.

Success criteria (from the Linear issue):

| Criterion | Met by |
|---|---|
| Sustained ≥30 fps while animating | SW-delta rendering: only flipped segments and their tight glow bounding boxes are repainted; Cortex-M7 draw time per frame is $<0.1$ ms; Phase B measures FPS on hardware and QEMU |
| Double-buffered | `lvgl_mipi_panel_create_db()` hardware double-buffer pipeline; flips synchronized to VSYNC ISR; goldens read presented buffer (`flip_sync()` + `scanned_fb()`) |
| No visible tearing or glitching | Back-buffer rendering + VSYNC flip by construction eliminates scanout tearing; per-boot delta-equality guard asserts mathematical pixel equivalence of delta vs full render |
| Delta rendering | Early-return guards on all setters (`set_value`, `set_peak`, `set_clip`, `set_segments`, `set_panel_color`); delta damage invalidation: compute bounding box of only flipped segments and call `lv_obj_invalidate_area()` |

## 2. Provenance & Reference Analysis

Reference: `SynthUI/reference/dc/LevelMeter.dc.html`.

In reference SVG/JS:
- ViewBox: `0 0 100 vh` where $vh = \text{round}(100 \cdot h / w)$.
- Number of segments: $n = \max(3, \text{segments})$ (default 10, range $[3, 30]$).
- Normalized value: $v \in [0, 1]$ (default 0.6).
- Peak hold indicator: $p \in [-1, 1]$ (default 0.8; $p < 0$ or $p = 0 \implies$ off).
- Clip indicator: boolean (default false).
- Lit count: $\text{lit} = \text{round}(v \cdot n)$.
- Peak index: $\text{peakIdx} = p \ge 0 \; ? \; \min(n - 1, \text{round}(p \cdot n) - 1) : -1$.
- Geometry padding:
  - $\text{pad} = vh \cdot 0.02$
  - $\text{pitch} = (vh - 2 \cdot \text{pad}) / n$
  - $\text{segH} = \text{pitch} \cdot 0.7$
- For segment index $i \in [0, n - 1]$ (where $0$ is bottom, $n - 1$ is top):
  - Normalized position factor: $f = i / (n - 1)$
  - Color thresholding:
    - $f \ge 0.9 \implies$ Red `#ff3b30`
    - $f \ge 0.72 \implies$ Amber `#ffa41f`
    - $f < 0.72 \implies$ Green `#3be03b`
  - Vertical coordinate:
    - $y = \text{pad} + (n - 1 - i) \cdot \text{pitch} + (\text{pitch} - \text{segH}) / 2$
  - State:
    - $\text{on} = (i < \text{lit}) \lor (i == \text{peakIdx}) \lor (\text{clip} \land i \ge n - 1)$
    - $\text{isPeak} = (i == \text{peakIdx} \land i \ge \text{lit})$
  - Slot rect:
    - $x = 10, w = 80, y, h = \text{segH}, rx = \text{segH} \cdot 0.34$.
    - Background slot well color: `#12161a`.
  - Colored body & glow:
    - Color: segment color ($f$).
    - Body opacity: $\text{on} \; ? \; 1.0 \; : \; 0.17$ (unlit ghost).
    - Glow stroke width: $\text{on} \; ? \; \text{segH} \cdot 0.7 \; : \; 0$.
    - Glow opacity: $\text{on} \; ? \; (\text{isPeak} \; ? \; 0.22 \; : \; 0.34) \; : \; 0$.
  - Highlight bar:
    - $hx = 18, hw = 64$.
    - $hy = y + \text{segH} \cdot 0.18$.
    - $hh = \max(0.8, \text{segH} \cdot 0.16)$.
    - $hr = \text{segH} \cdot 0.08$.
    - Color: `#ffffff`, opacity $\text{on} \; ? \; 0.5 \; : \; 0.06$.
  - Panel background: `#6d7a85` (default).

## 3. C++ Architecture & Header-Only Math

The math and types are designed in pure C++17, header-only, with zero dependencies on LVGL, allowing direct compilation in both embedded targets and host unit tests.

### 3.1 Types (`src/synthui_level_meter_types.h`)

```cpp
#ifndef SYNTHUI_LEVEL_METER_TYPES_H
#define SYNTHUI_LEVEL_METER_TYPES_H

#include <cstdint>
#include <cstdbool>

namespace synthui::level_meter {

constexpr uint8_t MIN_SEGMENTS = 3;
constexpr uint8_t MAX_SEGMENTS = 30;
constexpr uint8_t DEFAULT_SEGMENTS = 10;

constexpr uint32_t COLOR_RED       = 0xFF3B30;
constexpr uint32_t COLOR_AMBER     = 0xFFA41F;
constexpr uint32_t COLOR_GREEN     = 0x3BE03B;
constexpr uint32_t COLOR_SLOT_BG   = 0x12161A;
constexpr uint32_t COLOR_HIGHLIGHT = 0xFFFFFF;
constexpr uint32_t COLOR_PANEL_DEF = 0x6D7A85;

constexpr float DEFAULT_VALUE = 0.6f;
constexpr float DEFAULT_PEAK  = 0.8f;

struct Point {
    float x;
    float y;
};

struct Rect {
    float x;
    float y;
    float w;
    float h;
    float rx;
};

struct SegmentGeom {
    Rect slot;
    Rect highlight;
    uint32_t color;
    bool on;
    bool is_peak;
    float glow_w;
    float glow_opa;
    float core_opa;
    float hl_opa;
};

struct MeterLayout {
    float w;
    float h;
    float vh;
    float u;
    float pad;
    float pitch;
    float seg_h;
    uint8_t n;
    int32_t lit;
    int32_t peak_idx;
};

} // namespace synthui::level_meter

#endif // SYNTHUI_LEVEL_METER_TYPES_H
```

### 3.2 Geometry Math (`src/synthui_level_meter_math.h`)

```cpp
#ifndef SYNTHUI_LEVEL_METER_MATH_H
#define SYNTHUI_LEVEL_METER_MATH_H

#include "synthui_level_meter_types.h"
#include <cmath>
#include <algorithm>

namespace synthui::level_meter {

inline float clamp(float val, float min_val, float max_val) {
    if (std::isnan(val)) return min_val;
    return std::max(min_val, std::min(max_val, val));
}

inline bool compute_layout(float w, float h, uint8_t segments, float value, float peak, MeterLayout &l) {
    if (w < 1.0f || h < 1.0f) return false;
    l.w = w;
    l.h = h;
    l.vh = std::round(100.0f * h / w);
    l.u = w / 100.0f;
    l.n = std::max<uint8_t>(MIN_SEGMENTS, std::min<uint8_t>(MAX_SEGMENTS, segments));
    const float val_clamped = clamp(value, 0.0f, 1.0f);
    l.lit = static_cast<int32_t>(std::round(val_clamped * l.n));
    if (peak >= 0.0f) {
        int32_t p_idx = static_cast<int32_t>(std::round(peak * l.n)) - 1;
        l.peak_idx = std::min<int32_t>(l.n - 1, p_idx);
    } else {
        l.peak_idx = -1;
    }
    l.pad = l.vh * 0.02f;
    l.pitch = (l.vh - l.pad * 2.0f) / l.n;
    l.seg_h = l.pitch * 0.7f;
    return true;
}

inline uint32_t get_segment_color(uint8_t i, uint8_t n) {
    if (n <= 1) return COLOR_GREEN;
    float f = static_cast<float>(i) / static_cast<float>(n - 1);
    if (f >= 0.9f) return COLOR_RED;
    if (f >= 0.72f) return COLOR_AMBER;
    return COLOR_GREEN;
}

inline void get_segment_geom(const MeterLayout &l, uint8_t i, bool clip, SegmentGeom &s) {
    s.color = get_segment_color(i, l.n);
    float y = l.pad + (l.n - 1 - i) * l.pitch + (l.pitch - l.seg_h) * 0.5f;
    s.on = (static_cast<int32_t>(i) < l.lit) ||
           (static_cast<int32_t>(i) == l.peak_idx) ||
           (clip && i >= l.n - 1);
    s.is_peak = (static_cast<int32_t>(i) == l.peak_idx && static_cast<int32_t>(i) >= l.lit);

    s.slot.x = 10.0f;
    s.slot.w = 80.0f;
    s.slot.y = std::round(y * 10.0f) / 10.0f;
    s.slot.h = std::round(l.seg_h * 10.0f) / 10.0f;
    s.slot.rx = l.seg_h * 0.34f;

    s.glow_w = s.on ? l.seg_h * 0.7f : 0.0f;
    s.glow_opa = s.on ? (s.is_peak ? 0.22f : 0.34f) : 0.0f;
    s.core_opa = s.on ? 1.0f : 0.17f;

    s.highlight.x = 18.0f;
    s.highlight.w = 64.0f;
    s.highlight.y = std::round((y + l.seg_h * 0.18f) * 10.0f) / 10.0f;
    s.highlight.h = std::max(0.8f, l.seg_h * 0.16f);
    s.highlight.rx = l.seg_h * 0.08f;
    s.hl_opa = s.on ? 0.5f : 0.06f;
}

} // namespace synthui::level_meter

#endif // SYNTHUI_LEVEL_METER_MATH_H
```

## 4. LVGL 9 Custom Widget Implementation

### 4.1 Header (`src/synthui_level_meter.h`)

Provides C linkage for LVGL standard function calls and a C++ wrapper class `synthui::LevelMeter`:

```cpp
#ifndef SYNTHUI_LEVEL_METER_H
#define SYNTHUI_LEVEL_METER_H

#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_obj_class_t synthui_level_meter_class;

lv_obj_t *synthui_level_meter_create(lv_obj_t *parent);

void synthui_level_meter_set_value(lv_obj_t *obj, float val);
float synthui_level_meter_get_value(const lv_obj_t *obj);

void synthui_level_meter_set_peak(lv_obj_t *obj, float peak);
float synthui_level_meter_get_peak(const lv_obj_t *obj);

void synthui_level_meter_set_clip(lv_obj_t *obj, bool clip);
bool synthui_level_meter_get_clip(const lv_obj_t *obj);

void synthui_level_meter_set_segments(lv_obj_t *obj, uint8_t segments);
uint8_t synthui_level_meter_get_segments(const lv_obj_t *obj);

void synthui_level_meter_set_panel_color(lv_obj_t *obj, uint32_t rgb_hex);
uint32_t synthui_level_meter_get_panel_color(const lv_obj_t *obj);

#ifdef __cplusplus
} // extern "C"

namespace synthui {
class LevelMeter {
public:
    explicit LevelMeter(lv_obj_t *obj = nullptr) : obj_(obj) {}
    static LevelMeter create(lv_obj_t *parent) {
        return LevelMeter(synthui_level_meter_create(parent));
    }
    lv_obj_t *raw() const { return obj_; }
    operator lv_obj_t *() const { return obj_; }

    void setValue(float v) { synthui_level_meter_set_value(obj_, v); }
    float getValue() const { return synthui_level_meter_get_value(obj_); }

    void setPeak(float p) { synthui_level_meter_set_peak(obj_, p); }
    float getPeak() const { return synthui_level_meter_get_peak(obj_); }

    void setClip(bool c) { synthui_level_meter_set_clip(obj_, c); }
    bool getClip() const { return synthui_level_meter_get_clip(obj_); }

    void setSegments(uint8_t s) { synthui_level_meter_set_segments(obj_, s); }
    uint8_t getSegments() const { return synthui_level_meter_get_segments(obj_); }

    void setPanelColor(uint32_t rgb) { synthui_level_meter_set_panel_color(obj_, rgb); }
    uint32_t getPanelColor() const { return synthui_level_meter_get_panel_color(obj_); }

private:
    lv_obj_t *obj_;
};
} // namespace synthui
#endif

#endif // SYNTHUI_LEVEL_METER_H
```

### 4.2 Widget Implementation (`src/synthui_level_meter.cpp`)

- **Widget Struct:**
  ```cpp
  struct synthui_level_meter_t {
      lv_obj_t obj;
      float value;
      float peak;
      uint8_t segments;
      bool clip;
      uint32_t panel_color;
  };
  ```

- **Delta Damage Invalidation:**
  When `set_value`, `set_peak`, or `set_clip` is called:
  1. Compare existing parameters with new values; if identical or NaN, early-exit.
  2. Compute segment states ($[0 \dots n - 1]$) with old parameters and new parameters.
  3. Identify which segments have flipped state (`on_old != on_new` or `is_peak_old != is_peak_new`).
  4. For the contiguous change runs, calculate pixel dirty bounding boxes:
     - $Y$ range: $[\min(y) - \text{glow\_margin}, \max(y + \text{segH}) + \text{glow\_margin}]$.
     - $X$ range: from widget left to widget right.
     - Call `lv_obj_invalidate_area(obj, &dirty_area)`.
  5. Store new values.

- **Draw Method (`LV_EVENT_DRAW_MAIN`):**
  1. Panel background: Draw solid rectangle over widget coords with `panel_color` (`#6D7A85` default), clipped by LVGL to `layer->_clip_area`.
  2. Segments: Iterate $i \in [0, n - 1]$.
     - Clip check: if segment bounding box (including glow) does not intersect `layer->_clip_area`, skip.
     - Slot: `#12161A` opaque rounded rect.
     - Glow: if `on`, draw outer rounded rect inflated by `0.35 * seg_h * u` with opacity 56 (`isPeak`) or 87 (normal).
     - Core: segment color with opacity 255 (`on`) or 43 (unlit ghost 17%).
     - Highlight: `#FFFFFF` with opacity 128 (`on`) or 15 (unlit ghost 6%).

## 5. Pure Host Unit Testing

File: `SynthUI/tests/level_meter_test.cpp`, compiled with `c++ -std=c++17 -Wall -Wextra -Werror` and executed by `SynthUI/tests/run.sh`.

Checks:
1. `compute_layout`: Default size ($W=34, H=200 \implies vh=588, u=0.34$).
2. Configurable segments: $N=3, 10, 16, 24, 30$ and clamping of out-of-bounds inputs ($N=2 \implies 3$, $N=40 \implies 30$).
3. Color thresholds:
   - $N=10$: $i=9 \implies f=1.0$ (Red), $i=8 \implies f=0.888$ (Amber), $i=7 \implies f=0.777$ (Amber), $i=6 \implies f=0.666$ (Green).
4. Value to lit segments: $v=0.0 \implies 0$, $v=0.6 \implies 6$, $v=1.0 \implies 10$.
5. Peak indicator calculation: $p=-1 \implies -1$, $p=0.0 \implies -1$, $p=0.8 \implies 7$, $p=1.0 \implies 9$.
6. Clip indicator: activates segment $n - 1$.
7. Flipped segment detector: verifies that incremental level changes produce tight segment dirty intervals.

## 6. Consumer Gate & Hardware Double Buffering (`rt1176-evkb`)

Location: `examples/display/synthui_level_meter_test/`

### 6.1 Multi-Meter Synth Console Scene

Constructed on a 720×1280 RK055 panel with dark synth panel background (`#101020`):
1. **Master Stereo Pair (L/R):**
   - Left: $X=80, Y=80, W=48, H=380$, 24 segments, $v=0.78, p=0.88$.
   - Right: $X=144, Y=80, W=48, H=380$, 24 segments, $v=0.72, p=0.85$.
2. **Channel Strips (Tracks 1–4):**
   - 16 segments each, $W=36, H=240$.
   - Trk 1: $X=240, Y=80, v=0.65, p=0.75$.
   - Trk 2: $X=290, Y=80, v=0.85, p=0.92$.
   - Trk 3: $X=340, Y=80, v=0.45, p=0.60$.
   - Trk 4: $X=390, Y=80, v=0.95, p=0.98$ (clip=false).
3. **Aux / FX Return Strips (Aux 1–2):**
   - 10 segments each, $W=42, H=180$.
   - Aux 1: $X=470, Y=80, v=0.50, p=0.70$.
   - Aux 2 (Overdriven): $X=530, Y=80, v=1.00, p=1.00$, `clip = true`.

### 6.2 Test Phases & Gate Script (`run_qemu.sh`)

- **Phase A1: Initial Golden CRC:**
  - Full render + VSYNC flip.
  - FNV-1a 32-bit hash of 3,686,400 bytes presented buffer.
  - Demonstrated tripwire RED: altering the pinned golden CRC causes immediate failure.
- **Phase A2: 64-step Deterministic Delta Sequence:**
  - Deterministic animation across meters.
  - Measures `max_damage`: enforces `max_damage <= 15000` px.
  - Computes `delta_crc`.
  - Full screen invalidation `lv_obj_invalidate(scr)` produces `fresh_crc`.
  - Asserts bit-exact `delta_crc == fresh_crc`.
  - VSYNC health check: `flips > 0`, `isrs > 0`, `timeouts == 0`.
- **Phase B: Performance Loop:**
  - Measures animation FPS.

### 6.3 Visual Verification

- Compile with `-DLEVEL_METER_EYEBALL_HOLD=1`.
- Run under QEMU, dump memory via monitor `pmemsave 0x80000000 0x384000 frame.raw`.
- Convert raw XRGB to PNG.
- Save full screenshot and zoomed crop in `examples/display/synthui_level_meter_test/` and artifact directory.

### 6.4 Pin Bump

- Bump SynthUI pin in `evkb.cmake` line 131 to the newly created commit.
