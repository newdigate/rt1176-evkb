# SynthUI LedButton Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the DC LedButton primitive as `synthui_led_button` in SynthUI (LVGL 9, sw delta rendering, a `pressed` LATCH for the acid_box selection), verified by host unit tests and a new double-buffered QEMU-gated example `examples/display/synthui_led_button_test` in rt1176-evkb.

**Architecture:** Spec: `docs/superpowers/specs/2026-09-15-synthui-led-button-design.md` (Linear NEW-25). One widget TU with an eight-layer `LV_EVENT_DRAW_MAIN` pass driven by a pure, host-tested layout header; every setter early-returns on no change and invalidates ONLY the box its state paints (LED+halo, the cap group at both press offsets, or the whole key for cue/disabled). The consumer example runs the db pipeline (`lvgl_mipi_panel_create_db()`), Phase A pins a render golden, a 64-step LCG delta sequence against a fresh full render, an engagement bound and the vsync witness; Phase B animates for fps (ungated).

**Tech Stack:** LVGL 9.4.0 (sibling `~/Development/LVGL`, `LV_GRADIENT_MAX_STOPS 2`), SynthUI (`/Users/nicholasnewdigate/Development/SynthUI`, local-first), rt1176-evkb CMake + qemu2 through `tools/qrun`, ARM GCC 10 at `/Applications/ARM_10/bin/`.

## Global Constraints

- Clean-room provenance: `SynthUI/reference/` is NEVER compiled; every number below is the spec's written description of the DC geometry.
- `./run_qemu.sh`, never `sh run_qemu.sh` (it re-execs under `gtimeout`).
- Goldens are RECORDED from two bit-identical consecutive QEMU runs, never derived, never pasted in from a red run.
- Every gate assertion is DEMONSTRATED RED before it is trusted; the demonstration is quoted in the script header.
- Two repos, two commit streams: SynthUI commits carry the widget; evkb commits carry the example, gate and docs. The evkb pin bump comes LAST, after the SynthUI push.
- Paths: `SYNTHUI=/Users/nicholasnewdigate/Development/SynthUI`, `EVKB=/Users/nicholasnewdigate/Development/rt1170/evkb`.

## File Structure

SynthUI:
- `src/synthui_led_button_types.h` — colour enum + DC on/off table (LVGL-free)
- `src/synthui_led_button_math.h` — pure layout, damage boxes, palette (LVGL-free, header-only)
- `src/synthui_led_button.h` — public widget API
- `src/synthui_led_button.cpp` — `synthui_led_button_class`: draw, input hooks, setters
- `tests/led_button_test.c` + `tests/run.sh` — host suite

rt1176-evkb:
- `examples/display/synthui_led_button_test/{CMakeLists.txt, synthui_led_button_test.cpp, run_qemu.sh, transcript_qemu.txt}`
- `tools/license-audit.sh` (GATES entry), `tools/gate-vacuity.test.sh` (section 15), `examples/README.md`, `CLAUDE.md`, `evkb.cmake` (pin)

---

### Task 1: Types, layout math, palette + host unit test (SynthUI)

> **Executed 2026-09-15; the code below is SUPERSEDED by review.** Quality review found that adding the 2.5-unit press offset before rounding resized and slipped layers by a pixel (the LED shrank 15 -> 14 px at 96 px). The committed header (SynthUI `0fcffa2`) stores every rect at dy 0, carries `int32_t dy_px` added after rounding, converts through `synthui_led_button_rect_px` / `_circle_px`, returns `synthui_led_button_px_t` damage boxes, and puts `bezel_bw_px` / `cue_bw_px` / `halo_bw_px` in the layout; its test adds a pixel-space containment sweep. The repo is authoritative for Task 1; Task 2's code below is written against the committed API.

**Files:**
- Create: `$SYNTHUI/src/synthui_led_button_types.h`
- Create: `$SYNTHUI/src/synthui_led_button_math.h`
- Create: `$SYNTHUI/tests/led_button_test.c`
- Modify: `$SYNTHUI/tests/run.sh`

- [ ] **Step 1: Write the failing host test**

Create `$SYNTHUI/tests/led_button_test.c`:

```c
/* led_button_test.c - host unit test for the pure LedButton layout, damage
 * boxes and palette (synthui_led_button_math.h).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Every guard here was shown RED against a mutant before it was trusted:
 *   press box ignoring dy      -> "press box must cover the base at both offsets"
 *   lit box omitting the halo  -> "lit box must contain the halo"
 *   cap split at 0.50          -> "cap split sits at 62 %"
 *   dots threshold at 32       -> "dots drop out below 34 px" */
#undef NDEBUG
#include "../src/synthui_led_button_math.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static int approx(float a, float b) { return fabsf(a - b) < 0.01f; }

static int rect_contains(const synthui_led_button_rect_t *outer,
                         const synthui_led_button_rect_t *inner)
{
    return inner->x >= outer->x - 0.001f && inner->y >= outer->y - 0.001f &&
           inner->x + inner->w <= outer->x + outer->w + 0.001f &&
           inner->y + inner->h <= outer->y + outer->h + 0.001f;
}

int main(void)
{
    synthui_led_button_layout_t L;

    /* --- 100x100, unpressed: the DC box in pixels, 1 unit = 1 px --- */
    assert(synthui_led_button_compute_layout(100.0f, 100.0f, false, &L));
    assert(approx(L.s, 1.0f) && approx(L.ox, 0.0f) && approx(L.oy, 0.0f));
    assert(approx(L.dy, 0.0f));
    assert(L.dots_visible);
    assert(approx(L.bezel.x, 0) && approx(L.bezel.y, 0) && approx(L.bezel.w, 100) && approx(L.bezel.h, 100));
    assert(approx(L.well.x, 7) && approx(L.well.y, 6) && approx(L.well.w, 86) && approx(L.well.h, 88));
    assert(approx(L.cap.x, 10) && approx(L.cap.y, 9) && approx(L.cap.w, 80) && approx(L.cap.h, 80));
    assert(approx(L.highlight.x, 14) && approx(L.highlight.y, 12) && approx(L.highlight.w, 72) && approx(L.highlight.h, 11));
    assert(approx(L.led.x, 26) && approx(L.led.y, 19) && approx(L.led.w, 48) && approx(L.led.h, 15));
    assert(approx(L.halo.x, 21) && approx(L.halo.y, 14) && approx(L.halo.w, 58) && approx(L.halo.h, 25));
    assert(approx(L.base.x, 10) && approx(L.base.y, 82) && approx(L.base.w, 80) && approx(L.base.h, 7));
    assert(approx(L.dot1.cx, 38) && approx(L.dot1.cy, 26.5f) && approx(L.dot1.r, 1.7f));
    assert(approx(L.dot2.cx, 62) && approx(L.dot2.cy, 26.5f) && approx(L.dot2.r, 1.7f));
    assert(approx(L.bezel_r, 17) && approx(L.well_r, 13) && approx(L.cap_r, 11));
    assert(approx(L.highlight_r, 5.5f) && approx(L.halo_r, 8.5f) && approx(L.led_r, 3.5f) && approx(L.base_r, 3.5f));

    /* cap split sits at 62 % and the two halves tile the cap exactly */
    assert(approx(L.cap_top.y, 9) && approx(L.cap_top.h, 49.6f));
    assert(approx(L.cap_low.y, 58.6f) && approx(L.cap_low.h, 30.4f));
    assert(approx(L.cap_top.y + L.cap_top.h, L.cap_low.y));
    assert(approx(L.cap_top.h + L.cap_low.h, L.cap.h));
    assert(approx(L.cap_top.x, L.cap.x) && approx(L.cap_top.w, L.cap.w));
    assert(approx(L.cap_low.x, L.cap.x) && approx(L.cap_low.w, L.cap.w));

    /* --- pressed: dy = 2.5, only the cap group moves --- */
    synthui_led_button_layout_t P;
    assert(synthui_led_button_compute_layout(100.0f, 100.0f, true, &P));
    assert(approx(P.dy, 2.5f));
    assert(approx(P.bezel.y, L.bezel.y) && approx(P.well.y, L.well.y));
    assert(approx(P.cap.y, 11.5f) && approx(P.cap_top.y, 11.5f) && approx(P.cap_low.y, 61.1f));
    assert(approx(P.highlight.y, 14.5f) && approx(P.led.y, 21.5f) && approx(P.halo.y, 16.5f));
    assert(approx(P.base.y, 84.5f) && approx(P.dot1.cy, 29.0f) && approx(P.dot2.cy, 29.0f));

    /* --- scaling: 200x200 doubles everything, 2.5 units -> 5 px --- */
    synthui_led_button_layout_t D;
    assert(synthui_led_button_compute_layout(200.0f, 200.0f, true, &D));
    assert(approx(D.s, 2.0f) && approx(D.dy, 5.0f));
    assert(approx(D.led.x, 52) && approx(D.led.y, 43) && approx(D.led.w, 96) && approx(D.led.h, 30));
    assert(approx(D.cap_r, 22) && approx(D.dot1.r, 3.4f));

    /* --- non-square 120x80: an 80 px key centred with 20 px side margins --- */
    synthui_led_button_layout_t N;
    assert(synthui_led_button_compute_layout(120.0f, 80.0f, false, &N));
    assert(approx(N.s, 0.8f) && approx(N.ox, 20.0f) && approx(N.oy, 0.0f));
    assert(approx(N.bezel.x, 20) && approx(N.bezel.w, 80) && approx(N.bezel.h, 80));
    assert(approx(N.led.x, 20 + 26 * 0.8f));

    /* --- dots drop out below 34 px --- */
    synthui_led_button_layout_t S;
    assert(synthui_led_button_compute_layout(33.0f, 33.0f, false, &S));
    assert(!S.dots_visible);
    assert(synthui_led_button_compute_layout(34.0f, 34.0f, false, &S));
    assert(S.dots_visible);
    assert(synthui_led_button_compute_layout(120.0f, 33.0f, false, &S));   /* min side rules */
    assert(!S.dots_visible);

    /* --- damage boxes --- */
    /* lit box == halo box, at the CURRENT press offset, and contains the LED */
    synthui_led_button_rect_t lb;
    synthui_led_button_lit_box(100.0f, 100.0f, false, &lb);
    assert(approx(lb.x, 21) && approx(lb.y, 14) && approx(lb.w, 58) && approx(lb.h, 25));
    assert(rect_contains(&lb, &L.led) && rect_contains(&lb, &L.halo));
    synthui_led_button_lit_box(100.0f, 100.0f, true, &lb);
    assert(approx(lb.y, 16.5f));
    assert(rect_contains(&lb, &P.led) && rect_contains(&lb, &P.halo));
    /* press box covers every moving layer at BOTH offsets, and lies in the key */
    synthui_led_button_rect_t pb;
    synthui_led_button_press_box(100.0f, 100.0f, &pb);
    assert(approx(pb.x, 10) && approx(pb.y, 9) && approx(pb.w, 80) && approx(pb.h, 82.5f));
    assert(rect_contains(&pb, &L.cap) && rect_contains(&pb, &P.cap));
    assert(rect_contains(&pb, &L.highlight) && rect_contains(&pb, &P.highlight));
    assert(rect_contains(&pb, &L.halo) && rect_contains(&pb, &P.halo));
    assert(rect_contains(&pb, &L.base) && rect_contains(&pb, &P.base));   /* "press box must cover the base at both offsets" */
    assert(rect_contains(&L.bezel, &pb));
    synthui_led_button_press_box(200.0f, 200.0f, &pb);
    assert(approx(pb.h, 165.0f));

    /* --- colour table --- */
    assert(synthui_led_button_color_on(SYNTHUI_LED_BUTTON_RED)   == 0xFF3B30u);
    assert(synthui_led_button_color_off(SYNTHUI_LED_BUTTON_RED)  == 0x5E2B28u);
    assert(synthui_led_button_color_on(SYNTHUI_LED_BUTTON_AMBER) == 0xFFA41Fu);
    assert(synthui_led_button_color_off(SYNTHUI_LED_BUTTON_AMBER)== 0x5C3D18u);
    assert(synthui_led_button_color_on(SYNTHUI_LED_BUTTON_GREEN) == 0x4BE060u);
    assert(synthui_led_button_color_off(SYNTHUI_LED_BUTTON_GREEN)== 0x254A2Bu);
    assert(synthui_led_button_color_on(SYNTHUI_LED_BUTTON_BLUE)  == 0x5AA8FFu);
    assert(synthui_led_button_color_off(SYNTHUI_LED_BUTTON_BLUE) == 0x22384Fu);
    assert(synthui_led_button_color_on((synthui_led_button_color_t)99) == 0xFF3B30u);   /* out of range -> red */

    /* --- palette --- */
    synthui_led_button_palette_t p;
    synthui_led_button_palette(SYNTHUI_LED_BUTTON_RED, false, false, false, false, &p);
    assert(p.cap_top == 0xF7F5F1u && p.cap_mid == 0xE8E6E1u && p.cap_low == 0xC9C7C1u);
    assert(p.highlight_opa == 140 && p.base_opa == 217);
    assert(p.led_fill == 0x5E2B28u && !p.halo_on);
    assert(p.bezel_color == 0x3A3A3Du && approx(p.bezel_w_units, 2.0f));

    synthui_led_button_palette(SYNTHUI_LED_BUTTON_AMBER, true, false, false, false, &p);
    assert(p.led_fill == 0xFFA41Fu && p.halo_on && p.halo_color == 0xFFA41Fu);

    synthui_led_button_palette(SYNTHUI_LED_BUTTON_RED, true, true, false, false, &p);
    assert(p.cap_top == 0xDEDCD7u && p.cap_mid == 0xCBC9C3u && p.cap_low == 0xB4B2ADu);
    assert(p.highlight_opa == 71 && p.base_opa == 128);
    assert(p.halo_on);                                   /* pressed keeps the halo */

    synthui_led_button_palette(SYNTHUI_LED_BUTTON_RED, false, false, true, false, &p);
    assert(p.bezel_color == 0xFF3B30u && approx(p.bezel_w_units, 3.5f));

    synthui_led_button_palette(SYNTHUI_LED_BUTTON_GREEN, true, false, true, true, &p);
    assert(p.cap_top == 0xE2E1DEu && p.cap_mid == 0xD2D1CEu && p.cap_low == 0xBCBBB8u);
    assert(p.led_fill == 0x4A4A4Cu && !p.halo_on);       /* disabled: neutral LED, no halo, even when lit */
    assert(p.highlight_opa == 140 && p.base_opa == 217);  /* disabled is not pressed */
    assert(p.bezel_color == 0xFF3B30u);                  /* cue still shows on a disabled key */

    synthui_led_button_palette(SYNTHUI_LED_BUTTON_RED, true, true, false, true, &p);
    assert(p.cap_top == 0xE2E1DEu && p.highlight_opa == 71 && p.base_opa == 128);   /* disabled + pressed: disabled cap, pressed opacities */

    /* --- degenerate sizes --- */
    assert(!synthui_led_button_compute_layout(0.0f, 100.0f, false, &L));
    assert(!synthui_led_button_compute_layout(100.0f, -1.0f, false, &L));

    printf("led_button_test: all PASS\n");
    return 0;
}
```

