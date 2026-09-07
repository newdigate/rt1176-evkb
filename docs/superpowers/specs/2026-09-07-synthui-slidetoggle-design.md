# SynthUI SlideToggle — tear-free 30 fps widget — design

Date: 2026-09-07  
Status: approved (brainstorm 2026-09-07)  
Tracking: Linear **NEW-30** ("SynthUI SlideToggle: tear-free 30 fps widget").  
Related: NEW-20 (RotaryKnob), NEW-23 (Fader), NEW-24 (Lamp), NEW-26 (LevelMeter), NEW-27 (PanelButton), NEW-28 (PianoKey), NEW-29 (SevenSegment) — the clean-room provenance, pure host unit test, delta rendering discipline, double-buffered hardware pipeline, and gate guards this design inherits.

## 1. Goal & Scope

Implement the **SlideToggle** primitive from the DC reference set (`SynthUI/reference/dc/SlideToggle.dc.html`) as a SynthUI LVGL 9 custom widget (`src/synthui_slide_toggle`), written in modern C++17 with LVGL-free C++ header math (`src/synthui_slide_toggle_math.h`, `src/synthui_slide_toggle_types.h`), verified by pure host tests in `SynthUI/tests/slide_toggle_test.cpp` (integrated into `tests/run.sh`) and a new double-buffered evkb display example `examples/display/synthui_slide_toggle_test` running under QEMU targeting the i.MX RT1176 / MIMXRT1170-EVKB.

Success criteria (from Linear NEW-30):

| Criterion | Met by |
|---|---|
| Sustained ≥30 fps while animating | SW-delta rendering: only the slider well travel zone is repainted when switch position changes; unaffected elements (housing, panel background, waveform glyphs) are clipped away; Cortex-M7 draw time per frame is $<0.2$ ms; Phase B measures FPS on hardware and QEMU |
| Double-buffered | `lvgl_mipi_panel_create_db()` hardware double-buffer pipeline; flips synchronized to VSYNC ISR; goldens read presented buffer (`flip_sync()` + `scanned_fb()`) |
| No visible tearing or glitching | Back-buffer rendering + VSYNC flip by construction eliminates scanout tearing; per-boot delta-equality guard asserts mathematical pixel equivalence of delta vs full render |
| Delta rendering | Early-return guards on all setters (`set_value`, `set_positions`, `set_left_glyph`, `set_right_glyph`, `set_panel_color`, `set_disabled`); delta damage invalidation: `set_value` invalidates strictly the slider well travel bounding box ($<5000\text{ px}^2$), well below the $15000\text{ px}$ guard |

## 2. Provenance & Clean-Room Reference Analysis

Reference: `SynthUI/reference/dc/SlideToggle.dc.html`.

In reference SVG/JS:
- ViewBox: `0 0 100 vh` where $vh = \text{round}(100 \cdot h / w)$.
- Scale unit: $u = w / 100.0$.
- Widget properties:
  - `positions` (integer, min 2, max 4, default 2): number of discrete switch steps.
  - `value` (integer, clamped to $[0, \text{positions} - 1]$, default 0): current switch position.
  - `left` / `right` waveform glyphs (enum: `none`, `saw`, `square`, `tri`, `pulse`). Default: left = `saw`, right = `square`.
  - `disabled` (bool, default false): switches knob, glyph, and ridge colors to muted gray tones.
  - `panel` (color, default `#b9bcbc`): panel background color behind the widget.
  - `w`, `h`: widget bounding size (default $150 \times 52$).
- Waveform glyph geometry:
  - Local glyph box: $[0, 20] \times [0, 20]$.
  - Waveform path definitions:
    - `saw`: `M2 15L7 6L7 15L12 6L12 15L17 6` (5 line segments connecting 6 vertices).
    - `square`: `M2 15L2 7L7 7L7 15L12 15L12 7L17 7L17 15` (7 line segments connecting 8 vertices).
    - `tri`: `M2 15L6.5 6L11 15L15.5 6L18 11` (4 line segments connecting 5 vertices).
    - `pulse`: `M2 15L5 15L5 7L8 7L8 15L13 15L13 7L16 7L16 15` (8 line segments connecting 9 vertices).
    - `none`: no path.
  - Scale and layout:
    - $g = 20$, $gs = (vh \cdot 0.62) / 20 = vh \cdot 0.031$.
    - Left transform: translate $(1, (vh - g \cdot gs) / 2) = (1, vh \cdot 0.19)$, scale $gs$.
    - Right transform: translate $(100 - gx + 4, (vh - g \cdot gs) / 2) = (82, vh \cdot 0.19)$, scale $gs$, where $gx = 22$.
    - Stroke: width $2.2$ in glyph units ($\text{width}_{px} = \max(1, \text{lroundf}(2.2 \cdot gs \cdot u))$). Color `#232526` (`0x232526`), or `#8b8e8e` (`0x8B8E8E`) when disabled.
