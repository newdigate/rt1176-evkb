# SynthUI PianoKey — tear-free 30 fps widget — design

Date: 2026-09-07  
Status: approved (brainstorm 2026-09-07)  
Tracking: Linear **NEW-28** ("SynthUI PianoKey: tear-free 30 fps widget").  
Related: NEW-20 (RotaryKnob), NEW-23 (Fader), NEW-24 (Lamp), NEW-26 (LevelMeter), NEW-27 (PanelButton), NEW-29 (SevenSegment) — the clean-room provenance, pure host unit test, delta rendering discipline, double-buffered pipeline, and gate guards this design inherits.

## 1. Goal & Scope

Implement the **PianoKey** primitive from the DC reference set (`SynthUI/reference/dc/PianoKey.dc.html`, `SynthUI/reference/dc/Keyboard Sheet.dc.html`) as a SynthUI LVGL 9 custom widget (`src/synthui_piano_key`), written in modern C++17 with LVGL-free C++ header math (`src/synthui_piano_key_math.h`, `src/synthui_piano_key_types.h`), verified by pure host tests in `SynthUI/tests/piano_key_test.cpp` (integrated into `tests/run.sh`) and a new double-buffered evkb display example `examples/display/synthui_piano_key_test` running under QEMU targeting the i.MX RT1176 / MIMXRT1170-EVKB.

Success criteria (from Linear NEW-28):

| Criterion | Met by |
|---|---|
| Sustained ≥30 fps while animating | SW-delta rendering: only flipped elements and tight LED bloom bounding boxes are repainted; Cortex-M7 draw time per frame is $<0.1$ ms; Phase B measures FPS on hardware and QEMU |
| Double-buffered | `lvgl_mipi_panel_create_db()` hardware double-buffer pipeline; flips synchronized to VSYNC ISR; goldens read presented buffer (`flip_sync()` + `scanned_fb()`) |
| No visible tearing or glitching | Back-buffer rendering + VSYNC flip by construction eliminates scanout tearing; per-boot delta-equality guard asserts mathematical pixel equivalence of delta vs full render |
| Delta rendering | Early-return guards on all setters (`set_type`, `set_lit`, `set_pressed`, `set_zone_top`, `set_pad_height`); delta damage invalidation: `set_lit` invalidates strictly the LED glow bounding box ($<700\text{ px}^2$), while `set_pressed` invalidates the key bounding box ($<7500\text{ px}^2$ for white, $<2500\text{ px}^2$ for black), well below the $15000\text{ px}$ guard |

## 2. Provenance & Clean-Room Reference Analysis

References:
- `SynthUI/reference/dc/PianoKey.dc.html`
- `SynthUI/reference/dc/Keyboard Sheet.dc.html`

In reference SVG/JS:
- ViewBox: `0 0 100 vh` where $vh = \text{round}(100 \cdot h / w)$.
- Scale unit: $u = w / 100.0$.
- Key variants:
  - `white`: default $w=46, h=158$ (or $62 \times 210$), default $\text{zone\_top} = 0.58$ (in keyboard sheet $0.50$). Corner radius $\text{bodyR} = 4$.
  - `black`: default $w=30, h=76$, default $\text{zone\_top} = 0.10$. Corner radius $\text{bodyR} = 3$.
- State properties:
  - `lit` (bool, default false): controls red LED indicator.
  - `pressed` (bool, default false): vertical deflection $dy = \text{pressed} \; ? \; vh \cdot 0.012 : 0$, body gradient shift for white key.
  - `zone_top` (float, default $0.58$ for white, $0.10$ for black).
  - `pad_height` (float, default $48\text{ px}$ in component, $46\text{ px}$ in keyboard sheet).