Append to `$SYNTHUI/tests/run.sh` (after the `panel_button_test` pair, before `seven_segment_test`):

```sh
cc -Wall -Wextra -Werror -o "$out/led_button_test" tests/led_button_test.c
"$out/led_button_test"
```

- [ ] **Step 2: Run the suite to verify it fails**

Run: `cd $SYNTHUI && ./tests/run.sh`
Expected: FAIL with `fatal error: '../src/synthui_led_button_math.h' file not found` (the earlier suites still pass first).

- [ ] **Step 3: Create `src/synthui_led_button_types.h`**

```c
/* synthui_led_button_types.h - types and constants for SynthUI LedButton.
 * Header-only and LVGL-free for host testing.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_LED_BUTTON_TYPES_H
#define SYNTHUI_LED_BUTTON_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNTHUI_LED_BUTTON_RED = 0,   /* DC default */
    SYNTHUI_LED_BUTTON_AMBER,
    SYNTHUI_LED_BUTTON_GREEN,
    SYNTHUI_LED_BUTTON_BLUE,
} synthui_led_button_color_t;

/* DC LED table, 0xRRGGBB: {on, off} per colour. */
#define SYNTHUI_LED_BUTTON_RED_ON     0xFF3B30u
#define SYNTHUI_LED_BUTTON_RED_OFF    0x5E2B28u
#define SYNTHUI_LED_BUTTON_AMBER_ON   0xFFA41Fu
#define SYNTHUI_LED_BUTTON_AMBER_OFF  0x5C3D18u
#define SYNTHUI_LED_BUTTON_GREEN_ON   0x4BE060u
#define SYNTHUI_LED_BUTTON_GREEN_OFF  0x254A2Bu
#define SYNTHUI_LED_BUTTON_BLUE_ON    0x5AA8FFu
#define SYNTHUI_LED_BUTTON_BLUE_OFF   0x22384Fu

/* Chrome, 0xRRGGBB. */
#define SYNTHUI_LED_BUTTON_BEZEL       0x1C1C1Eu
#define SYNTHUI_LED_BUTTON_BEZEL_STROKE 0x3A3A3Du
#define SYNTHUI_LED_BUTTON_CUE_STROKE  0xFF3B30u
#define SYNTHUI_LED_BUTTON_WELL        0x101012u
#define SYNTHUI_LED_BUTTON_BASE        0x8E8B84u
#define SYNTHUI_LED_BUTTON_LED_DISABLED 0x4A4A4Cu

#ifdef __cplusplus
}
#endif
#endif /* SYNTHUI_LED_BUTTON_TYPES_H */
```

- [ ] **Step 4: Create `src/synthui_led_button_math.h`**