- Housing geometry:
  - $hx = \text{hasLeft} \; ? \; 22 : 3$.
  - $hRight = \text{hasRight} \; ? \; 100 - 22 = 78 : 97$.
  - $hw = hRight - hx$.
  - $hh = vh \cdot 0.72$, $hy = (vh - hh) / 2 = vh \cdot 0.14$.
  - Corner radius: $rx = 1.6$. Fill `#0f0f10` (`0x0F0F10`).
- Well / Slot geometry:
  - $inset = \max(1.6, vh \cdot 0.06)$.
  - $wellX = hx + inset$, $wellY = hy + inset$.
  - $wellW = hw - 2 \cdot inset$, $wellH = hh - 2 \cdot inset$.
  - Fill `#1d1d1f` (`0x1D1D1F`), sharp corners ($rx = 0$).
- Knob geometry:
  - $knobW = wellW / \text{positions}$, $knobH = wellH$.
  - Knob origin: $knobX = wellX + \text{value} \cdot knobW$, $knobY = wellY$.
  - Sub-elements:
    1. Shadow: offset $(+1.5, +2.0)$ from knob origin, size $(knobW, knobH)$, radius $1.2$, fill `#000000`, opacity $0.45$ ($\alpha = 115$).
    2. Body: size $(knobW, knobH)$, radius $1.2$, fill `#2c2e2f` (`0x2C2E2F`) active or `#3a3c3d` (`0x3A3C3D`) disabled, border stroke `#0a0a0b` (`0x0A0A0B`), width $1$ px.
    3. Top highlight: size $(knobW, knobH \cdot 0.16)$, radius $1.2$, fill `#ffffff`, opacity $0.12$ ($\alpha = 31$) active or $0.05$ ($\alpha = 13$) disabled.
    4. 4 Vertical ridges: at fractions $f \in \{0.26, 0.44, 0.62, 0.8\}$:
       - $x_{rel} = \text{round}(knobW \cdot f \cdot 10) / 10$.
       - $y1_{rel} = knobH \cdot 0.14$, $y2_{rel} = knobH \cdot 0.86$.
       - Stroke width: $ridgeW = \max(0.8, knobW \cdot 0.055)$.
       - Color: `#6a6e70` (`0x6A6E70`) active, `#54585a` (`0x54585A`) disabled. Round cap ends.

## 3. C++ Architecture & Header-Only Math

The math and types are implemented in pure C++17, header-only, with zero LVGL dependencies.

### 3.1 Types (`SynthUI/src/synthui_slide_toggle_types.h`)

```cpp
#ifndef SYNTHUI_SLIDE_TOGGLE_TYPES_H
#define SYNTHUI_SLIDE_TOGGLE_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNTHUI_SLIDE_TOGGLE_GLYPH_NONE   = 0,
    SYNTHUI_SLIDE_TOGGLE_GLYPH_SAW    = 1,
    SYNTHUI_SLIDE_TOGGLE_GLYPH_SQUARE = 2,
    SYNTHUI_SLIDE_TOGGLE_GLYPH_TRI    = 3,
    SYNTHUI_SLIDE_TOGGLE_GLYPH_PULSE  = 4,
} synthui_slide_toggle_glyph_t;

/* Colors */
#define SYNTHUI_SLIDE_TOGGLE_COLOR_PANEL_DEFAULT 0xB9BCBCu
#define SYNTHUI_SLIDE_TOGGLE_COLOR_HOUSING       0x0F0F10u
#define SYNTHUI_SLIDE_TOGGLE_COLOR_WELL          0x1D1D1Fu
#define SYNTHUI_SLIDE_TOGGLE_COLOR_KNOB_ACTIVE   0x2C2E2Fu
#define SYNTHUI_SLIDE_TOGGLE_COLOR_KNOB_DISABLED 0x3A3C3Du
#define SYNTHUI_SLIDE_TOGGLE_COLOR_KNOB_BORDER   0x0A0A0Bu
#define SYNTHUI_SLIDE_TOGGLE_COLOR_GLYPH_ACTIVE  0x232526u
#define SYNTHUI_SLIDE_TOGGLE_COLOR_GLYPH_DISABLED 0x8B8E8Eu
#define SYNTHUI_SLIDE_TOGGLE_COLOR_RIDGE_ACTIVE  0x6A6E70u
#define SYNTHUI_SLIDE_TOGGLE_COLOR_RIDGE_DISABLED 0x54585Au

#ifdef __cplusplus
}
#endif
#endif /* SYNTHUI_SLIDE_TOGGLE_TYPES_H */
```

