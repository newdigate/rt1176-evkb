# SynthUI Lamp — tear-free 30 fps widget — design

Date: 2026-09-07
Status: approved (brainstorm 2026-09-07)
Tracking: Linear **NEW-24** ("SynthUI Lamp: tear-free 30 fps widget").
Related: NEW-20 (RotaryKnob), NEW-23 (Fader) — the architecture, delta discipline, db pipeline, and gate guards this design inherits.

## 1. Goal & Scope

Implement the **Lamp** primitive from the DC reference set (`SynthUI/reference/dc/Lamp.dc.html`) as a SynthUI LVGL 9 custom widget (`src/synthui_lamp`), verified by host tests in `SynthUI/tests/` and a new double-buffered evkb display example `examples/display/synthui_lamp_test` running under QEMU and targeting the i.MX RT1176 / MIMXRT1170-EVKB.

Success criteria (from the Linear issue), with how each is met:

| Criterion | Met by |
|---|---|
| Sustained ≥30 fps while animating | SW-delta rendering: microsecond-level draw times for 16-lamp banks on CPU; Phase B animation loop measures frame interval on hardware and QEMU (§8) |
| Double-buffered | `lvgl_mipi_panel_create_db()` hardware double-buffer pipeline; flips synchronized to VSYNC ISR; goldens read presented buffer (`flip_sync()` + `scanned_fb()`) |
| No visible tearing or glitching | Back-buffer rendering + VSYNC flip by construction eliminates scanout tearing; per-boot delta-equality guard asserts mathematical pixel equivalence of delta vs full render |
| Delta rendering | Redraw only what changed: early-return guards on all setters suppress untouched frames; toggling lamps invalidate only their own bounding box |

## 2. Non-Goals

- No GPU/VGLite compositor TU in this issue (YAGNI: 16 flat circles/rects take <0.5 ms in software on Cortex-M7; missing ≥30 fps on silicon will re-open GPU as a separate brainstorm).
- No automatic click-toggle state mutation by default (the widget is a passive indicator; callers may enable `LV_OBJ_FLAG_CLICKABLE` to receive `LV_EVENT_CLICKED` while application code retains state authority).
- No custom text or numerical readouts (the DC reference has none).
- No arbitrary rotation (lamps are axis-aligned).

## 3. Widget Files & Public API (SynthUI)

New files in `SynthUI`, clean-room vector rebuild following the repository's provenance rules:

- `src/synthui_lamp.h` — public API, shape enum, standard DC color constants
- `src/synthui_lamp.cpp` — widget implementation (`synthui_lamp_class`)
- `tests/lamp_test.c` — host unit tests (geometry bounds, unit scaling, color definitions, setter early-exits)

### Types & Constants (`synthui_lamp.h`)

```c
#ifndef SYNTHUI_LAMP_H
#define SYNTHUI_LAMP_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNTHUI_LAMP_SHAPE_ROUND = 0,   /* circular lamp (DC default) */
    SYNTHUI_LAMP_SHAPE_BAR,         /* rectangular bar with rounded corners (r=2) */
    SYNTHUI_LAMP_SHAPE_PILL,        /* pill with fully rounded ends (r=vh*0.24) */
} synthui_lamp_shape_t;

/* Standard DC reference lamp colors (0xRRGGBB) */
#define SYNTHUI_LAMP_COLOR_PINK     0xF078B0  /* DC default */
#define SYNTHUI_LAMP_COLOR_RED      0xE04848
#define SYNTHUI_LAMP_COLOR_GREEN    0x48E070
#define SYNTHUI_LAMP_COLOR_AMBER    0xF0A030
#define SYNTHUI_LAMP_COLOR_BLUE     0x90A8F0
#define SYNTHUI_LAMP_COLOR_CYAN     0xD8F0F0
#define SYNTHUI_LAMP_COLOR_DEFAULT  SYNTHUI_LAMP_COLOR_PINK

extern const lv_obj_class_t synthui_lamp_class;

lv_obj_t *synthui_lamp_create(lv_obj_t *parent);

void synthui_lamp_set_on(lv_obj_t *obj, bool on);
bool synthui_lamp_get_on(const lv_obj_t *obj);

void synthui_lamp_set_shape(lv_obj_t *obj, synthui_lamp_shape_t shape);
synthui_lamp_shape_t synthui_lamp_get_shape(const lv_obj_t *obj);

void synthui_lamp_set_color(lv_obj_t *obj, uint32_t rgb_hex);
uint32_t synthui_lamp_get_color(const lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
#endif
```

## 4. Geometry — Written Description of DC Reference

Geometry is evaluated in **viewBox unit space** (`vw = 100`, `vh = round(100 · H / W)`, scale `u = W / 100.0f` px/unit) and mapped to integer coordinates at draw time:

- Margin: `m = 12`
- Outer bezel: width `vw`, height `vh`, corner radius `1.5`
- Inner well: inset by `1.6` on all sides: `x = 1.6`, `y = 1.6`, `innerW = vw - 3.2`, `innerH = vh - 3.2`
- Top highlight line: width `innerW`, height `topLine = max(1.2, vh · 0.06)`
- Round shape (`SYNTHUI_LAMP_SHAPE_ROUND`):
  - Center: `cx = vw / 2`, `cy = vh / 2`
  - Core radius: `r = min(vw, vh) / 2 - m`
  - Glow radius (when on): `r_glow = r + glowW / 2` where `glowW = min(vw, vh) · 0.22`