```c
/* synthui_led_button_math.h - pure layout, damage boxes and palette for the
 * SynthUI LedButton.  Header-only and LVGL-free for direct host unit testing.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Geometry is the spec's written description of the DC reference
 * (docs/superpowers/specs/2026-09-15-synthui-led-button-design.md section 4):
 * a 100-unit box scaled by s = min(w,h)/100 and centred; the press offset dy
 * is 2.5 units and moves every layer except the bezel and the well. */
#ifndef SYNTHUI_LED_BUTTON_MATH_H
#define SYNTHUI_LED_BUTTON_MATH_H

#include <stdbool.h>
#include <stdint.h>
#include "synthui_led_button_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYNTHUI_LED_BUTTON_UNIT        100.0f
#define SYNTHUI_LED_BUTTON_PRESS_DY    2.5f    /* units */
#define SYNTHUI_LED_BUTTON_HALO_REACH  5.0f    /* units the halo extends past the LED */
#define SYNTHUI_LED_BUTTON_CAP_SPLIT   0.62f   /* top->mid over the first 62 % of the cap */
#define SYNTHUI_LED_BUTTON_DOTS_MIN_PX 34.0f   /* the sheet: below 34 px the dots drop out */

/* 8-bit opacities from the DC alphas (x255, rounded) */
#define SYNTHUI_LED_BUTTON_WELL_OPA        230   /* 0.90 */
#define SYNTHUI_LED_BUTTON_HIGHLIGHT_OPA   140   /* 0.55 */
#define SYNTHUI_LED_BUTTON_HIGHLIGHT_OPA_P  71   /* 0.28 pressed */
#define SYNTHUI_LED_BUTTON_BASE_OPA        217   /* 0.85 */
#define SYNTHUI_LED_BUTTON_BASE_OPA_P      128   /* 0.50 pressed */
#define SYNTHUI_LED_BUTTON_DOT_OPA          56   /* 0.22 */
#define SYNTHUI_LED_BUTTON_HALO_OPA         97   /* 0.38 */

typedef struct { float x, y, w, h; } synthui_led_button_rect_t;
typedef struct { float cx, cy, r; } synthui_led_button_circle_t;

typedef struct {
    float s;              /* px per unit */
    float ox, oy;         /* centring offset of the 100-unit box, px */
    float dy;             /* press offset, px */
    bool  dots_visible;
    synthui_led_button_rect_t bezel, well, cap, cap_top, cap_low, highlight, halo, led, base;
    synthui_led_button_circle_t dot1, dot2;
    float bezel_r, well_r, cap_r, highlight_r, halo_r, led_r, base_r;   /* px */
} synthui_led_button_layout_t;

typedef struct {
    uint32_t cap_top, cap_mid, cap_low;
    uint8_t  highlight_opa, base_opa;
    uint32_t led_fill;
    uint32_t halo_color;
    bool     halo_on;
    uint32_t bezel_color;
    float    bezel_w_units;
} synthui_led_button_palette_t;

static inline uint32_t synthui_led_button_color_on(synthui_led_button_color_t c)
{
    switch (c) {
    case SYNTHUI_LED_BUTTON_AMBER: return SYNTHUI_LED_BUTTON_AMBER_ON;
    case SYNTHUI_LED_BUTTON_GREEN: return SYNTHUI_LED_BUTTON_GREEN_ON;
    case SYNTHUI_LED_BUTTON_BLUE:  return SYNTHUI_LED_BUTTON_BLUE_ON;
    default:                       return SYNTHUI_LED_BUTTON_RED_ON;
    }
}

static inline uint32_t synthui_led_button_color_off(synthui_led_button_color_t c)
{
    switch (c) {
    case SYNTHUI_LED_BUTTON_AMBER: return SYNTHUI_LED_BUTTON_AMBER_OFF;
    case SYNTHUI_LED_BUTTON_GREEN: return SYNTHUI_LED_BUTTON_GREEN_OFF;
    case SYNTHUI_LED_BUTTON_BLUE:  return SYNTHUI_LED_BUTTON_BLUE_OFF;
    default:                       return SYNTHUI_LED_BUTTON_RED_OFF;
    }
}

static inline synthui_led_button_rect_t synthui_led_button_unit_rect(
    float ox, float oy, float s, float x, float y, float w, float h)
{
    synthui_led_button_rect_t r;
    r.x = ox + x * s; r.y = oy + y * s; r.w = w * s; r.h = h * s;
    return r;
}

static inline bool synthui_led_button_compute_layout(float w, float h, bool pressed,
                                                     synthui_led_button_layout_t *L)
{
    if (w <= 0.0f || h <= 0.0f) return false;
    const float side = w < h ? w : h;
    const float s = side / SYNTHUI_LED_BUTTON_UNIT;
    const float ox = (w - side) * 0.5f;
    const float oy = (h - side) * 0.5f;
    const float dy = pressed ? SYNTHUI_LED_BUTTON_PRESS_DY : 0.0f;
    const float cap_top_h = 80.0f * SYNTHUI_LED_BUTTON_CAP_SPLIT;

    L->s = s; L->ox = ox; L->oy = oy; L->dy = dy * s;
    L->dots_visible = side >= SYNTHUI_LED_BUTTON_DOTS_MIN_PX;

    L->bezel     = synthui_led_button_unit_rect(ox, oy, s, 0.0f, 0.0f, 100.0f, 100.0f);
    L->well      = synthui_led_button_unit_rect(ox, oy, s, 7.0f, 6.0f, 86.0f, 88.0f);
    L->cap       = synthui_led_button_unit_rect(ox, oy, s, 10.0f, 9.0f + dy, 80.0f, 80.0f);
    L->cap_top   = synthui_led_button_unit_rect(ox, oy, s, 10.0f, 9.0f + dy, 80.0f, cap_top_h);
    L->cap_low   = synthui_led_button_unit_rect(ox, oy, s, 10.0f, 9.0f + dy + cap_top_h, 80.0f, 80.0f - cap_top_h);
    L->highlight = synthui_led_button_unit_rect(ox, oy, s, 14.0f, 12.0f + dy, 72.0f, 11.0f);
    L->led       = synthui_led_button_unit_rect(ox, oy, s, 26.0f, 19.0f + dy, 48.0f, 15.0f);
    L->halo      = synthui_led_button_unit_rect(ox, oy, s,
                       26.0f - SYNTHUI_LED_BUTTON_HALO_REACH, 19.0f + dy - SYNTHUI_LED_BUTTON_HALO_REACH,
                       48.0f + 2.0f * SYNTHUI_LED_BUTTON_HALO_REACH, 15.0f + 2.0f * SYNTHUI_LED_BUTTON_HALO_REACH);
    L->base      = synthui_led_button_unit_rect(ox, oy, s, 10.0f, 82.0f + dy, 80.0f, 7.0f);
    L->dot1.cx = ox + 38.0f * s; L->dot1.cy = oy + (26.5f + dy) * s; L->dot1.r = 1.7f * s;
    L->dot2.cx = ox + 62.0f * s; L->dot2.cy = oy + (26.5f + dy) * s; L->dot2.r = 1.7f * s;

    /* SVG radii are the path's; a stroke adds half its width outside, so the
     * bezel (rx 16, stroke 2) and the halo (rx 3.5, stroke 10) carry the OUTER
     * radius here because LVGL draws borders inside the area. */
    L->bezel_r = 17.0f * s;
    L->well_r = 13.0f * s;
    L->cap_r = 11.0f * s;
    L->highlight_r = 5.5f * s;
    L->halo_r = (3.5f + SYNTHUI_LED_BUTTON_HALO_REACH) * s;
    L->led_r = 3.5f * s;
    L->base_r = 3.5f * s;
    return true;
}

/* Damage box for a lit/colour change: the halo box at the CURRENT press offset. */
static inline void synthui_led_button_lit_box(float w, float h, bool pressed,
                                              synthui_led_button_rect_t *out)
{
    synthui_led_button_layout_t L;
    if (!synthui_led_button_compute_layout(w, h, pressed, &L)) { out->x = out->y = out->w = out->h = 0.0f; return; }
    *out = L.halo;
}

/* Damage box for a press change: the cap group at BOTH offsets --
 * x 10..90, y 9 .. (82 + 7 + 2.5) = 91.5 units. */
static inline void synthui_led_button_press_box(float w, float h, synthui_led_button_rect_t *out)
{
    synthui_led_button_layout_t L;
    if (!synthui_led_button_compute_layout(w, h, false, &L)) { out->x = out->y = out->w = out->h = 0.0f; return; }
    *out = synthui_led_button_unit_rect(L.ox, L.oy, L.s, 10.0f, 9.0f, 80.0f, 82.5f);
}

static inline void synthui_led_button_palette(synthui_led_button_color_t color,
                                              bool lit, bool pressed, bool cue, bool disabled,
                                              synthui_led_button_palette_t *p)
{
    if (disabled)      { p->cap_top = 0xE2E1DEu; p->cap_mid = 0xD2D1CEu; p->cap_low = 0xBCBBB8u; }
    else if (pressed)  { p->cap_top = 0xDEDCD7u; p->cap_mid = 0xCBC9C3u; p->cap_low = 0xB4B2ADu; }
    else               { p->cap_top = 0xF7F5F1u; p->cap_mid = 0xE8E6E1u; p->cap_low = 0xC9C7C1u; }
    p->highlight_opa = pressed ? SYNTHUI_LED_BUTTON_HIGHLIGHT_OPA_P : SYNTHUI_LED_BUTTON_HIGHLIGHT_OPA;
    p->base_opa      = pressed ? SYNTHUI_LED_BUTTON_BASE_OPA_P : SYNTHUI_LED_BUTTON_BASE_OPA;
    p->led_fill      = disabled ? SYNTHUI_LED_BUTTON_LED_DISABLED
                                : (lit ? synthui_led_button_color_on(color) : synthui_led_button_color_off(color));
    p->halo_color    = synthui_led_button_color_on(color);
    p->halo_on       = lit && !disabled;
    p->bezel_color   = cue ? SYNTHUI_LED_BUTTON_CUE_STROKE : SYNTHUI_LED_BUTTON_BEZEL_STROKE;
    p->bezel_w_units = cue ? 3.5f : 2.0f;
}

#ifdef __cplusplus
}
#endif
#endif /* SYNTHUI_LED_BUTTON_MATH_H */
```

- [ ] **Step 5: Run the suite to verify it passes**

Run: `cd $SYNTHUI && ./tests/run.sh`
Expected: `led_button_test: all PASS` among the others, exit 0.

- [ ] **Step 6: Demonstrate three guards RED (mutants), then restore**

In `synthui_led_button_math.h`:
1. change the press box height `82.5f` to `80.0f` → run → expected assert failure on the line commented `"press box must cover the base at both offsets"`;
2. change `SYNTHUI_LED_BUTTON_CAP_SPLIT` to `0.50f` → run → assert on `approx(L.cap_top.h, 49.6f)`;
3. change `SYNTHUI_LED_BUTTON_DOTS_MIN_PX` to `32.0f` → run → assert on `!S.dots_visible` for 33 px;
4. in `synthui_led_button_lit_box` replace `*out = L.halo;` with `*out = L.led;` → run → assert on `rect_contains(&lb, &L.halo)` ("lit box must contain the halo").

Restore each after it fails. The four failing assert lines are the ones named in the test file's header comment (already written to match).

- [ ] **Step 7: Commit (SynthUI)**

```bash
cd $SYNTHUI
git add src/synthui_led_button_types.h src/synthui_led_button_math.h tests/led_button_test.c tests/run.sh
git commit -m "synthui_led_button: types, pure layout/damage/palette math and host unit tests (NEW-25)

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 2: The LVGL widget (SynthUI)

**Files:**
- Create: `$SYNTHUI/src/synthui_led_button.h`
- Create: `$SYNTHUI/src/synthui_led_button.cpp`

There is no host harness for LVGL code; the widget is verified by Task 3's example and Task 4's gate. Write it in full, compile it through Task 3's example build.

- [ ] **Step 1: Create `src/synthui_led_button.h`**

```c
/* synthui_led_button.h - SynthUI LedButton, LVGL 9 custom widget.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * The DC "909-led-button" step key: an ivory cap sunk in a black bezel with a
 * lozenge LED across the top third.  Four boolean states and a colour; state
 * change is colour and one 2.5-unit offset, nothing resizes.
 *
 * The widget owns NO toggle logic: it emits stock LV_EVENT_CLICKED and the
 * application sets `lit` (synthui_step / synthui_panel_button's model).
 * `pressed` is a LATCH -- the drawn pressed state is (latch || LV_STATE_PRESSED),
 * so a finger sinks any key while held and the latch keeps a selected key sunk
 * afterwards (the acid_box edit cursor). */