### 3.2 Geometry Math (`SynthUI/src/synthui_slide_toggle_math.h`)

```cpp
#ifndef SYNTHUI_SLIDE_TOGGLE_MATH_H
#define SYNTHUI_SLIDE_TOGGLE_MATH_H

#include "synthui_slide_toggle_types.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

#ifdef __cplusplus
namespace synthui::slide_toggle {

struct Point { float x, y; };

struct GlyphPath {
    int num_points;
    Point points[10];
};

struct SlideToggleGeom {
    float w, h;
    float vw, vh, u;
    int32_t positions;
    int32_t value;
    bool disabled;
    synthui_slide_toggle_glyph_t left_glyph;
    synthui_slide_toggle_glyph_t right_glyph;
    uint32_t panel_color;

    bool has_left;
    bool has_right;
    float hx, hy, hw, hh;
    float well_x, well_y, well_w, well_h;
    float knob_w, knob_h;
    float knob_x, knob_y;
    float knob_top;
    float knob_hl_opa;
    uint32_t knob_fill;
    uint32_t glyph_color;
    uint32_t ridge_color;
    float ridge_w;
    float ridge_x[4];
    float ridge_y1, ridge_y2;

    float left_tx, left_ty, left_scale;
    float right_tx, right_ty, right_scale;
};

bool compute_geom(float w, float h,
                  int32_t positions, int32_t value,
                  synthui_slide_toggle_glyph_t left, synthui_slide_toggle_glyph_t right,
                  uint32_t panel_color, bool disabled,
                  SlideToggleGeom &g);

GlyphPath get_glyph_path(synthui_slide_toggle_glyph_t glyph);

void compute_knob_dirty_area(const SlideToggleGeom &g, int32_t x0, int32_t y0,
                             int32_t &x1, int32_t &y1, int32_t &x2, int32_t &y2);

} // namespace synthui::slide_toggle
#endif
#endif /* SYNTHUI_SLIDE_TOGGLE_MATH_H */
```

### 3.3 Pure Host Unit Tests (`SynthUI/tests/slide_toggle_test.cpp`)

Directly compiled via `c++ -std=c++17 -Wall -Wextra -Werror` in `tests/run.sh`:
- Verifies aspect ratio and viewBox coordinate scaling ($vw=100, vh=\text{round}(100 \cdot h / w)$).
- Verifies positions clamping ($[2, 4]$) and value clamping ($[0, \text{positions} - 1]$).
- Verifies housing bounds: $hx = 22$ (with glyph) vs $3$ (no glyph), $hRight = 78$ vs $97$, $hh = vh \cdot 0.72$, $hy = (vh - hh) / 2$.
- Verifies well bounds and inset calculation: $inset = \max(1.6, vh \cdot 0.06)$.
- Verifies knob width $knobW = wellW / positions$ and position offsets for values $0, 1, 2, 3$.
- Verifies glyph path coordinates for `saw`, `square`, `tri`, `pulse`, and `none`.
- Verifies knob sub-elements: shadow offset (+1.5, +2.0), highlight height ($knobH \cdot 0.16$), ridge positions at $[0.26, 0.44, 0.62, 0.8]$.
- Verifies color selection between active and disabled states.
- Verifies delta damage bounding box calculation: `compute_knob_dirty_area()` stays strictly within the slider well travel zone ($< 15000\text{ px}$).

## 4. LVGL 9 Custom Widget Implementation

### 4.1 C API & C++ Wrapper (`src/synthui_slide_toggle.h`, `.cpp`)

- Widget structure:
  ```c
  typedef struct {
      lv_obj_t obj;
      int32_t positions;
      int32_t value;
      synthui_slide_toggle_glyph_t left_glyph;
      synthui_slide_toggle_glyph_t right_glyph;
      uint32_t panel_color;
      bool disabled;
  } synthui_slide_toggle_t;
  ```
- Public C API:
  - `synthui_slide_toggle_create(lv_obj_t *parent)`
  - `synthui_slide_toggle_set_value(lv_obj_t *obj, int32_t value)`, `get_value`
  - `synthui_slide_toggle_set_positions(lv_obj_t *obj, int32_t positions)`, `get_positions`
  - `synthui_slide_toggle_set_left_glyph(lv_obj_t *obj, synthui_slide_toggle_glyph_t glyph)`, `get_left_glyph`
  - `synthui_slide_toggle_set_right_glyph(lv_obj_t *obj, synthui_slide_toggle_glyph_t glyph)`, `get_right_glyph`
  - `synthui_slide_toggle_set_panel_color(lv_obj_t *obj, uint32_t color)`, `get_panel_color`
  - `synthui_slide_toggle_set_disabled(lv_obj_t *obj, bool disabled)`, `get_disabled`
