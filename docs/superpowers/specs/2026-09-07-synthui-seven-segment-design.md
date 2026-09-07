# SynthUI SevenSegment — font strategy brainstorm + widget — design

Date: 2026-09-07
Status: approved (brainstorm 2026-09-07)
Tracking: Linear **NEW-29** ("SynthUI SevenSegment: font strategy brainstorm + widget").
Related: NEW-20 (RotaryKnob), NEW-23 (Fader), NEW-24 (Lamp), NEW-27 (PanelButton) — the clean-room architecture, delta discipline, double-buffered pipeline, and gate guards this design inherits.

## 1. Goal & Scope

Implement the **SevenSegment** primitive from the DC reference set (`SynthUI/reference/dc/SevenSegment.dc.html`) as a SynthUI LVGL 9 custom widget (`src/synthui_seven_segment`), verified by host tests in `SynthUI/tests/` and a new double-buffered evkb display example `examples/display/synthui_seven_segment_test` running under QEMU in `/Users/moolet/Development/qemu-rt1170/build` targeting the i.MX RT1176 / MIMXRT1170-EVKB.

Success criteria (from the Linear issue):

| Criterion | Met by |
|---|---|
| Sustained ≥30 fps while animating | SW-delta rendering: flat hexagonal vector primitives and triangle descriptors execute in sub-millisecond CPU draw time; Phase B animation loop measures frame interval on hardware and QEMU (§8) |
| Double-buffered | `lvgl_mipi_panel_create_db()` hardware double-buffer pipeline; flips synchronized to VSYNC ISR; goldens read presented buffer (`flip_sync()` + `scanned_fb()`) |
| No visible tearing or glitching | Back-buffer rendering + VSYNC flip by construction eliminates scanout tearing; per-boot delta-equality guard asserts mathematical pixel equivalence of delta vs full render |
| Delta rendering | Redraw only what changed: early-return guards on all setters suppress untouched frames; text changes invalidate only the bounding boxes of modified character cells |

## 2. Digit Rendering Strategy Evaluation (Brainstorm Decision)

Four candidate strategies were evaluated for NEW-29:

1. **Strategy A: Per-Segment Geometric Primitives (LVGL Software Vector Rendering)** — **Selected**
   - **Memory Cost:** 0 bytes RAM, 0 bytes ROM (no bitmap tables or font data).
   - **GPU Overhead:** 0 GPU paths (runs on Cortex-M7 sw pipeline; ~28 flat triangles per digit, <0.1 ms for 4 digits).
   - **Delta Support:** Invalidates only modified digit cells via `lv_obj_invalidate_area`.
   - **Flexibility:** Arbitrary scaling (16–320 px), arbitrary slant angle ($0^\circ$–$14^\circ$), clean independent styling for lit, unlit (ghost), glow bloom, dot, and colon.
   - **QEMU Compatibility:** Bit-exact deterministic rasterization for golden CRC verification.
2. **Strategy B: Pre-baked Digit Atlas (Bitmap sprites)** — Rejected: high flash usage (~120–500 kB), poor dynamic scaling, cannot easily do dynamic ghosting without dual masks.
3. **Strategy C: Pre-rendered Per-Segment Sprites** — Rejected: shearing requires transformed blits, memory bandwidth higher than direct triangle fills.
4. **Strategy D: VGLite GPU Vector Paths (GC355)** — Rejected: GC355 command buffer dispatch overhead for tiny geometries; QEMU is `gpu-absent`, so SW fallback is required regardless.

**Decision recorded in Linear NEW-29:** Strategy A selected.

## 3. Non-Goals

- No GPU/VGLite compositor TU in this issue (pure software vector primitives on Cortex-M7 easily exceed ≥30 fps).
- No dynamic heap allocations in widget setters (text capacity is clamped to 16 characters in static struct storage).
- No arbitrary non-sheared rotations (displays are axis-aligned with optional shear rake).

## 4. Widget Files & Public API (SynthUI)

Clean-room vector rebuild following the repository's provenance rules:

- `src/synthui_seven_segment_types.h` — Clean header with enums, bitmasks, and color definitions (LVGL-free for host tests)
- `src/synthui_seven_segment.h` — Public LVGL 9 widget API
- `src/synthui_seven_segment_math.h` — Header-only pure geometry math (LVGL-free for host tests)
- `src/synthui_seven_segment.cpp` — Widget implementation (`synthui_seven_segment_class`)
- `tests/seven_segment_test.c` — Host unit tests (geometry bounds, shear math, character bitmasks, setter early-exits)

### Types & Constants (`synthui_seven_segment_types.h`)

```c
#ifndef SYNTHUI_SEVEN_SEGMENT_TYPES_H
#define SYNTHUI_SEVEN_SEGMENT_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNTHUI_SEVEN_SEGMENT_MAX_CHARS 16

/* Standard DC reference accent colors (0xRRGGBB) */
#define SYNTHUI_SEVEN_SEGMENT_COLOR_BLUE_ON     0xDCECFF
#define SYNTHUI_SEVEN_SEGMENT_COLOR_BLUE_GLOW   0x90A8F0
#define SYNTHUI_SEVEN_SEGMENT_COLOR_CYAN_ON     0xD8F0F0
#define SYNTHUI_SEVEN_SEGMENT_COLOR_CYAN_GLOW   0x78C8D8
#define SYNTHUI_SEVEN_SEGMENT_COLOR_AMBER_ON    0xFFE0A8
#define SYNTHUI_SEVEN_SEGMENT_COLOR_AMBER_GLOW  0xF0A030
#define SYNTHUI_SEVEN_SEGMENT_COLOR_RED_ON      0xFFC8C0
#define SYNTHUI_SEVEN_SEGMENT_COLOR_RED_GLOW    0xE04848

#define SYNTHUI_SEVEN_SEGMENT_COLOR_DEFAULT_ON   SYNTHUI_SEVEN_SEGMENT_COLOR_BLUE_ON
#define SYNTHUI_SEVEN_SEGMENT_COLOR_DEFAULT_GLOW SYNTHUI_SEVEN_SEGMENT_COLOR_BLUE_GLOW

/* Segment bitmasks (a-g, dot, colon) */
#define SYNTHUI_SEG_A     (1u << 0)
#define SYNTHUI_SEG_B     (1u << 1)
#define SYNTHUI_SEG_C     (1u << 2)
#define SYNTHUI_SEG_D     (1u << 3)
#define SYNTHUI_SEG_E     (1u << 4)
#define SYNTHUI_SEG_F     (1u << 5)
#define SYNTHUI_SEG_G     (1u << 6)
#define SYNTHUI_SEG_DOT   (1u << 7)
#define SYNTHUI_SEG_COLON (1u << 8)

#ifdef __cplusplus
}
#endif
#endif /* SYNTHUI_SEVEN_SEGMENT_TYPES_H */
```

### Public API (`synthui_seven_segment.h`)

```c
#ifndef SYNTHUI_SEVEN_SEGMENT_H
#define SYNTHUI_SEVEN_SEGMENT_H

#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>
#include "synthui_seven_segment_types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_obj_class_t synthui_seven_segment_class;

lv_obj_t *synthui_seven_segment_create(lv_obj_t *parent);

/* Text display (clamped to SYNTHUI_SEVEN_SEGMENT_MAX_CHARS) */
void synthui_seven_segment_set_text(lv_obj_t *obj, const char *text);
const char *synthui_seven_segment_get_text(const lv_obj_t *obj);

/* Accent styling */
void synthui_seven_segment_set_accent(lv_obj_t *obj, uint32_t on_hex, uint32_t glow_hex);
uint32_t synthui_seven_segment_get_accent_on(const lv_obj_t *obj);
uint32_t synthui_seven_segment_get_accent_glow(const lv_obj_t *obj);

/* Ghost unlit segments (default true) */
void synthui_seven_segment_set_ghost(lv_obj_t *obj, bool ghost);
bool synthui_seven_segment_get_ghost(const lv_obj_t *obj);

/* Slant angle in degrees (default 6.0 deg, range 0..14) */
void synthui_seven_segment_set_slant(lv_obj_t *obj, float slant_deg);
float synthui_seven_segment_get_slant(const lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
#endif /* SYNTHUI_SEVEN_SEGMENT_H */
```