#ifndef SYNTHUI_LED_BUTTON_H
#define SYNTHUI_LED_BUTTON_H

#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>
#include "synthui_led_button_types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_obj_class_t synthui_led_button_class;

lv_obj_t *synthui_led_button_create(lv_obj_t *parent);

void synthui_led_button_set_lit(lv_obj_t *obj, bool lit);
bool synthui_led_button_get_lit(const lv_obj_t *obj);

void synthui_led_button_set_pressed(lv_obj_t *obj, bool pressed);   /* the latch */
bool synthui_led_button_get_pressed(const lv_obj_t *obj);

void synthui_led_button_set_cue(lv_obj_t *obj, bool cue);
bool synthui_led_button_get_cue(const lv_obj_t *obj);

/* Greys the cap and LED, suppresses the halo, clears LV_OBJ_FLAG_CLICKABLE
 * (restored on re-enable). */
void synthui_led_button_set_disabled(lv_obj_t *obj, bool disabled);
bool synthui_led_button_get_disabled(const lv_obj_t *obj);

void synthui_led_button_set_color(lv_obj_t *obj, synthui_led_button_color_t color);
synthui_led_button_color_t synthui_led_button_get_color(const lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
#endif /* SYNTHUI_LED_BUTTON_H */
```

- [ ] **Step 2: Create `src/synthui_led_button.cpp`**

```cpp
/* synthui_led_button.cpp - SynthUI LedButton, LVGL 9 custom widget.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#include "synthui_led_button.h"
#include "synthui_led_button_math.h"
#include <lvgl_private.h>
#include <math.h>

#define MY_CLASS (&synthui_led_button_class)

typedef struct {
    lv_obj_t obj;
    synthui_led_button_color_t color;
    bool lit;
    bool pressed;      /* the latch */
    bool cue;
    bool disabled;
} synthui_led_button_t;

static void led_constructor(const lv_obj_class_t *cls, lv_obj_t *obj);
static void led_destructor(const lv_obj_class_t *cls, lv_obj_t *obj);
static void led_event(const lv_obj_class_t *cls, lv_event_t *e);
static void led_draw(synthui_led_button_t *b, lv_layer_t *layer);

const lv_obj_class_t synthui_led_button_class = {
    .base_class     = &lv_obj_class,
    .constructor_cb = led_constructor,
    .destructor_cb  = led_destructor,
    .event_cb       = led_event,
    /* designators follow lv_obj_class_private.h declaration order -- name
     * declares before width_def (the rotary's note). */
    .name           = "synthui_led_button",
    .width_def      = 96,       /* the DC default size */
    .height_def     = 96,
    .instance_size  = sizeof(synthui_led_button_t),
};

lv_obj_t *synthui_led_button_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_class_create_obj(&synthui_led_button_class, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

static void led_constructor(const lv_obj_class_t *cls, lv_obj_t *obj)
{
    LV_UNUSED(cls);
    synthui_led_button_t *b = (synthui_led_button_t *)obj;
    b->color = SYNTHUI_LED_BUTTON_RED;
    b->lit = b->pressed = b->cue = b->disabled = false;
    /* CLICKABLE is the base default and taps are the whole input story;
     * a scrollable key would swallow taps as drags (synthui_step's reasoning). */
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void led_destructor(const lv_obj_class_t *cls, lv_obj_t *obj)
{
    LV_UNUSED(cls);
    LV_UNUSED(obj);
}

/* The DRAWN pressed state reads LV_STATE_PRESSED directly, not a flag kept
 * from events: lv_obj_add_state(obj, LV_STATE_PRESSED) sends no
 * LV_EVENT_PRESSED, and the spec requires that state to draw exactly like the
 * latch (the gate scene pins it with key 15). */
static bool led_drawn_pressed(const synthui_led_button_t *b)
{
    return b->pressed || lv_obj_has_state((const lv_obj_t *)b, LV_STATE_PRESSED);
}

/* --- pixel helpers.  ALL float->pixel rounding goes through the math
 * header's rect_px / circle_px (the conversion the host sweep tests); this
 * file only offsets the widget-relative result by the object's coords. --- */
static void led_px_to_area(lv_area_t *out, const lv_area_t *c, const synthui_led_button_px_t *px)
{
    out->x1 = c->x1 + px->x1;
    out->y1 = c->y1 + px->y1;
    out->x2 = c->x1 + px->x2;
    out->y2 = c->y1 + px->y2;
}

static void led_area(lv_area_t *out, const lv_area_t *c, const synthui_led_button_rect_t *r, int32_t dy_px)
{
    const synthui_led_button_px_t px = synthui_led_button_rect_px(r, dy_px);
    led_px_to_area(out, c, &px);
}

static void led_circle_area(lv_area_t *out, const lv_area_t *c, const synthui_led_button_circle_t *k, int32_t dy_px)
{
    const synthui_led_button_px_t px = synthui_led_button_circle_px(k, dy_px);
    led_px_to_area(out, c, &px);
}

static int32_t led_radius(float v)   /* radii: at least 1 px */
{
    const int32_t p = (int32_t)lroundf(v);
    return p < 1 ? 1 : p;
}

static void led_invalidate_px(lv_obj_t *obj, const synthui_led_button_px_t *px)
{
    lv_area_t c, a;
    lv_obj_get_coords(obj, &c);
    led_px_to_area(&a, &c, px);
    lv_obj_invalidate_area(obj, &a);
}

static void led_invalidate_lit_box(lv_obj_t *obj)
{
    synthui_led_button_t *b = (synthui_led_button_t *)obj;
    lv_area_t c;
    lv_obj_get_coords(obj, &c);
    synthui_led_button_px_t px;
    synthui_led_button_lit_box((float)lv_area_get_width(&c), (float)lv_area_get_height(&c),
                               led_drawn_pressed(b), &px);
    led_invalidate_px(obj, &px);
}

static void led_invalidate_press_box(lv_obj_t *obj)
{
    lv_area_t c;
    lv_obj_get_coords(obj, &c);
    synthui_led_button_px_t px;
    synthui_led_button_press_box((float)lv_area_get_width(&c), (float)lv_area_get_height(&c), &px);
    led_invalidate_px(obj, &px);
}

/* A finger went down or came up.  The press box does not depend on the press
 * state, so the invalidation is correct whether or not the indev has already
 * flipped LV_STATE_PRESSED when this event arrives; the draw that follows reads
 * the settled state.  A latched key does not move, so nothing is invalidated. */
static void led_on_press_edge(lv_obj_t *obj)
{
    const synthui_led_button_t *b = (const synthui_led_button_t *)obj;
    if (b->pressed) return;
    led_invalidate_press_box(obj);
}

static void led_event(const lv_obj_class_t *cls, lv_event_t *e)
{
    LV_UNUSED(cls);
    if (lv_obj_event_base(MY_CLASS, e) != LV_RESULT_OK) return;
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_DRAW_MAIN:
        led_draw((synthui_led_button_t *)obj, lv_event_get_layer(e));
        break;
    /* Transient press: the key sinks while a finger is down. */
    case LV_EVENT_PRESSED:
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        led_on_press_edge(obj);
        break;
    default:
        break;
    }
}

static void led_fill(lv_layer_t *layer, const lv_area_t *a, uint32_t hex, lv_opa_t opa, int32_t radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(hex);
    d.bg_opa = opa;
    d.radius = radius;
    lv_draw_rect(layer, &d, a);
}

static void led_grad(lv_layer_t *layer, const lv_area_t *a, uint32_t top, uint32_t bottom, int32_t radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.bg_grad.dir = LV_GRAD_DIR_VER;
    d.bg_grad.stops_count = 2;
    d.bg_grad.stops[0].color = lv_color_hex(top);
    d.bg_grad.stops[0].opa = LV_OPA_COVER;
    d.bg_grad.stops[0].frac = 0;
    d.bg_grad.stops[1].color = lv_color_hex(bottom);
    d.bg_grad.stops[1].opa = LV_OPA_COVER;
    d.bg_grad.stops[1].frac = 255;
    d.radius = radius;
    lv_draw_rect(layer, &d, a);
}

static void led_draw(synthui_led_button_t *b, lv_layer_t *layer)
{
    lv_area_t c;
    lv_obj_get_coords((lv_obj_t *)b, &c);
    const int32_t w = lv_area_get_width(&c);
    const int32_t h = lv_area_get_height(&c);
    if (w <= 0 || h <= 0) return;

    const bool pressed = led_drawn_pressed(b);
    synthui_led_button_layout_t L;
    if (!synthui_led_button_compute_layout((float)w, (float)h, pressed, &L)) return;
    synthui_led_button_palette_t P;
    synthui_led_button_palette(b->color, b->lit, pressed, b->cue, b->disabled, &P);

    lv_area_t a;
    const int32_t dy = L.dy_px;   /* whole pixels, added AFTER rounding: a press never resizes a layer */

    /* 1. bezel (never moves): fill + border, the SVG stroke drawn inside the extent */
    led_area(&a, &c, &L.bezel, 0);
    {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(SYNTHUI_LED_BUTTON_BEZEL);
        d.bg_opa = LV_OPA_COVER;
        d.radius = led_radius(L.bezel_r);
        d.border_color = lv_color_hex(P.bezel_color);
        d.border_width = b->cue ? L.cue_bw_px : L.bezel_bw_px;
        d.border_opa = LV_OPA_COVER;
        d.border_side = LV_BORDER_SIDE_FULL;
        lv_draw_rect(layer, &d, &a);
    }

    /* 2. well (never moves) */
    led_area(&a, &c, &L.well, 0);
    led_fill(layer, &a, SYNTHUI_LED_BUTTON_WELL, SYNTHUI_LED_BUTTON_WELL_OPA, led_radius(L.well_r));

    /* 3. cap: solid mid under two 2-stop halves (LV_GRADIENT_MAX_STOPS is 2).
     * Each half's inner corners are rounded too, but they meet the solid mid
     * at exactly the mid colour, so the rounding is invisible. */
    led_area(&a, &c, &L.cap, dy);
    led_fill(layer, &a, P.cap_mid, LV_OPA_COVER, led_radius(L.cap_r));
    led_area(&a, &c, &L.cap_top, dy);
    led_grad(layer, &a, P.cap_top, P.cap_mid, led_radius(L.cap_r));
    led_area(&a, &c, &L.cap_low, dy);
    led_grad(layer, &a, P.cap_mid, P.cap_low, led_radius(L.cap_r));

    /* 4. highlight */
    led_area(&a, &c, &L.highlight, dy);
    led_fill(layer, &a, 0xFFFFFFu, P.highlight_opa, led_radius(L.highlight_r));

    /* 5. halo: a 10-unit border on the LED grown by 5; the LED fill covers
     * the inner half, which is what SVG's stroke-over-fill produces */
    if (P.halo_on) {
        led_area(&a, &c, &L.halo, dy);
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_opa = LV_OPA_TRANSP;
        d.radius = led_radius(L.halo_r);
        d.border_color = lv_color_hex(P.halo_color);
        d.border_width = L.halo_bw_px;
        d.border_opa = SYNTHUI_LED_BUTTON_HALO_OPA;
        d.border_side = LV_BORDER_SIDE_FULL;
        lv_draw_rect(layer, &d, &a);
    }

    /* 6. LED */
    led_area(&a, &c, &L.led, dy);
    led_fill(layer, &a, P.led_fill, LV_OPA_COVER, led_radius(L.led_r));

    /* 7. moulding dots (dropped below 34 px) */
    if (L.dots_visible) {
        led_circle_area(&a, &c, &L.dot1, dy);
        led_fill(layer, &a, 0x000000u, SYNTHUI_LED_BUTTON_DOT_OPA, LV_RADIUS_CIRCLE);
        led_circle_area(&a, &c, &L.dot2, dy);
        led_fill(layer, &a, 0x000000u, SYNTHUI_LED_BUTTON_DOT_OPA, LV_RADIUS_CIRCLE);
    }

    /* 8. base */
    led_area(&a, &c, &L.base, dy);
    led_fill(layer, &a, SYNTHUI_LED_BUTTON_BASE, P.base_opa, led_radius(L.base_r));
}

/* --- setters: early-return on no change, invalidate only the box painted --- */

void synthui_led_button_set_lit(lv_obj_t *obj, bool lit)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    synthui_led_button_t *b = (synthui_led_button_t *)obj;
    if (b->lit == lit) return;
    b->lit = lit;
    led_invalidate_lit_box(obj);
}