- Key body geometry:
  - Outer rect: $x=0, y=0, w=100, h=vh$, radius $\text{bodyR}$.
  - Horizontal 3-stop linear gradient:
    - Stop 0.0: `bodyA`
    - Stop 0.32: `bodyB`
    - Stop 1.0: `bodyC`
    - Palette:
      - Black key: `bodyA = #2b2b2b`, `bodyB = #141414`, `bodyC = #0b0b0b`.
      - White unpressed: `bodyA = #f6f5f2`, `bodyB = #eae8e3`, `bodyC = #c7c5c0`.
      - White pressed: `bodyA = #dcdad5`, `bodyB = #cfcdc8`, `bodyC = #b2b0ab`.
  - Border stroke: `edge`:
    - Black key: `#000000`, stroke width $\text{edgeW} = 1.6\text{ units}$.
    - White key: `#4a4a48`, stroke width $\text{edgeW} = 1.8\text{ units}$.
  - Right edge shade band:
    - Rect at $x=78, y=0, w=22, h=vh$.
    - Fill `#000000`, opacity $0.25$ ($\alpha = 64$) for black, $0.07$ ($\alpha = 18$) for white.
- LED indicator:
  - Circle at $cx=50, cy = ledY = vh \cdot (\text{zone\_top} + 0.055) + dy$.
  - Radius: $ledR = \min(14, vh \cdot 0.032 + 4)$.
  - Core fill: lit `#ff2a20` (`0xFF2A20`), unlit `#5c1c17` (`0x5C1C17`).
  - Outer bloom glow (when lit): stroke `#ff2a20`, width $1.5 \cdot ledR$, opacity $0.34$ ($\alpha = 87$).
    - Modeled in 2D rasterization as an outer filled circle of radius $R_{bloom} = 1.75 \cdot ledR$ at opacity $0.34$, overlaid by the opaque core circle of radius $ledR$.
- Ridged pad:
  - Pad top position: $padTop = vh \cdot (\text{zone\_top} + 0.14) + dy$.
  - Pad width: $padW = 62.0\text{ units}$ ($x = 19$, centering pad at $x=50$).
  - Pad height: $padH = \text{pad\_height} > 0 \; ? \; \text{pad\_height} \cdot (100 / w) : vh \cdot 0.94 - padTop$.
  - Shadow rect: at $(19 + 1.5, padTop + 2.0)$, size $(padW, padH)$, radius $2.5\text{ units}$, fill `#000000`, opacity $0.35$ ($\alpha = 89$).
  - Face rect: at $(19, padTop)$, size $(padW, padH)$, radius $2.5\text{ units}$, stroke `#3a3c3c` ($1.4\text{ units}$), horizontal 3-stop gradient (`#ececeb` at 0%, `#c4c4c1` at 40%, `#8f918f` at 100%).
  - 4 Ridges: horizontal lines at fractions $f \in [0.22, 0.42, 0.62, 0.82]$ of $padH$:
    - $y_{rel} = \text{round}(padH \cdot f \cdot 10) / 10$, $y = padTop + y_{rel}$.
    - $x_1 = 19 + padW \cdot 0.16 = 28.92$.
    - $x_2 = 19 + padW \cdot 0.84 = 71.08$.
    - Ridge stroke width: $\max(1.0, padH \cdot 0.035)$.
    - Color: `#7d807e`, stroke-linecap="round".

## 3. C++ Architecture & Header-Only Math

The math and types are implemented in pure C++17, header-only, with zero LVGL dependencies.

### 3.1 Types (`SynthUI/src/synthui_piano_key_types.h`)