## 5. Geometry — Written Description of DC Reference

Geometry is evaluated in **viewBox unit space** ($H = 112$, $T = 12$, $t = T/2 = 6$, $ADV = 76$):

- **Horizontal Segments** $h(x, y, L)$ (6-vertex hexagon with 45° pointed ends):
  - $(x, y) \to (x+t, y-t) \to (x+L-t, y-t) \to (x+L, y) \to (x+L-t, y+t) \to (x+t, y+t)$
  - `a`: $h(15, 12, 34)$ (top horizontal)
  - `g`: $h(15, 56, 34)$ (middle horizontal)
  - `d`: $h(15, 100, 34)$ (bottom horizontal)
- **Vertical Segments** $v(x, y, L)$ (6-vertex hexagon with 45° pointed ends):
  - $(x, y) \to (x+t, y+t) \to (x+t, y+L-t) \to (x, y+L) \to (x-t, y+L-t) \to (x-t, y+t)$
  - `f`: $v(12, 15, 38)$ (upper left)
  - `b`: $v(58, 15, 38)$ (upper right)
  - `e`: $v(12, 59, 38)$ (lower left)
  - `c`: $v(58, 59, 38)$ (lower right)
- **Standalone Punctuation Characters (1 char = 1 cell, horizontally centered):**
  - Cell width: $W_{\text{punct}} = 44$ units, horizontal center $cx = 22$.
  - Period `.` : circle at $(cx=22, cy=100, r=7)$. No unlit ghost segments.
  - Colon `:` : circles at $(cx=22, cy=38, r=6)$ and $(cx=22, cy=76, r=6)$. No unlit ghost segments.
- **Slant Shear Transformation:**
  - Shear factor $S = \tan(\text{slant} \cdot \pi / 180)$ (default $6^\circ$, $S \approx 0.1051$).
  - Bottom-aligned transformation:
    $$x_{\text{sheared}} = x + (H - y) \cdot S$$
  - Guarantees $x_{\text{sheared}} \ge 0$ across the full display area. Overhang width $W_{\text{overhang}} = H \cdot S$.
- **Scale Factor:**
  $$u = \frac{h}{112.0\text{f}}$$
  Screen coordinates:
  $$X_{\text{screen}} = X_{\text{obj}} + \text{round}(x_{\text{sheared}} \cdot u), \quad Y_{\text{screen}} = Y_{\text{obj}} + \text{round}(y \cdot u)$$

## 6. Palette & Rendering Hierarchy (`DRAW_MAIN`)

| Element | Color (`blue`) | Color (`cyan`) | Color (`amber`) | Color (`red`) | Opacity |
|---|---|---|---|---|---|
| Lit Segment Core | `#DCECFF` | `#D8F0F0` | `#FFE0A8` | `#FFC8C0` | 100% (`LV_OPA_COVER`) |
| Lit Segment Glow | `#90A8F0` | `#78C8D8` | `#F0A030` | `#E04848` | 28% (71 / 255) |
| Ghost Unlit Core | `#90A8F0` | `#78C8D8` | `#F0A030` | `#E04848` | 11% (28 / 255) |
| Dot / Colon Core | `#DCECFF` | `#D8F0F0` | `#FFE0A8` | `#FFC8C0` | 100% (`LV_OPA_COVER`) |
| Dot / Colon Glow | `#90A8F0` | `#78C8D8` | `#F0A030` | `#E04848` | 30% (76 / 255) |
| Well Background | `#181830` | `#181830` | `#181830` | `#181830` | 100% (`LV_OPA_COVER` default) |

*(When `LV_STATE_DISABLED` is present, core and glow opacities are scaled by 50%).*