bool synthui_led_button_get_lit(const lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    return ((const synthui_led_button_t *)obj)->lit;
}

void synthui_led_button_set_pressed(lv_obj_t *obj, bool pressed)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    synthui_led_button_t *b = (synthui_led_button_t *)obj;
    if (b->pressed == pressed) return;
    b->pressed = pressed;
    if (lv_obj_has_state(obj, LV_STATE_PRESSED)) return;   /* finger still down: nothing moves */
    led_invalidate_press_box(obj);
}

bool synthui_led_button_get_pressed(const lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    return ((const synthui_led_button_t *)obj)->pressed;
}

void synthui_led_button_set_cue(lv_obj_t *obj, bool cue)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    synthui_led_button_t *b = (synthui_led_button_t *)obj;
    if (b->cue == cue) return;
    b->cue = cue;
    lv_obj_invalidate(obj);   /* the bezel ring is the outer edge on four sides */
}

bool synthui_led_button_get_cue(const lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    return ((const synthui_led_button_t *)obj)->cue;
}

void synthui_led_button_set_disabled(lv_obj_t *obj, bool disabled)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    synthui_led_button_t *b = (synthui_led_button_t *)obj;
    if (b->disabled == disabled) return;
    b->disabled = disabled;
    if (disabled) lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    else          lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_invalidate(obj);
}

bool synthui_led_button_get_disabled(const lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    return ((const synthui_led_button_t *)obj)->disabled;
}

void synthui_led_button_set_color(lv_obj_t *obj, synthui_led_button_color_t color)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    synthui_led_button_t *b = (synthui_led_button_t *)obj;
    if (b->color == color) return;
    b->color = color;
    led_invalidate_lit_box(obj);
}

synthui_led_button_color_t synthui_led_button_get_color(const lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    return ((const synthui_led_button_t *)obj)->color;
}
```

- [ ] **Step 3: Host-compile the TU for syntax only** (no LVGL on the host, so a quick include check is all that is available here)

Run: `cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/display/synthui_panel_button_test && cmake --build build 2>&1 | grep -E "synthui_led_button|warning|error"`
Expected: a compile line for `synthui_led_button.cpp.obj` and no warnings or errors (SynthUI globs `src/*.cpp`, so any SynthUI-linking example compiles the new file with the real ARM toolchain and LVGL). Do not run that example's gate. (A host `cc -fsyntax-only` of the math header alone fails on unused `static inline` functions under `-Werror`; it is not a useful check.)

- [ ] **Step 4: Commit (SynthUI)**

```bash
cd $SYNTHUI
git add src/synthui_led_button.h src/synthui_led_button.cpp
git commit -m "synthui_led_button: LVGL 9 custom widget -- eight-layer sw draw, pressed latch OR'd with LV press, per-box delta invalidation (NEW-25)

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 3: The consumer example `examples/display/synthui_led_button_test` (rt1176-evkb)

**Files:**
- Create: `$EVKB/examples/display/synthui_led_button_test/CMakeLists.txt`
- Create: `$EVKB/examples/display/synthui_led_button_test/synthui_led_button_test.cpp`

- [ ] **Step 1: Create `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(synthui_led_button_test)

add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
import_evkb_synthui()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(synthui_led_button_test
    synthui_led_button_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp
)
teensy_target_link_libraries(synthui_led_button_test cores MipiDisplay PXP)

target_link_libraries(synthui_led_button_test.elf SynthUI LVGL stdc++)

target_include_directories(synthui_led_button_test.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(LED_BUTTON_EYEBALL_HOLD)
    add_compile_definitions(LED_BUTTON_EYEBALL_HOLD=${LED_BUTTON_EYEBALL_HOLD})
endif()
```

- [ ] **Step 2: Create `synthui_led_button_test.cpp`**

```cpp
/* synthui_led_button_test - synthui_led_button (NEW-25) on the RK055
 * (720x1280 XRGB8888, db pipeline), checksummed.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Scene: a 4x4 bank of 16 keys, row-major index i = row*4 + col:
 *    0 off red 100        1 lit red 100         2 lit amber 100       3 lit green 100
 *    4 lit blue 100       5 latched off red 100 6 lit+latched red 100 7 cue off red 100
 *    8 cue+lit red 100    9 disabled off 100   10 disabled+lit 100   11 32 px lit (no dots)
 *   12 34 px lit (dots)  13 150 px lit         14 120x80 lit (centred) 15 100 lit + LV_STATE_PRESSED
 *
 * Phase A (gated): led_button_crc golden -> 64-step LCG delta sequence over
 * lit/pressed/cue/color on keys 0..12 -> led_button_delta_crc vs
 * led_button_fresh_crc, led_button_damage engagement, led_button_vsync,
 * crc_done, PASS token.  Keys 13..15 are excluded from the LCG so the
 * engagement bound (10000 px) is exactly the largest legitimate box of a
 * 100 px key (a cue change repaints the whole key); a 150 px key's cue
 * would be 22500 and say nothing about engagement.
 * Phase B (after crc_done, ungated): a playhead cue sweep + LED chaser
 * across all 16 keys, measuring frame time (led_button_fps). */
#include <Arduino.h>
#include <string.h>
#include <math.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_led_button.h"

#ifdef LED_BUTTON_EYEBALL_HOLD
static void eyeball_hold(int n)
{
    if (n != LED_BUTTON_EYEBALL_HOLD) return;
    Serial1.printf("LED_BUTTON_EYEBALL_HOLD=%d\n", n);
    for (;;) { }
}
#else
#define eyeball_hold(n) ((void)0)
#endif

struct KeyConfig {
    int32_t w, h;
    bool lit, pressed, cue, disabled;
    bool lv_state;            /* also lv_obj_add_state(LV_STATE_PRESSED) -- draws as pressed */
    synthui_led_button_color_t color;
};

static const KeyConfig kKeys[16] = {
    { 100, 100, false, false, false, false, false, SYNTHUI_LED_BUTTON_RED   },   /*  0 */
    { 100, 100, true,  false, false, false, false, SYNTHUI_LED_BUTTON_RED   },   /*  1 */
    { 100, 100, true,  false, false, false, false, SYNTHUI_LED_BUTTON_AMBER },   /*  2 */
    { 100, 100, true,  false, false, false, false, SYNTHUI_LED_BUTTON_GREEN },   /*  3 */
    { 100, 100, true,  false, false, false, false, SYNTHUI_LED_BUTTON_BLUE  },   /*  4 */
    { 100, 100, false, true,  false, false, false, SYNTHUI_LED_BUTTON_RED   },   /*  5 latched */
    { 100, 100, true,  true,  false, false, false, SYNTHUI_LED_BUTTON_RED   },   /*  6 lit + latched */
    { 100, 100, false, false, true,  false, false, SYNTHUI_LED_BUTTON_RED   },   /*  7 cue */
    { 100, 100, true,  false, true,  false, false, SYNTHUI_LED_BUTTON_RED   },   /*  8 cue + lit */
    { 100, 100, false, false, false, true,  false, SYNTHUI_LED_BUTTON_RED   },   /*  9 disabled */
    { 100, 100, true,  false, false, true,  false, SYNTHUI_LED_BUTTON_RED   },   /* 10 disabled + lit: no halo */
    {  32,  32, true,  false, false, false, false, SYNTHUI_LED_BUTTON_RED   },   /* 11 no dots */
    {  34,  34, true,  false, false, false, false, SYNTHUI_LED_BUTTON_RED   },   /* 12 dots */
    { 150, 150, true,  false, false, false, false, SYNTHUI_LED_BUTTON_RED   },   /* 13 large */
    { 120,  80, true,  false, false, false, false, SYNTHUI_LED_BUTTON_RED   },   /* 14 non-square */
    { 100, 100, true,  false, false, false, true,  SYNTHUI_LED_BUTTON_RED   },   /* 15 LV_STATE_PRESSED */
};