```cpp
#ifndef SYNTHUI_PIANO_KEY_TYPES_H
#define SYNTHUI_PIANO_KEY_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNTHUI_PIANO_KEY_WHITE = 0,
    SYNTHUI_PIANO_KEY_BLACK = 1,
} synthui_piano_key_type_t;

/* Colors */
#define SYNTHUI_PIANO_KEY_COLOR_LED_LIT          0xFF2A20u
#define SYNTHUI_PIANO_KEY_COLOR_LED_UNLIT        0x5C1C17u
#define SYNTHUI_PIANO_KEY_COLOR_LED_BLOOM        0xFF2A20u

#define SYNTHUI_PIANO_KEY_COLOR_WHITE_A_UNPRESSED 0xF6F5F2u
#define SYNTHUI_PIANO_KEY_COLOR_WHITE_B_UNPRESSED 0xEAE8E3u
#define SYNTHUI_PIANO_KEY_COLOR_WHITE_C_UNPRESSED 0xC7C5C0u

#define SYNTHUI_PIANO_KEY_COLOR_WHITE_A_PRESSED   0xDCDAD5u
#define SYNTHUI_PIANO_KEY_COLOR_WHITE_B_PRESSED   0xCFCDC8u
#define SYNTHUI_PIANO_KEY_COLOR_WHITE_C_PRESSED   0xB2B0ABu

#define SYNTHUI_PIANO_KEY_COLOR_WHITE_EDGE       0x4A4A48u

#define SYNTHUI_PIANO_KEY_COLOR_BLACK_A          0x2B2B2Bu
#define SYNTHUI_PIANO_KEY_COLOR_BLACK_B          0x141414u
#define SYNTHUI_PIANO_KEY_COLOR_BLACK_C          0x0B0B0Bu
#define SYNTHUI_PIANO_KEY_COLOR_BLACK_EDGE       0x000000u

#define SYNTHUI_PIANO_KEY_COLOR_PAD_A            0xECECEBu
#define SYNTHUI_PIANO_KEY_COLOR_PAD_B            0xC4C4C1u
#define SYNTHUI_PIANO_KEY_COLOR_PAD_C            0x8F918Fu
#define SYNTHUI_PIANO_KEY_COLOR_PAD_EDGE         0x3A3C3Cu
#define SYNTHUI_PIANO_KEY_COLOR_PAD_RIDGE        0x7D807Eu

#ifdef __cplusplus
}
#endif
#endif /* SYNTHUI_PIANO_KEY_TYPES_H */
```

### 3.2 Geometry Math (`SynthUI/src/synthui_piano_key_math.h`)

```cpp
#ifndef SYNTHUI_PIANO_KEY_MATH_H
#define SYNTHUI_PIANO_KEY_MATH_H

#include "synthui_piano_key_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#ifdef __cplusplus
namespace synthui::piano_key {

struct Point { float x, y; };
struct Rect  { float x, y, w, h; };

struct KeyGeom {
    float w, h;
    float vw, vh, u;
    synthui_piano_key_type_t type;
    bool lit;
    bool pressed;
    float zone_top;
    float pad_height;

    float dy;
    float body_r;
    float edge_w;
    uint32_t body_a, body_b, body_c;
    uint32_t edge_c;
    float shade_a;

    float led_cx, led_cy, led_r, led_bloom_r;
    uint32_t led_fill;

    float pad_x, pad_y, pad_w, pad_h;
    float ridge_w;
    float ridge_y[4];
    float ridge_x1, ridge_x2;
};

bool compute_geom(float w, float h,
                  synthui_piano_key_type_t type,
                  bool lit, bool pressed,
                  float zone_top, float pad_height,
                  KeyGeom &g);

void compute_led_dirty_area(const KeyGeom &g, int32_t x0, int32_t y0,
                            int32_t &x1, int32_t &y1, int32_t &x2, int32_t &y2);

} // namespace synthui::piano_key
#endif
#endif /* SYNTHUI_PIANO_KEY_MATH_H */
```

### 3.3 Pure Host Unit Tests (`SynthUI/tests/piano_key_test.cpp`)

Directly compiled via `c++ -std=c++17 -Wall -Wextra -Werror` in `tests/run.sh`:
- Verifies aspect ratio and viewBox coordinate scaling ($vw=100, vh=\text{round}(100 \cdot h / w)$).
- Verifies vertical deflection $dy = vh \cdot 0.012$ when `pressed=true`, and $0$ when `false`.
- Verifies default `zone_top` fallback: $0.58$ for white, $0.10$ for black.
- Verifies LED center and bloom radius: $cx=50, ledR = \min(14, vh \cdot 0.032 + 4), R_{bloom} = 1.75 \cdot ledR$.
- Verifies pad width ($62$) and horizontal centering ($x=19$), 4 ridge fractions, and ridge bounds ($x_1=28.92, x_2=71.08$).
- Verifies color selection across white/black keys and unpressed/pressed/lit states.
- Verifies delta damage bounding box calculation for `lit` flip.