- Modern C++ wrapper `synthui::SlideToggle`:
  - `create(parent)`
  - `setValue(val)`, `getValue()`
  - `setPositions(pos)`, `getPositions()`
  - `setLeftGlyph(g)`, `getLeftGlyph()`
  - `setRightGlyph(g)`, `getRightGlyph()`
  - `setPanelColor(c)`, `getPanelColor()`
  - `setDisabled(d)`, `isDisabled()`
  - `raw()` / `operator lv_obj_t*()`
- Touch click interaction:
  - On `LV_EVENT_CLICKED`, if `!disabled`, advances `value = (value + 1) % positions`, calls `synthui_slide_toggle_set_value()`, and sends `LV_EVENT_VALUE_CHANGED`.

### 4.2 Delta Rendering Discipline & Damage Invalidation

1. **Early returns:** Every setter compares the incoming value against the current state and returns immediately if unchanged.
2. **Targeted slider well invalidation:**
   When `set_value(obj, value)` changes:
   - Compute the bounding box encompassing the slider well travel zone (including knob shadow).
   - Construct `lv_area_t dirty = { x1, y1, x2, y2 }`.
   - Call `lv_obj_invalidate_area(obj, &dirty)`.
   - Maximum invalidated area for standard widgets ($150 \times 52$) is $\approx 2500\text{ px}$, consuming minimal memory bus bandwidth.
3. **Full invalidation on layout changes:**
   When `positions`, `left_glyph`, `right_glyph`, `panel_color`, or `disabled` changes:
   - Invalidate entire widget bounding box via `lv_obj_invalidate(obj)`.
4. **Drawing sub-element clipping against `layer->_clip_area`:**
   In `slide_toggle_draw(toggle, layer)`:
   - Glyph bounding boxes are checked against `layer->_clip_area`:
     - If clip area does not intersect left/right glyph regions, skip glyph path rasterization entirely!
     - When only `value` is dirtied, only the well background and knob (shadow, body, highlight, ridges) are rasterized.

## 5. EVKB Display Example Scaffolding & Scene (`rt1176-evkb`)

- Directory: `examples/display/synthui_slide_toggle_test/`
  - `CMakeLists.txt`
  - `synthui_slide_toggle_test.cpp`
  - `run_qemu.sh`
- Panel & Hardware Pipeline:
  - Double-buffered hardware pipeline via `lvgl_mipi_panel_create_db(Display)` on the $720 \times 1280$ RK055 panel.
- Multi-state gallery scene displaying:
  - Row 1: 2-position toggles (saw/square, tri/pulse, none/saw).
  - Row 2: Multi-position toggles (3-position tri/pulse, 4-position saw/square).
  - Row 3: Panel color variations (`#b9bcbc`, `#6d7a85`, `#39434b`, `#d6d4cf`).
  - Row 4: Disabled states (2-position and 3-position disabled).
  - Active test bank: toggles driven deterministically during delta test sequence.
- Test Phases:
  - **Phase A1:** Initial full render, captures golden CRC (FNV-1a over $720 \times 1280 \times 4$ presented buffer).
  - **SLIDE_TOGGLE_EYEBALL_HOLD:** When defined, halts loop after initial render for QEMU monitor `pmemsave` frame inspection.
  - **Phase A2:** 64-step deterministic delta sequence cycling switch positions.
  - **Guard Assertions:**
    - `delta_crc == fresh_crc` (`slide_toggle_delta_eq=PASS`).
    - Max damage bound guard: `slide_toggle_damage max <= 15000`.
    - VSYNC health: `flips > 0`, `isrs > 0`, `timeouts = 0`.
    - Completion token: `crc_done`.
  - **Phase B:** Continuous animation benchmark reporting `slide_toggle_fps`.

## 6. QEMU Gate Runner & CI Verification

- Script: `examples/display/synthui_slide_toggle_test/run_qemu.sh`
- Sourcing `tools/gate-lib.sh`.
- Steps:
  1. Boot under QEMU, run test, verify UART tokens (`PANEL_OK`, `slide_toggle_crc`, `slide_toggle_delta_eq=PASS`, damage bound, VSYNC health, `crc_done`).
  2. Record golden CRC across consecutive runs and pin in `run_qemu.sh`.
  3. Tripwire RED test: Temporarily change golden to `0xDEADBEEF`, verify failure exit code 1.
  4. Bump SynthUI commit pin in `evkb.cmake` line 131 to the newly committed SynthUI SHA.