#define LCG_KEYS 13   /* keys 0..12 take part in the delta sequence */

static lv_obj_t *g_key[16];
static bool g_lit[16], g_pressed[16], g_cue[16];
static synthui_led_button_color_t g_color[16];

static void opaque_bg(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
}

/* Build the bank from the mutable state arrays. Both the golden and the fresh
 * reference pass use this, so the object hierarchy is identical. */
static lv_obj_t *build_bank(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    opaque_bg(scr);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SynthUI LedButton");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    for (int i = 0; i < 16; i++) {
        const int row = i / 4, col = i % 4;
        const KeyConfig *k = &kKeys[i];
        lv_obj_t *b = synthui_led_button_create(scr);
        lv_obj_set_size(b, k->w, k->h);
        const int32_t cx = 90 + col * 180;
        const int32_t cy = 200 + row * 180;
        lv_obj_set_pos(b, cx - k->w / 2, cy - k->h / 2);
        synthui_led_button_set_color(b, g_color[i]);
        synthui_led_button_set_lit(b, g_lit[i]);
        synthui_led_button_set_pressed(b, g_pressed[i]);
        synthui_led_button_set_cue(b, g_cue[i]);
        synthui_led_button_set_disabled(b, k->disabled);
        if (k->lv_state) lv_obj_add_state(b, LV_STATE_PRESSED);
        g_key[i] = b;
    }
    return scr;
}

/* Checksum the PRESENTED buffer: flip_sync() first, then scanned_fb(). */
static uint32_t sum_active_screen(void)
{
    lvgl_mipi_panel_flip_sync();
    lvgl_sum_reset();
    lvgl_sum_feed(lvgl_mipi_panel_scanned_fb(), PANEL_FB_BYTES);
    return lvgl_sum_value();
}

static uint32_t sum_screen(lv_obj_t *scr)
{
    lv_screen_load(scr);
    lv_obj_invalidate(scr);
    lv_refr_now(NULL);
    return sum_active_screen();
}

/* --- delta guards: per-invalidate area recorder --- */
static int32_t s_delta_maxarea = 0;
static long    s_delta_total = 0;
static bool    s_delta_record = false;

static void delta_inv_cb(lv_event_t *e)
{
    if (!s_delta_record) return;
    const lv_area_t *a = (const lv_area_t *)lv_event_get_param(e);
    const int32_t px = lv_area_get_width(a) * lv_area_get_height(a);
    s_delta_total += px;
    if (px > s_delta_maxarea) s_delta_maxarea = px;
}

#define LED_BUTTON_DELTA_STEPS 64
static uint32_t s_lcg;
static uint32_t lcg_next(void)
{
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return s_lcg;
}

/* Runs ON the already-rendered golden bank; evolves the state arrays in place. */
static uint32_t delta_run_sequence(void)
{
    lv_display_add_event_cb(lv_display_get_default(), delta_inv_cb,
                            LV_EVENT_INVALIDATE_AREA, NULL);
    s_lcg = 0x5EEDF00Du;
    s_delta_maxarea = 0;
    s_delta_total = 0;
    s_delta_record = true;

    for (int step = 0; step < LED_BUTTON_DELTA_STEPS; step++) {
        const uint32_t r = lcg_next();
        const int idx = (int)((r >> 16) % LCG_KEYS);
        switch ((r >> 24) & 3) {
        case 0:
            g_lit[idx] = !g_lit[idx];
            synthui_led_button_set_lit(g_key[idx], g_lit[idx]);
            break;
        case 1:
            g_pressed[idx] = !g_pressed[idx];
            synthui_led_button_set_pressed(g_key[idx], g_pressed[idx]);
            break;
        case 2:
            g_cue[idx] = !g_cue[idx];
            synthui_led_button_set_cue(g_key[idx], g_cue[idx]);
            break;
        default:
            g_color[idx] = (synthui_led_button_color_t)(((int)g_color[idx] + 1) & 3);
            synthui_led_button_set_color(g_key[idx], g_color[idx]);
            break;
        }
        lv_refr_now(NULL);
    }
    s_delta_record = false;
    return sum_active_screen();
}

/* --- Phase B: fps measurement and a continuous cue sweep + LED chaser --- */
#define LED_BUTTON_FPS_MAX 512
static uint32_t g_fps_us[LED_BUTTON_FPS_MAX];
static volatile uint32_t g_fps_n = 0, g_fps_frames = 0;
static volatile bool g_fps_timing = false, g_fps_skip = false;
static volatile bool g_fps_rendered = false;
static volatile uint32_t g_fps_t0 = 0;
static uint32_t g_anim_step = 0;

static void fps_refr_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:   g_fps_t0 = micros(); break;
    case LV_EVENT_RENDER_READY: g_fps_rendered = true; break;
    case LV_EVENT_REFR_READY:
        if (g_fps_rendered && g_fps_timing) {
            if (g_fps_skip) g_fps_skip = false;
            else {
                g_fps_frames++;
                if (g_fps_n < LED_BUTTON_FPS_MAX)
                    g_fps_us[g_fps_n++] = micros() - g_fps_t0;
            }
        }
        g_fps_rendered = false;
        break;
    default: break;
    }
}

static void key_anim_cb(lv_timer_t *t)
{
    (void)t;
    g_anim_step++;
    const uint32_t head = g_anim_step % 16;
    for (int i = 0; i < 16; i++) {
        synthui_led_button_set_cue(g_key[i], i == (int)head);
        synthui_led_button_set_lit(g_key[i], ((g_anim_step / 4) + i) % 3 == 0);
    }
}