## 4. LVGL 9 Custom Widget Implementation

### 4.1 C API & C++ Wrapper (`src/synthui_piano_key.h`, `.cpp`)

- Widget structure:
  ```c
  typedef struct {
      lv_obj_t obj;
      synthui_piano_key_type_t type;
      bool lit;
      bool pressed;
      float zone_top;
      float pad_height;
  } synthui_piano_key_t;
  ```
- Public C API:
  - `synthui_piano_key_create(lv_obj_t *parent)`
  - `synthui_piano_key_set_type`, `get_type`
  - `synthui_piano_key_set_lit`, `get_lit`
  - `synthui_piano_key_set_pressed`, `get_pressed`
  - `synthui_piano_key_set_zone_top`, `get_zone_top`
  - `synthui_piano_key_set_pad_height`, `get_pad_height`
- Modern C++ wrapper `synthui::PianoKey`:
  - `create(parent)`
  - `setType(type)`
  - `setLit(lit)`, `isLit()`
  - `setPressed(pressed)`, `isPressed()`
  - `setZoneTop(zone_top)`
  - `setPadHeight(pad_height)`
  - `raw()` / `operator lv_obj_t*()`

### 4.2 Delta Rendering Discipline & Damage Invalidation

1. **Early returns:** Every setter compares new value to current value and returns immediately if unchanged.
2. **Targeted LED invalidation:**
   When `set_lit(obj, lit)` changes:
   - Compute LED center $(cx_{px}, cy_{px})$ and outer bloom radius $R_{bloom\_px} = \text{ceil}(1.75 \cdot ledR \cdot u) + 2$.
   - Construct `lv_area_t dirty = { cx - R, cy - R, cx + R, cy + R }`.
   - Call `lv_obj_invalidate_area(obj, &dirty)`.
   - Maximum invalidated area is $\approx 676\text{ px}$, consuming negligible bus bandwidth.
3. **Key press / geometry invalidation:**
   When `pressed`, `type`, `zone_top`, or `pad_height` changes:
   - Invalidate entire widget bounding box via `lv_obj_invalidate(obj)`.
   - Even the full white key area is only $46 \times 158 = 7,268\text{ px} \le 15000\text{ px}$.
4. **Drawing sub-element clipping:**
   In `key_draw(key, layer)`:
   - Check sub-element bounding boxes against `layer->_clip_area`:
     - If clip area does not intersect key body or pad, skip rasterizing them!
     - When only `lit` is dirtied, only the LED circles are rasterized.

### 4.3 Rasterization of Gradients and Primitives in LVGL 9

Because `LV_GRADIENT_MAX_STOPS == 2` in LVGL 9:
- **Key body 3-stop gradient:** Decomposed into two adjacent horizontal 2-stop gradient rects:
  - Left sub-rect: $x \in [0, 32]$, grad `bodyA` $\to$ `bodyB`.
  - Right sub-rect: $x \in [32, 100]$, grad `bodyB` $\to$ `bodyC`.
  - Followed by right edge shade band ($x \in [78, 100]$, black at $\alpha = 18$ or $64$).
  - Perimeter border stroke: drawn with `radius = body_r * u`, width `edge_w * u`, color `edge_c`.
- **LED Indicator:**
  - If `lit`: draw outer circle of radius $1.75 \cdot ledR \cdot u$, color `#ff2a20`, opacity $87$ (34%).
  - Core circle: radius $ledR \cdot u$, color lit (`#ff2a20`) or unlit (`#5c1c17`), opacity LV_OPA_COVER.