- Bar shape (`SYNTHUI_LAMP_SHAPE_BAR`):
  - Position: `bx = m`, `by = vh · 0.26`
  - Dimensions: `bw = vw - 2 · m`, `bh = vh · 0.48`
  - Core corner radius: `br = 2`
  - Glow expansion (when on): inflated by `glowW / 2` on all sides; corner radius `br + glowW / 2`
- Pill shape (`SYNTHUI_LAMP_SHAPE_PILL`):
  - Same bounding box as Bar, but corner radius `br = vh · 0.24` (fully rounded capsule caps)

## 5. Palette & Opacities

| State | Core Fill Opacity | Glow Stroke Opacity | Glow Width | Top Line Opacity |
|---|---|---|---|---|
| ON (`on = true`) | 100% (`LV_OPA_COVER`) | 30% (`LV_OPA_30`, 76) | `min(vw, vh) · 0.22` | 35% (`LV_OPA_35`, 89) |
| OFF (`on = false`) | 30% (`LV_OPA_30`, 76) | 0% (`LV_OPA_TRANSP`) | 0 | 35% (`LV_OPA_35`, 89) |
| DISABLED (`LV_STATE_DISABLED`) | 15% (`LV_OPA_15`, 38) | 0% (`LV_OPA_TRANSP`) | 0 | 20% (`LV_OPA_20`, 51) |

Fixed colors:
- Bezel: `#303048`
- Well: `#181830`
- Top highlight line: `#7890D8`
- Core / Glow: dynamic `color` property (default `#F078B0`)

## 6. Rendering Hierarchy (`DRAW_MAIN`)

A single draw pass in `LV_EVENT_DRAW_MAIN` emits 5 clipped draw descriptors:

1. **Bezel:** `lv_draw_rect` over `[coords.x1, coords.y1, coords.x2, coords.y2]`, `bg_color = #303048`, `radius = round(1.5 · u)` (min 1).
2. **Well:** `lv_draw_rect` over well area inset by `round(1.6 · u)`, `bg_color = #181830`, `radius = 0`.
3. **Glow (if `on` and not disabled):**
   - For round: circle at `(cx, cy)` with radius `round((r + glowW / 2) · u)`.
   - For bar/pill: rect expanded by `round(glowW / 2 · u)` with corner radius `round((br + glowW / 2) · u)`.
   - `bg_color = color`, `bg_opa = 76`.
4. **Core:**
   - For round: circle at `(cx, cy)` with radius `round(r · u)`.
   - For bar/pill: rect at `(bx, by)` of size `bw × bh` with corner radius `round(br · u)`.
   - `bg_color = color`, `bg_opa = on ? 255 : 76` (halved if disabled).
5. **Top Highlight Line:**
   - Rect at top of well, height `round(topLine · u)`.
   - `bg_color = #7890D8`, `bg_opa = disabled ? 51 : 89`, `radius = 0`.

## 7. State Management & Delta Damage Model

- Structure definition:
  ```c
  typedef struct {
      lv_obj_t obj;
      uint32_t color;
      synthui_lamp_shape_t shape;
      bool on;
  } synthui_lamp_t;
  ```
- **Early-Return Rule:**
  ```c
  if (lamp->on == on) return;
  lamp->on = on;
  lv_obj_invalidate(obj);
  ```
  Applies identically to `set_shape` and `set_color`. Redundant updates cost 0 cycles in rendering and 0 damage area.
- **Damage Area:** `lv_obj_invalidate(obj)` marks the widget's bounding rectangle dirty. In a 16-lamp bank where 2 lamps toggle, only 3.2k pixels are touched, leaving >99% of the screen buffer intact.

## 8. Consumer Example & Gate (`rt1176-evkb`)

- Directory: `examples/display/synthui_lamp_test/`
- Pipeline: `lvgl_mipi_panel_create_db()` on RK055 panel (720×1280 XRGB8888).
- **Scene:** 16-lamp bank (4×4 grid) exercising all features:
  - Row 0: 4 round lamps (Pink on, Red on, Green off, Amber on)
  - Row 1: 4 bar lamps (Blue on, Cyan off, Pink off, Red on)
  - Row 2: 4 pill lamps (Green on, Amber off, Blue on, Cyan on)
  - Row 3: Edge cases (Disabled round lamp, custom hex colors, rapid toggle targets)
- **Phase A (QEMU-Gated Correctness):**
  1. Full initial render → emit `lamp_crc=0x...` (pinned golden).
  2. 64-step deterministic delta sequence: fixed-seed LCG toggles lamps across the grid.
  3. **Delta-Equality Guard:** `lamp_delta_crc` vs `lamp_fresh_crc`. Asserts `lamp_delta_eq=PASS`—verifies that delta-rendered state is bit-identical to a fresh full render.
  4. **Damage Engagement Check:** Measures `lamp_damage max=... total=...` across the 64 steps, asserting `max <= 8000` px/step (confirming partial invalidation rather than full screen refresh).
  5. Script checks `LVGL_FLUSHED=PASS`, `LVGL_BYTES=3686400`, `crc_done`, and prints `PASS: SynthUI lamp render verified`.
- **Phase B (Animation & FPS Benchmark):**
  - Un-gated running chaser / pattern animation measuring `lamp_fps` frame intervals, confirming sustained ≥30 fps.

## 9. Host Unit Tests (`SynthUI`)

- File: `SynthUI/tests/lamp_test.c`, integrated into `SynthUI/tests/run.sh`.
- Checks:
  - Coordinate scaling at various widget dimensions (48×48, 72×38, 34×16).
  - Shape enum validation and defaults.
  - Color palette definitions match DC hex values.
  - Setter early-return logic.