static void key_fps_phase(const char *tag, uint32_t target_frames)
{
    static bool cbs_added = false;
    if (!cbs_added) {
        lv_display_t *disp = lv_display_get_default();
        lv_display_add_event_cb(disp, fps_refr_cb, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(disp, fps_refr_cb, LV_EVENT_RENDER_READY, NULL);
        lv_display_add_event_cb(disp, fps_refr_cb, LV_EVENT_REFR_READY, NULL);
        cbs_added = true;
    }
    g_fps_n = 0;
    g_fps_frames = 0;
    g_fps_skip = true;
    g_fps_timing = true;

    lv_timer_t *anim = lv_timer_create(key_anim_cb, 15, NULL);
    const uint32_t t0 = millis();
    while (g_fps_n < target_frames && (millis() - t0) < 10000u) {
        lvgl_rt1176_loop();
    }
    g_fps_timing = false;
    lv_timer_delete(anim);

    uint32_t s[LED_BUTTON_FPS_MAX];
    const uint32_t n = g_fps_n ? g_fps_n : 1;
    memcpy(s, (const void *)g_fps_us, n * sizeof(uint32_t));
    for (uint32_t i = 1; i < n; i++) {
        const uint32_t v = s[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
        s[j + 1] = v;
    }
    Serial1.printf("%s frames=%lu mfps_med=%lu us_med=%lu us_min=%lu us_max=%lu\n",
                   tag,
                   (unsigned long)g_fps_n,
                   (unsigned long)(1000000000ull / (s[n / 2] ? s[n / 2] : 1)),
                   (unsigned long)s[n / 2],
                   (unsigned long)s[0],
                   (unsigned long)s[n - 1]);
}

void setup()
{
    Serial1.begin(115200);
    while (!Serial1 && millis() < 2000) {}
    Serial1.println("=== BOOT ===");
    Serial1.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n", "synthui_led_button_test", 1, __DATE__, __TIME__);
    Serial1.println("SYNTHUI_LED_BUTTON_BEGIN");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) {
        Serial1.println("crc_done");
        return;
    }
    Display.fillScreen(0x0000);

    lvgl_rt1176_begin();
    lvgl_mipi_panel_create_db(Display);

    Serial1.println("led_button_scene=16 grid=4x4");

    /* Phase A1: full initial render -> golden led_button_crc */
    for (int i = 0; i < 16; i++) {
        g_lit[i] = kKeys[i].lit;
        g_pressed[i] = kKeys[i].pressed;
        g_cue[i] = kKeys[i].cue;
        g_color[i] = kKeys[i].color;
    }
    lv_screen_load(build_bank());
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000) {
        lvgl_rt1176_loop();
    }
    Serial1.printf("LVGL_FLUSHED=%s\n",
                   lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    Serial1.printf("LVGL_BYTES=%lu\n",
                   (unsigned long)(lvgl_mipi_panel_flushed_px()
                                   * PANEL_BYTES_PER_PIXEL));
    const uint32_t led_button_crc = sum_active_screen();
    Serial1.printf("led_button_crc=0x%08lX\n", (unsigned long)led_button_crc);
    eyeball_hold(1);

    /* Phase A2: 64-step deterministic delta sequence vs a fresh full render */
    const uint32_t d_seq = delta_run_sequence();
    eyeball_hold(2);
    const uint32_t d_full = sum_screen(build_bank());
    eyeball_hold(3);

    Serial1.printf("led_button_delta_crc=0x%08lX\n", (unsigned long)d_seq);
    Serial1.printf("led_button_fresh_crc=0x%08lX\n", (unsigned long)d_full);
    Serial1.printf("led_button_delta_eq=%s\n", (d_seq == d_full) ? "PASS" : "FAIL");
    Serial1.printf("led_button_damage max=%ld total=%ld steps=%d\n",
                   (long)s_delta_maxarea, s_delta_total, LED_BUTTON_DELTA_STEPS);
    Serial1.printf("led_button_vsync flips=%lu isrs=%lu timeouts=%lu\n",
                   (unsigned long)lvgl_mipi_panel_flips(),
                   (unsigned long)lvgl_mipi_panel_vsync_isrs(),
                   (unsigned long)lvgl_mipi_panel_vsync_timeouts());
    Serial1.println("crc_done");
    Serial1.println("PASS: SynthUI led_button render verified");

    /* Phase B: cue sweep + LED chaser, fps measured (silicon is the answer) */
    key_fps_phase("led_button_fps", 64);

    {
        lv_mem_monitor_t mm;
        lv_mem_monitor(&mm);
        Serial1.printf("led_button_mem total=%lu used_pct=%u max_used=%lu frag_pct=%u\n",
                       (unsigned long)mm.total_size, (unsigned)mm.used_pct,
                       (unsigned long)mm.max_used, (unsigned)mm.frag_pct);
    }

    /* keep animating for an eyes/camera pass */
    lv_timer_create(key_anim_cb, 15, NULL);
}

void loop()
{
    lvgl_rt1176_loop();
}
```

- [ ] **Step 3: Configure and build**

Run:
```bash
cd $EVKB/examples/display/synthui_led_button_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```
Expected: `build/synthui_led_button_test.elf` (+ `.hex`) with no warnings from `synthui_led_button.cpp` (the SynthUI target compiles the new TU via the `src/*.cpp` glob — a fresh configure is required for the glob to pick it up; `CONFIGURE_DEPENDS` handles reconfigures).

- [ ] **Step 4: Boot in QEMU by hand and read the tokens**

Run: `cd $EVKB && ./tools/rt1170-qemu.sh examples/display/synthui_led_button_test/build/synthui_led_button_test.elf 2>&1 | head -40` (or the equivalent `tools/qrun` invocation Task 4's script uses, with `-serial stdio`).
Expected, in order: `PANEL_OK`, `led_button_scene=16 grid=4x4`, `LVGL_FLUSHED=PASS`, `LVGL_BYTES=3686400`, `led_button_crc=0x........`, `led_button_delta_crc=`, `led_button_fresh_crc=` EQUAL to it, `led_button_delta_eq=PASS`, `led_button_damage max=10000 total=... steps=64` (max ≤ 10000), `led_button_vsync ... timeouts=0`, `crc_done`, `PASS: SynthUI led_button render verified`.

If `delta_eq=FAIL`: a setter's box is too small. Diagnose by narrowing the LCG to one op at a time (`case 0` only, then `case 1`, …) — the op whose sequence differs names the box.

- [ ] **Step 5: Commit (evkb)**

```bash
cd $EVKB
git add examples/display/synthui_led_button_test/CMakeLists.txt examples/display/synthui_led_button_test/synthui_led_button_test.cpp
git commit -m "synthui_led_button_test: 16-key bank on the db pipeline -- golden, LCG delta sequence over lit/pressed/cue/color, damage engagement, vsync witness, fps phase (NEW-25)

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 4: The QEMU gate, golden, RED demonstrations, fixture (rt1176-evkb)

**Files:**
- Create: `$EVKB/examples/display/synthui_led_button_test/run_qemu.sh` (mode 755)
- Create: `$EVKB/examples/display/synthui_led_button_test/transcript_qemu.txt`

- [ ] **Step 1: Create `run_qemu.sh`** with a placeholder golden `0x00000000` that Step 2 replaces

```sh
#!/bin/sh
# synthui_led_button_test -- the SynthUI LedButton (NEW-25) rendered on the
# RK055 through the db pipeline, checksummed.  Spec:
# docs/superpowers/specs/2026-09-15-synthui-led-button-design.md section 8.
#
# Demonstrated RED (dates filled in when done):
#   - golden altered to 0xDEADBEEF          -> "FAIL: led_button checksum"
#   - set_lit reverted to lv_obj_invalidate -> "FAIL: delta damage not engaged"
#   - lit box shrunk to the bare LED        -> "FAIL: delta equality"
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/synthui_led_button_test.elf"
OUT=$(gate_capture_path "$DIR" synthui_led_button.uart)
DBG=$(gate_capture_path "$DIR" synthui_led_button.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# 20s: bring-up margin plus headroom for the 64 delta steps and 64 fps loop frames.
sleep 20; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
# Panel chain first: a framebuffer no display owns would still checksum.
grep -q "PANEL_OK"          "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "led_button_scene=16 grid=4x4" "$OUT" || { echo "FAIL: scene line missing"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
# Flushed AREA of the first refresh -- 720*1280*4 at XRGB8888, anchored.
grep -qE "LVGL_BYTES=3686400\r?$" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }
# GOLDEN CHECKSUM -- FNV-1a over the whole 720x1280 PRESENTED buffer, the
# 16-key bank (off; lit red/amber/green/blue; latched; lit+latched; cue;
# cue+lit; disabled; disabled+lit; 32 px; 34 px; 150 px; 120x80; LV pressed).
# Recorded across two consecutive bit-identical QEMU runs (2026-09-.., NEW-25,
# vendored LVGL 9.4.0, XRGB8888).  On a mismatch work out WHICH of {SynthUI
# pin, LVGL pin, lv_conf.h, fonts, scene} changed; do NOT paste in whatever
# the board printed.
grep -qE "led_button_crc=0x00000000\r?$" "$OUT" || { echo "FAIL: led_button checksum"; exit 1; }
# DELTA EQUALITY: the 64-step LCG sequence (lit / pressed latch / cue / colour
# on keys 0..12) rendered through the widget's delta damage must be
# PIXEL-IDENTICAL to a fresh full render of the final state.
DSEQ=$(grep -a -oE "led_button_delta_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
DFUL=$(grep -a -oE "led_button_fresh_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DSEQ" ] && [ -n "$DFUL" ] || { echo "FAIL: delta guard tokens missing"; exit 1; }
[ "$DSEQ" = "$DFUL" ] || { echo "FAIL: delta render differs from full render ($DSEQ vs $DFUL)"; exit 1; }
grep -qE "led_button_delta_eq=PASS\r?$" "$OUT" || { echo "FAIL: delta equality"; exit 1; }
# ENGAGEMENT: the largest single invalidated area during the 64-step segment
# must stay key-sized.  Bound 10000 px = a whole 100 px key, which a cue
# change legitimately repaints; a latch change is 80x83 = 6640, a lit change 58x25 = 1450.
# Keys 13..15 (150 px, 120x80, LV-pressed) are outside the LCG on purpose.
# A change that quietly reverts a setter to full-screen invalidation fails
# HERE and nowhere else (a full 720x1280 screen is 921600 px).
DAREA=$(grep -a -oE "led_button_damage max=[0-9]+" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DAREA" ] && [ "$DAREA" -gt 0 ] || { echo "FAIL: delta damage guard missing or zero"; exit 1; }
[ "$DAREA" -le 10000 ] || { echo "FAIL: delta damage not engaged (max=$DAREA)"; exit 1; }
# vsync-fence health (db pipeline): a timeout means the tear-free property
# silently degraded with every golden still green.
grep -qE "led_button_vsync flips=[0-9]+ isrs=[0-9]+ timeouts=0\r?$" "$OUT" || { echo "FAIL: vsync fence unhealthy or missing"; exit 1; }
grep -q "crc_done" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
grep -q "PASS: SynthUI led_button render verified" "$OUT" || { echo "FAIL: render verification token missing"; exit 1; }
echo "PASS: SynthUI led_button render verified"
```

Run: `chmod +x $EVKB/examples/display/synthui_led_button_test/run_qemu.sh`

- [ ] **Step 2: Record the golden from two consecutive runs**

Run twice:
```bash
cd $EVKB/examples/display/synthui_led_button_test && ./run_qemu.sh; grep -a "led_button_crc=" build/synthui_led_button.uart
```
Expected: the script FAILS on `FAIL: led_button checksum` both times (placeholder golden) and both captures print the SAME `led_button_crc=0x........`. If they differ, the render is nondeterministic — stop and diagnose (the candidates are an uninitialised palette field or the LCG touching a key outside 0..12); do NOT pick one.

Then replace `0x00000000` in `run_qemu.sh` with the recorded value and fill in the date in the header comment.

- [ ] **Step 3: Verify GREEN**

Run: `./run_qemu.sh`
Expected: last line `PASS: SynthUI led_button render verified`, exit 0.

- [ ] **Step 4: Demonstrate the three RED arms by name, then restore**

1. In `run_qemu.sh` change the golden to `0xDEADBEEF` → `./run_qemu.sh` → expected `FAIL: led_button checksum`. Restore.
2. In `$SYNTHUI/src/synthui_led_button.cpp`, in `synthui_led_button_set_lit` replace `led_invalidate_lit_box(obj);` with `lv_obj_invalidate(lv_obj_get_screen(obj));` → rebuild (`cmake --build build`) → `./run_qemu.sh` → expected `FAIL: delta damage not engaged (max=921600)`. Restore.
3. In `synthui_led_button_math.h`, in `synthui_led_button_lit_box` replace `rect_px(&L.halo, L.dy_px)` with `rect_px(&L.led, L.dy_px)` → rebuild → `./run_qemu.sh` → expected `FAIL: delta render differs from full render (...)` (stale halo pixels when a lit LED goes off). Restore, rebuild, `./run_qemu.sh` GREEN again.

Record the dates against the three arms in the script header.

- [ ] **Step 5: Capture the fixture**

Run: `cp build/synthui_led_button.uart transcript_qemu.txt` (after a GREEN run). Prepend nothing; the vacuity suite replays it verbatim.

- [ ] **Step 6: Commit (evkb)**

```bash
cd $EVKB
git add examples/display/synthui_led_button_test/run_qemu.sh examples/display/synthui_led_button_test/transcript_qemu.txt
git commit -m "synthui_led_button_test: QEMU gate -- golden recorded over two runs, delta equality, engagement bound 10000, vsync witness; RED three ways by name (NEW-25)

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 5: Vacuity case, GATES manifest, README, CLAUDE.md count (rt1176-evkb)

**Files:**
- Modify: `$EVKB/tools/gate-vacuity.test.sh` (append section 15 after section 14, `pxp_rotate_probe`)
- Modify: `$EVKB/tools/license-audit.sh` (GATES list, after the `synthui_lamp_test` line)
- Modify: `$EVKB/examples/README.md` (display row)
- Modify: `$EVKB/CLAUDE.md` (gate count line 109 and the target line 590)

- [ ] **Step 1: Add vacuity section 15**

Append after section 14's `fi`:

```sh
# --- 15. synthui_led_button_test: green fixture passes; bad golden and a
# missing damage counter fail by name (NEW-25).  Same shape as the fader's
# section 8: an absent counter must never read as the good outcome.
LBT="examples/display/synthui_led_button_test"
if [ -d "$EVKB/$LBT" ] && [ -f "$EVKB/$LBT/transcript_qemu.txt" ]; then
    run_gate "$LBT" "run_qemu.sh" "$EVKB/$LBT/transcript_qemu.txt"; rc=$?
    [ "$rc" -eq 0 ] && result=0 || result=1
    report "green_still_passes_synthui_led_button_test" $result

    sed 's|^led_button_crc=0x........|led_button_crc=0xBADBADBA|' \
        "$EVKB/$LBT/transcript_qemu.txt" > "$WORK/lb_badcrc.txt"
    run_gate "$LBT" "run_qemu.sh" "$WORK/lb_badcrc.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "FAIL: led_button checksum" || result=1
    report "led_button_bad_golden_fails_by_name" $result

    grep -v "^led_button_damage " "$EVKB/$LBT/transcript_qemu.txt" > "$WORK/lb_nodmg.txt"
    run_gate "$LBT" "run_qemu.sh" "$WORK/lb_nodmg.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "delta damage guard missing" || result=1
    report "led_button_missing_damage_counter_fails" $result
else
    echo "SKIP: synthui_led_button_test vacuity (example or fixture missing)"
fi
```

- [ ] **Step 2: Run the vacuity suite**

Run: `cd $EVKB && ./tools/gate-vacuity.test.sh 2>&1 | tail -8`
Expected: three new `PASS:` lines (`green_still_passes_synthui_led_button_test`, `led_button_bad_golden_fails_by_name`, `led_button_missing_damage_counter_fails`) and a total of **58** (55 + 3). Re-derive the count from `grep -c "^PASS:"` on the run, never from memory.

- [ ] **Step 3: Add the GATES entry**

In `tools/license-audit.sh`, after `examples/display/synthui_lamp_test:synthui_lamp_test \`, add:
```
examples/display/synthui_led_button_test:synthui_led_button_test \
```

Run: `cd $EVKB && ./tools/license-audit.sh 2>&1 | tail -3`
Expected: `LICENSE-AUDIT: PASS`, with `examples/display/synthui_led_button_test` walked (its dep-path count printed). Run it BEFORE or AFTER a sweep, never during.

- [ ] **Step 4: README row**

In `examples/README.md`, in the `**display**` row, after the `synthui_step_test (...)` entry add:

```
`synthui_led_button_test` (the DC 909 step key as `synthui_led_button`, NEW-25: a 4x4 bank covering off, lit in four LED colours, the pressed LATCH, cue, disabled, the 34 px dot dropout and a non-square key; one render golden, a 64-step LCG delta sequence over lit/pressed/cue/colour that must equal a fresh full render, an engagement bound of exactly one 100 px key, and the vsync witness),
```

- [ ] **Step 5: CLAUDE.md count**

Line 109: change `The sweep covers **140 gates** — the acid_box LANDSCAPE work added ONE` to `The sweep covers **141 gates** — NEW-25 added ONE on <date> (`display/synthui_led_button_test`: the DC 909 step key with a pressed latch; 140 before it), and before that the acid_box LANDSCAPE work added ONE`. Line 590: `**140 passed, 0 failed, 0 SKIP**` → `**141 passed, 0 failed, 0 SKIP**`, and `**139 passed, 1 failed, 0 SKIP**` → `**140 passed, 1 failed, 0 SKIP**`.

- [ ] **Step 6: Confirm the runner lists it**

Run: `cd $EVKB && ./tools/run-all-qemu-gates.sh -l | grep -c "" ; ./tools/run-all-qemu-gates.sh -l | grep led_button`
Expected: `142` lines (141 gates + the trailing summary line) and `rt1176:display/synthui_led_button_test`.

- [ ] **Step 7: Commit (evkb)**

```bash
cd $EVKB
git add tools/gate-vacuity.test.sh tools/license-audit.sh examples/README.md CLAUDE.md
git commit -m "synthui_led_button_test: vacuity section 15 (3 cases), GATES entry, README row, gate count 140 -> 141 (NEW-25)

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 6: Push SynthUI, bump the pin, fresh-user verification, sweep (rt1176-evkb)

**Files:**
- Modify: `$EVKB/evkb.cmake:131` (the SynthUI pin)

- [ ] **Step 1: Push SynthUI and read the SHA**

```bash
cd $SYNTHUI && git push origin master && git rev-parse HEAD
```
Expected: push accepted; note the 40-char SHA. A pin must name a PUSHED SHA (the 2026-09-07 slide_toggle lesson).

- [ ] **Step 2: Bump the pin**

In `$EVKB/evkb.cmake` line 131 replace `abc44e105b3138dbf40d8c522e45464870c585c4` with the new SHA and prepend to the comment: `Bumped <date> (synthui_led_button widget, NEW-25; earlier: synthui_slide_toggle widget, NEW-30; ...`.

- [ ] **Step 3: Fresh-user verification by RUNNING the gate on the fetched ELF**

```bash
cd $EVKB/examples/display/synthui_led_button_test
rm -rf build-fetch
cmake -B build-fetch -DEVKB_FORCE_FETCH=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake 2>&1 | tee build-fetch/configure.log | grep -i "synthui"
cmake --build build-fetch
mv build build-local && ln -s build-fetch build && ./run_qemu.sh; rc=$?; rm build && mv build-local build; echo "fetched-elf gate rc=$rc"
```
Expected: the configure log shows SynthUI cloned at the new SHA; the gate PASSES against the fetched-source ELF (`rc=0`). A configure that succeeds proves the subdirectory resolves; only the gate run proves the fetched code behaves. Remove `build-fetch` afterwards.

- [ ] **Step 4: Rebuild the other SynthUI-linking gate dirs** (a pin move does not rebuild them; a stale ELF passes vacuously)

```bash
cd $EVKB && for d in examples/display/synthui_*_test examples/display/acid_box examples/display/rotary_knob_bench examples/display/vglite_lvgl_test; do (cd $d && cmake --build build 2>&1 | tail -1); done
```
Expected: each prints its final link line; none fails. (`acid_box` and `vglite_lvgl_test` link SynthUI too.)

- [ ] **Step 5: Full sweep, one run, output captured**

Run: `cd $EVKB && ./tools/run-all-qemu-gates.sh 2>&1 | tee /tmp/sweep-led-button.log | tail -5`
Expected: `gates: 141 passed`, exit 0, `0 SKIP`. Read `docs/KNOWN-BROKEN-GATES.md` first; a red in the load-sensitivity class (`cm4_audio_test`, `m2_rx_demo[txaggr]`, `m2_uap_lwip[uap]`, `bt_tone_test[media]`, `synthui_slide_toggle_test`) is re-run idle before it is believed. Nothing else concurrent during the sweep — not the audit, not `bench_check`.

- [ ] **Step 6: Licence audit after the sweep**

Run: `cd $EVKB && ./tools/license-audit.sh 2>&1 | tail -2`
Expected: `LICENSE-AUDIT: PASS`, 120 manifests.

- [ ] **Step 7: Commit the pin and the close-out note (evkb)**

Add a `✅ **Measured <date>: 141 gates discovered, 141 passed, 0 failed, 0 SKIP**` block at the top of the measurement history in `CLAUDE.md` (above the 2026-09-15 landscape block) recording: the sweep line, audit PASS + manifest count, vacuity 58/58, fresh-user verified by RUNNING the gate on the fetched ELF, and the SynthUI SHA.

```bash
cd $EVKB
git add evkb.cmake CLAUDE.md
git commit -m "evkb.cmake: SynthUI pin -> <sha> (synthui_led_button, NEW-25); close-out: sweep 141/141/0, audit PASS, vacuity 58/58, fresh-user gate run on the fetched ELF

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 7: Silicon — golden, fps checkpoint, transcript (bench session)

**Files:**
- Create: `$EVKB/examples/display/synthui_led_button_test/transcript_hw_evkb.txt`

This task needs the EVKB on the bench. Follow the `flashing-rt1170-evkb` skill; the notes below are the acid_box-era recipe.

- [ ] **Step 1: Flash without the VCOM held**

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
cd $EVKB/examples/display/synthui_led_button_test
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/synthui_led_button_test.hex
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build/synthui_led_button_test.hex
```
Expected: `File matches flash`. (Use the `.hex`: LinkServer refused `vglite_conformance`'s `.elf` once and the `.hex` is the same bytes.)

- [ ] **Step 2: Attach the console, then press SW4**

Run: `python3 $EVKB/tools/rt1170-console.py /dev/cu.usbmodem* 115200 | tee transcript_hw_evkb.txt` then press SW4.
Expected: the same token sequence as QEMU; `led_button_delta_eq=PASS`; `led_button_vsync ... timeouts=0`; `led_button_fps frames=64 mfps_med=... ` — the ≥30 fps criterion is `mfps_med >= 30000`.

- [ ] **Step 3: Repeat the boot twice more** (SW4) and confirm `led_button_crc` is bit-identical on all three boots. One boot hid a nondeterminism defect in NEW-20; three is the floor.

- [ ] **Step 4: Eyes on the glass**

Confirm by eye: ivory caps, red/amber/green/blue LEDs with a soft halo when lit, the latched keys sitting visibly lower with a dimmer highlight, the red bezel on the cue keys, grey disabled LEDs with no halo, no dots on the 32 px key and dots on the 34 px one, the 120x80 key drawn as a centred square. Then the Phase B animation: no flashing squares (the scanout-flash class is invisible to checksums; a camera at 60 fps is the instrument if in doubt).

- [ ] **Step 5: Record and commit**

Head `transcript_hw_evkb.txt` with a dated summary: the silicon golden (three boots), `mfps_med`, the verdict against 30 fps, and anything the eye found. If `mfps_med < 30000`, file a Linear issue "SynthUI LedButton GC355 compositor" and record the number; do not widen this task.

```bash
cd $EVKB
git add examples/display/synthui_led_button_test/transcript_hw_evkb.txt
git commit -m "synthui_led_button_test: silicon acceptance -- golden 0x........ on three boots, mfps_med=....., fences clean, eyes on glass (NEW-25)

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

Then move NEW-25 to Done in Linear with the transcript's summary as the closing comment.
