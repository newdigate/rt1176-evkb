# SynthUI PanelButton — tear-free 30 fps widget — design

Date: 2026-09-07
Status: approved (brainstorm 2026-09-07)
Tracking: Linear **NEW-27** ("SynthUI PanelButton: tear-free 30 fps widget").
Related: NEW-20 (RotaryKnob), NEW-23 (Fader), NEW-24 (Lamp) — the architecture, delta discipline, db pipeline, and gate guards this design inherits.

## 1. Goal & Scope

Implement the **PanelButton** primitive from the DC reference set (`SynthUI/reference/dc/PanelButton.dc.html`) as a SynthUI LVGL 9 custom widget (`src/synthui_panel_button`), verified by host tests in `SynthUI/tests/` and a new double-buffered evkb display example `examples/display/synthui_panel_button_test` running under QEMU in `/Users/moolet/Development/qemu-rt1170/build` targeting the i.MX RT1176 / MIMXRT1170-EVKB.

Success criteria (from the Linear issue), with how each is met:

| Criterion | Met by |
|---|---|
| Sustained ≥30 fps while animating | SW-delta rendering: flat vector shapes and clipped draw descriptors execute in sub-millisecond CPU draw time; Phase B animation loop measures frame interval on hardware and QEMU (§8) |
| Double-buffered | `lvgl_mipi_panel_create_db()` hardware double-buffer pipeline; flips synchronized to VSYNC ISR; goldens read presented buffer (`flip_sync()` + `scanned_fb()`) |
| No visible tearing or glitching | Back-buffer rendering + VSYNC flip by construction eliminates scanout tearing; per-boot delta-equality guard asserts mathematical pixel equivalence of delta vs full render |
| Delta rendering | Redraw only what changed: early-return guards on all setters suppress untouched frames; toggling buttons invalidate only their own bounding box |

## 2. Non-Goals

- No GPU/VGLite compositor TU in this issue (YAGNI: 16 flat vector buttons take <0.5 ms in software on Cortex-M7; missing ≥30 fps on silicon will re-open GPU as a separate brainstorm).
- No internal click-toggle state mutation by default (state is strictly programmatic via `synthui_panel_button_set_on()`; callers receive `LV_EVENT_CLICKED` while application code retains state authority).
- No text label rendering inside the button (the DC reference has none; labels are external).
- No arbitrary rotation (buttons are axis-aligned).

## 3. Widget Files & Public API (SynthUI)

New files in `SynthUI`, clean-room vector rebuild following the repository's provenance rules:

- `src/synthui_panel_button_types.h` — Clean header with enums and color definitions (LVGL-free for host tests)
- `src/synthui_panel_button.h` — Public LVGL 9 widget API
- `src/synthui_panel_button_math.h` — Header-only pure geometry math (LVGL-free for host tests)
- `src/synthui_panel_button.cpp` — Widget implementation (`synthui_panel_button_class`)
- `tests/panel_button_test.c` — Host unit tests (geometry bounds, scaling, glyph vertices, setter early-exits)

### Types & Constants (`synthui_panel_button_types.h` & `synthui_panel_button.h`)