- **Ridged Pad:**
  - Cast shadow: rect at $(19 + 1.5, padTop + 2.0) \cdot u$, radius $2.5 \cdot u$, color `#000000`, opacity $89$ (35%).
  - Face: two adjacent horizontal 2-stop gradient rects split at $0.40 \cdot padW$:
    - Left: `#ececeb` $\to$ `#c4c4c1`.
    - Right: `#c4c4c1` $\to$ `#8f918f`.
  - Border stroke: width $1.4 \cdot u$, radius $2.5 \cdot u$, color `#3a3c3c`.
  - 4 Ridges: drawn using `lv_draw_line` with `round_start = 1, round_end = 1`, color `#7d807e`.

## 5. EVKB Display Example & QEMU Consumer Gate

### 5.1 Test Scene (`examples/display/synthui_piano_key_test`)

Based on `Keyboard Sheet.dc.html`:
- Display: 720×1280 RK055 panel in portrait orientation.
- Dark synth panel background (`#101020` / `#2a2a2c`).
- Top console strip:
  - Play button (`PanelButton`, glyph PLAY, accent GREEN).
  - Stop button (`PanelButton`, glyph STOP, accent AMBER).
  - Note readout (`SevenSegment`, accent RED, e.g. "C3").
  - Status toggle (`PanelButton` or toggle).
- Full 2-octave Keyboard:
  - 14 White keys ($w=46, h=158, \text{gap}=2$, total width $670\text{ px}$, centered at $x=25$).
  - 10 Black keys ($w=30, h=76$, overlapping across the C-D-E / F-G-A-B boundary pattern).
- State Demonstration Cards (below keyboard):
  - 6 keys showing the 6 canonical states from the spec:
    1. White idle (`lit=false, pressed=false`)
    2. White armed (`lit=true, pressed=false`)
    3. White held (`lit=true, pressed=true`)
    4. Black idle (`lit=false, pressed=false`)
    5. Black armed (`lit=true, pressed=false`)
    6. Black held (`lit=true, pressed=true`)

### 5.2 Test Phases & QEMU Gate Script (`run_qemu.sh`)

1. **Phase A1 — Initial Full Render Golden:**
   - Flush scene to display buffer, `lvgl_mipi_panel_flip_sync()`.
   - Calculate FNV-1a 32-bit CRC over full presented framebuffer ($720 \times 1280 \times 4$ bytes).
   - Assert pinned golden CRC: `grep -qE "piano_key_crc=0x[0-9A-F]{8}"`.
   - Demonstrate tripwire RED: verified that an altered CRC fails CI.
2. **Phase A2 — 64-Step Deterministic Delta Sequence:**
   - 64-step arpeggiator / walking note sequence:
     - Steps through active notes, toggling `lit` and `pressed` states across white and black keys.
     - Tracks `max_damage` and `total_damage`.
   - Evaluates:
     - `piano_key_delta_crc`: CRC after 64 delta steps.
     - Full fresh repaint of the exact same final state -> `piano_key_fresh_crc`.
     - Delta-equality guard: `delta_crc == fresh_crc` (`piano_key_delta_eq=PASS`).
     - Delta damage guard: `piano_key_damage max <= 15000` px.
     - VSYNC health guard: `piano_key_vsync flips > 0 timeouts=0`.
3. **Phase B — Animation Benchmark:**
   - Continuous keyboard arpeggiator loop reporting sustained FPS.

### 5.3 Build Configuration & Pin Bump

- `evkb.cmake`: bump SynthUI library SHA to the new commit.

## 6. Verification Plan

1. Host unit tests:
   `./tests/run.sh` in `SynthUI` passes all test suites cleanly (`knob_math_test`, `rotary_palette_test`, `fader_math_test`, `fader_color_test`, `lamp_test`, `panel_button_test`, `seven_segment_test`, `level_meter_test`, and `piano_key_test`).
2. EVKB target build:
   `synthui_piano_key_test` builds cleanly with arm-none-eabi GCC.
3. QEMU gate execution:
   `run_qemu.sh` executes in QEMU, asserts all tokens, verifies delta-equality, verifies damage bounds, and reports PASS.
4. Visual framebuffer capture:
   Eyeball hold mode captures the exact presented 720×1280 framebuffer via QEMU monitor (`pmemsave`). Crop close-up and full panel saved and presented for visual inspection.