### Draw Passes per Intersecting Cell in `LV_EVENT_DRAW_MAIN`:
1. **Background:** `lv_draw_rect` covering the cell with `#181830` to erase previous frame content.
2. **Ghost Segments:** If `ghost == true` and character is not `.` or `:`, draw all 7 unlit segments as 4 triangles each with `color = glow`, `opa = 28`.
3. **Lit Glow Bloom:** For lit segments, draw stroke along hexagon perimeter with `width = round(7 * u)`, `color = glow`, `opa = 71`. For dot/colon, draw outer circle.
4. **Lit Core:** For lit segments, draw 4 core triangles with `color = on`, `opa = LV_OPA_COVER`. For dot/colon, draw filled circles.

## 7. State Management & Delta Invalidation

- **Widget Struct:**
  ```c
  typedef struct {
      lv_obj_t obj;
      char text[SYNTHUI_SEVEN_SEGMENT_MAX_CHARS + 1];
      uint32_t on_color;
      uint32_t glow_color;
      float slant;
      bool ghost;
  } synthui_seven_segment_t;
  ```
- **Early-Return Rule:**
  ```c
  if (strncmp(seg->text, text, SYNTHUI_SEVEN_SEGMENT_MAX_CHARS) == 0) return;
  ```
  Redundant updates cost 0 CPU cycles and produce 0 damaged pixels.
- **Delta Cell Damage:**
  When `strlen(new_text) == strlen(curr_text)`:
  - Find all indices $i$ where `new_text[i] != curr_text[i]`.
  - Calculate the screen bounding rect of each changed cell $i$.
  - Invalidate only those cell rects with `lv_obj_invalidate_area(obj, &cell_area)`.
  - Touches only ~$\frac{1}{N}$ of the widget area per single-digit update.
- If string length changes: call full `lv_obj_invalidate(obj)`.

## 8. Consumer Example & Gate (`rt1176-evkb`)

- Directory: `examples/display/synthui_seven_segment_test/`
- Pipeline: `lvgl_mipi_panel_create_db()` on RK055 panel (720×1280 XRGB8888).
- **Scene:** Synth control surface display:
  - Tempo readout: `"140.0"` ($h = 96$, Blue accent).
  - Clock readout: `"01:04"` ($h = 56$, Cyan accent).
  - Patch readout: `"P-A3"` ($h = 56$, Amber accent).
  - Status readout: `"REC.8"` ($h = 56$, Red accent).
  - Variations: Ghost-off readout (`ghost = false`, $h = 44$) and Disabled readout (`LV_STATE_DISABLED`, $h = 44$).
- **Phase A (QEMU Gated Correctness):**
  1. Full initial render $\to$ emit `seven_segment_crc=0x...` (pinned golden).
  2. 64-step deterministic delta sequence (counter ticks).
  3. **Delta-Equality Guard:** `seven_segment_delta_crc` vs `seven_segment_fresh_crc`. Asserts `seven_segment_delta_eq=PASS`.
  4. **Damage Engagement Check:** Asserts `seven_segment_damage max <= 20000` px across the 64 steps.
  5. **VSYNC Guard:** Asserts `seven_segment_vsync timeouts=0`.
  6. Script checks `LVGL_FLUSHED=PASS`, `LVGL_BYTES=3686400`, `crc_done`, and prints `PASS: SynthUI seven_segment render verified`.
- **Phase B (Animation & FPS Benchmark):**
  - Continuous running animation loop measuring `seven_segment_fps` frame intervals, confirming sustained $\ge 30$ fps.
- **Eyeball Support:**
  - `SEVEN_SEGMENT_EYEBALL_HOLD` compile macro for static framebuffer inspection.

## 9. Host Unit Tests (`SynthUI`)

- File: `SynthUI/tests/seven_segment_test.c`, integrated into `SynthUI/tests/run.sh`.
- Checks:
  - Hexagonal vertices and triangle decomposition for all segments $a$–$g$.
  - Punctuation centering: verify `.` and `:` are centered at $W_{\text{punct}} / 2 = 22$.
  - Slant shear transformations at $0^\circ, 6^\circ, 14^\circ$.
  - Bitmask mappings for all 27 reference characters + `.` and `:`.
  - Scale conversions and bounding rects.
  - Safe truncation at 16 characters.
  - Degenerate dimensions reject gracefully ($h \le 0$).