```c
#ifndef SYNTHUI_PANEL_BUTTON_TYPES_H
#define SYNTHUI_PANEL_BUTTON_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNTHUI_PANEL_BUTTON_GLYPH_PLAY = 0,    /* default */
    SYNTHUI_PANEL_BUTTON_GLYPH_STOP,
    SYNTHUI_PANEL_BUTTON_GLYPH_RECORD,
    SYNTHUI_PANEL_BUTTON_GLYPH_REWIND,
    SYNTHUI_PANEL_BUTTON_GLYPH_FORWARD,
    SYNTHUI_PANEL_BUTTON_GLYPH_UP,
    SYNTHUI_PANEL_BUTTON_GLYPH_DOWN,
    SYNTHUI_PANEL_BUTTON_GLYPH_BAR,
    SYNTHUI_PANEL_BUTTON_GLYPH_DOT,
    SYNTHUI_PANEL_BUTTON_GLYPH_NONE,
} synthui_panel_button_glyph_t;

/* Standard DC reference accent colors (0xRRGGBB) */
#define SYNTHUI_PANEL_BUTTON_ACCENT_GREEN   0x48E070u  /* DC default */
#define SYNTHUI_PANEL_BUTTON_ACCENT_AMBER   0xF0A030u
#define SYNTHUI_PANEL_BUTTON_ACCENT_RED     0xF04848u
#define SYNTHUI_PANEL_BUTTON_ACCENT_BLUE    0x90A8F0u
#define SYNTHUI_PANEL_BUTTON_ACCENT_PALE    0xD8F0F0u
#define SYNTHUI_PANEL_BUTTON_ACCENT_NONE    0x303048u
#define SYNTHUI_PANEL_BUTTON_ACCENT_DEFAULT SYNTHUI_PANEL_BUTTON_ACCENT_GREEN

#ifdef __cplusplus
}
#endif
#endif /* SYNTHUI_PANEL_BUTTON_TYPES_H */
```

```c
#ifndef SYNTHUI_PANEL_BUTTON_H
#define SYNTHUI_PANEL_BUTTON_H

#include <lvgl.h>
#include <stdbool.h>
#include "synthui_panel_button_types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_obj_class_t synthui_panel_button_class;

lv_obj_t *synthui_panel_button_create(lv_obj_t *parent);

void synthui_panel_button_set_on(lv_obj_t *obj, bool on);
bool synthui_panel_button_get_on(const lv_obj_t *obj);

void synthui_panel_button_set_glyph(lv_obj_t *obj, synthui_panel_button_glyph_t glyph);
synthui_panel_button_glyph_t synthui_panel_button_get_glyph(const lv_obj_t *obj);

void synthui_panel_button_set_accent(lv_obj_t *obj, uint32_t rgb_hex);
uint32_t synthui_panel_button_get_accent(const lv_obj_t *obj);

void synthui_panel_button_set_glyph_scale(lv_obj_t *obj, float scale);
float synthui_panel_button_get_glyph_scale(const lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
#endif /* SYNTHUI_PANEL_BUTTON_H */
```

## 4. Geometry — Written Description of DC Reference

Geometry is evaluated in **viewBox unit space** (`vw = 100`, `vh = round(100 · H / W)`, scale `u = W / 100.0f` px/unit) and mapped to integer coordinates at draw time:

- Outer Bezel: width `vw`, height `vh`, corner radius `2.0`
- Inner Well: inset by `i = 2.2` on all sides: `innerW = vw - 4.4`, `innerH = vh - 4.4`
- Sheen Band: `y = i`, `height = vh · 0.11`
- Sheen Line: `y = i + vh · 0.11`, `height = vh · 0.05`
- Shadow Band: `y = vh - i - (vh · 0.1)`, `height = vh · 0.1`
- Glyph Transformation:
  - Bounding size: `g = min(vw, vh) · glyph_scale` (default `glyph_scale = 0.62`)
  - Scale: `s = g / 100.0`
  - Origin offset: `tx = (vw - g) / 2`, `ty = (vh - g) / 2`
  - Mapping formula for any template point `(px, py)`: `vx = tx + s · px`, `vy = ty + s · py`
  - Screen coordinate: `x_px = x0 + round(vx · u)`, `y_px = y0 + round(vy · u)`
- Clean-Room Glyph Template Coordinates (in 100×100 normalized box):
  - `PLAY`: 1 triangle `(32, 18), (84, 50), (32, 82)`
  - `STOP`: 1 rectangle `x = 28, y = 28, w = 44, h = 44`
  - `RECORD`: 1 circle `center = (50, 50), radius = 30`
  - `REWIND`: 2 triangles:
    - T1: `(50, 22), (20, 50), (50, 78)`
    - T2: `(80, 22), (50, 50), (80, 78)`
  - `FORWARD`: 2 triangles:
    - T1: `(50, 22), (80, 50), (50, 78)`
    - T2: `(20, 22), (50, 50), (20, 78)`
  - `UP`: 1 triangle `(50, 30), (74, 66), (26, 66)`
  - `DOWN`: 1 triangle `(50, 70), (74, 34), (26, 34)`
  - `BAR`: 1 rectangle `x = 18, y = 42, w = 64, h = 16`
  - `DOT`: 1 circle `center = (50, 50), radius = 16`
  - `NONE`: no geometry drawn

## 5. Palette & Opacities

| Element | Color / Gradient | Opacity (ON) | Opacity (OFF) | Opacity (DISABLED) |
|---|---|---|---|---|
| Outer Bezel | `#303048` | 100% (`LV_OPA_COVER`) | 100% (`LV_OPA_COVER`) | 100% (`LV_OPA_COVER`) |
| Body Face Top | `on ? #A0B4F4 : #90A8F0` | 100% (`LV_OPA_COVER`) | 100% (`LV_OPA_COVER`) | 50% (`LV_OPA_50`) |
| Body Face Bottom | `#486090` | 100% (`LV_OPA_COVER`) | 100% (`LV_OPA_COVER`) | 50% (`LV_OPA_50`) |
| Sheen Band | `#D8D8F0` | 75% (191) | 75% (191) | 40% (102) |
| Sheen Line | `#F0F0F0` | 90% (230) | 90% (230) | 50% (128) |
| Shadow Band | `#303048` | 50% (128) | 50% (128) | 30% (76) |
| Wash Overlay | `accent` | 12% (31) | 0% (0) | 0% (0) |
| Glyph Glow | `accent` | 32% (82) | 0% (0) | 0% (0) |
| Glyph Core | `on ? accent : #303048` | 100% (255) | 100% (255) | 40% (102) |

Fixed Colors:
- Bezel: `#303048`
- Sheen Band: `#D8D8F0`
- Sheen Line: `#F0F0F0`
- Shadow: `#303048`
- Body Bottom: `#486090`

## 6. Rendering Hierarchy (`DRAW_MAIN`)

A single draw pass in `LV_EVENT_DRAW_MAIN` emits clipped draw descriptors in back-to-front order:

1. **Bezel:** `lv_draw_rect` over `[coords.x1, coords.y1, coords.x2, coords.y2]`, `bg_color = #303048`, `radius = max(1, round(2.0 · u))`.
2. **Body Face:** `lv_draw_rect` over inner area inset by `round(2.2 · u)`. Vertical gradient (`LV_GRAD_DIR_VER`, 2 stops: top `on ? #A0B4F4 : #90A8F0`, bottom `#486090`).
3. **Sheen Band:** `lv_draw_rect` at top of inner area, height `round(vh · 0.11 · u)`, `bg_color = #D8D8F0`, `bg_opa = 191`.
4. **Sheen Line:** `lv_draw_rect` at `y = round((i + vh · 0.11) · u)`, height `max(1, round(vh · 0.05 · u))`, `bg_color = #F0F0F0`, `bg_opa = 230`.
5. **Shadow Band:** `lv_draw_rect` at `y = round(shadow_y · u)`, height `round(vh · 0.1 · u)`, `bg_color = #303048`, `bg_opa = 128`.
6. **Wash Overlay (if `on` and not disabled):** `lv_draw_rect` over inner area, `bg_color = accent`, `bg_opa = 31`.
7. **Glyph Glow (if `on` and not disabled):**
   - For triangles: `lv_draw_line` along all 3 triangle edges, width `max(1, round(14 · s · u))`, `color = accent`, `opa = 82`, `round_start = 1, round_end = 1`.
   - For rects (`stop`, `bar`): `lv_draw_rect` inflated by `round(7 · s · u)`, `radius = max(1, round(7 · s · u))`, `bg_color = accent`, `bg_opa = 82`.
   - For circles (`record`, `dot`): `lv_draw_rect` inflated by `round(7 · s · u)`, `radius = LV_RADIUS_CIRCLE`, `bg_color = accent`, `bg_opa = 82`.
8. **Glyph Core:**
   - Color: `on ? accent : #303048` (opacity halved if `LV_STATE_DISABLED`).
   - Triangles via `lv_draw_triangle`.
   - Rectangles via `lv_draw_rect`.
   - Circles via `lv_draw_rect` with `radius = LV_RADIUS_CIRCLE`.

## 7. State Management & Delta Damage Model

- Structure definition:
  ```c
  typedef struct {
      lv_obj_t obj;
      uint32_t accent;
      synthui_panel_button_glyph_t glyph;
      float glyph_scale;
      bool on;
  } synthui_panel_button_t;
  ```
- **Early-Return Rule:**
  ```c
  if (btn->on == on) return;
  btn->on = on;
  lv_obj_invalidate(obj);
  ```
  Applies identically to `set_glyph`, `set_accent`, and `set_glyph_scale`. Redundant updates cost 0 cycles in rendering and 0 damage area.
- **Damage Area:** `lv_obj_invalidate(obj)` marks the widget's bounding rectangle dirty. In a 16-button bank where 2 buttons toggle, only ~8.5k pixels are touched, leaving >99% of the screen buffer intact.

## 8. Consumer Example & Gate (`rt1176-evkb`)

- Directory: `examples/display/synthui_panel_button_test/`
- Pipeline: `lvgl_mipi_panel_create_db()` on RK055 panel (720×1280 XRGB8888).
- **Scene:** 16-button bank (4×4 grid) exercising all features:
  - Row 0 (Transport 74×58): Play (`green` on), Stop (`amber` off), Record (`red` on), Rewind (`blue` off)
  - Row 1 (Directional & Bar): Forward (`blue` off, 74×58), Up (`pale` on, 66×54), Down (`pale` off, 66×54), Bar (`green` off, 100×46)
  - Row 2 (Dots & Sizes): Dot (`amber` on, 60×60), None/Blank (`blue` off, 74×58), Large Play (`red` off, 96×75), Large Stop (`green` on, 96×75)
  - Row 3 (Edge cases & States): Disabled Play (on, 74×58), Disabled Record (off, 74×58), Custom Hex Pink Dot (on, 74×58), Custom Hex Cyan Bar (off, 74×58)
- **Phase A (QEMU-Gated Correctness):**
  1. Full initial render → emit `panel_button_crc=0x...` (pinned golden).
  2. 64-step deterministic delta sequence: fixed-seed LCG toggles buttons across the grid.
  3. **Delta-Equality Guard:** `panel_button_delta_crc` vs `panel_button_fresh_crc`. Asserts `panel_button_delta_eq=PASS`—verifies that delta-rendered state is bit-identical to a fresh full render.
  4. **Damage Engagement Check:** Measures `panel_button_damage max=... total=...` across the 64 steps, asserting `max <= 10000` px/step (confirming partial invalidation rather than full screen refresh).
  5. **VSYNC Guard:** Asserts `panel_button_vsync timeouts=0` (tear-free double buffering verified).
  6. Script checks `LVGL_FLUSHED=PASS`, `LVGL_BYTES=3686400`, `crc_done`, and prints `PASS: SynthUI panel_button render verified`.
- **Phase B (Animation & FPS Benchmark):**
  - Un-gated running pattern animation measuring `panel_button_fps` frame intervals, confirming sustained ≥30 fps.
- **Eyeball Support:**
  - `PANEL_BUTTON_EYEBALL_HOLD` compile macro for hold states and QEMU `pmemsave` capture.

## 9. Host Unit Tests (`SynthUI`)

- File: `SynthUI/tests/panel_button_test.c`, integrated into `SynthUI/tests/run.sh`.
- Checks:
  - Coordinate scaling at various widget dimensions (74×58, 66×54, 96×75, 108×46).
  - Glyph vertices and bounds for all 10 glyphs match geometry equations.
  - Viewbox aspect ratio and scale conversions.
  - Accent color definitions match DC hex values.
  - Degenerate dimensions reject gracefully (`w <= 0` or `h <= 0`).
  - Setter early-return logic.
