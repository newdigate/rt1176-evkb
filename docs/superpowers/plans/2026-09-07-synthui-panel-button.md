# SynthUI PanelButton Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the DC PanelButton primitive as `synthui_panel_button` in SynthUI (LVGL 9, sw delta rendering), verified by host unit tests and a new double-buffered QEMU-gated example `examples/display/synthui_panel_button_test` in rt1176-evkb.

**Architecture:** Spec: [`docs/superpowers/specs/2026-09-07-synthui-panel-button-design.md`](file:///Users/moolet/Development/rt1170/rt1176-evkb/docs/superpowers/specs/2026-09-07-synthui-panel-button-design.md). Single widget TU in SynthUI with an 8-layer draw pass (`LV_EVENT_DRAW_MAIN`), early-return delta setters on unchanged state, and scoped widget-level invalidation. The consumer example runs the hardware double-buffer pipeline (`lvgl_mipi_panel_create_db()`) and executes Phase A correctness (pinned golden CRC, 64-step LCG delta sequence, delta-equality guard, damage bounds) and Phase B (continuous animation benchmark).

**Tech Stack:** LVGL 9.4.0 (vendored in rt1176-evkb tree), SynthUI (`/Users/moolet/Development/SynthUI`, local-first), rt1176-evkb CMake + QEMU (`/Users/moolet/Development/qemu-rt1170/build/qemu-system-arm`).

## Global Constraints
- Clean-room provenance rules: `reference/` is never compiled; clean-room vector rebuilds from written descriptions only.
- `./run_qemu.sh`, never `sh run_qemu.sh`.
- Use the QEMU binary at `/Users/moolet/Development/qemu-rt1170/build/qemu-system-arm`.
- Goldens are RECORDED from two bit-identical consecutive QEMU runs, never derived.
- A demonstrated-RED is required for gate assertions.

---

### Task 1: `synthui_panel_button_types.h`, `synthui_panel_button_math.h` + Host Unit Test (SynthUI repo)

**Files:**
- Create: `/Users/moolet/Development/SynthUI/src/synthui_panel_button_types.h`
- Create: `/Users/moolet/Development/SynthUI/src/synthui_panel_button_math.h`
- Create: `/Users/moolet/Development/SynthUI/tests/panel_button_test.c`
- Modify: `/Users/moolet/Development/SynthUI/tests/run.sh`

**Interfaces:**
- Produces: `synthui_panel_button_glyph_t`, `synthui_panel_button_geom_t`, `synthui_panel_button_triangle_t`, `synthui_panel_button_rect_t`, `synthui_panel_button_circle_t`, `synthui_panel_button_compute_geom(float w, float h, float glyph_scale, synthui_panel_button_geom_t *g)`

- [ ] **Step 1: Write the failing host test**

Create `/Users/moolet/Development/SynthUI/tests/panel_button_test.c`:
```c
/* panel_button_test.c - host unit test for pure panel button geometry & math.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#undef NDEBUG
#include "../src/synthui_panel_button_math.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static int approx_eq(float a, float b) { return fabsf(a - b) < 0.05f; }

int main(void)
{
    synthui_panel_button_geom_t g;

    /* 74x58 Transport button (DC default) */
    assert(synthui_panel_button_compute_geom(74.0f, 58.0f, 0.62f, &g));
    assert(approx_eq(g.vw, 100.0f));
    assert(approx_eq(g.vh, 78.0f));       /* round(100*58/74) = 78 */
    assert(approx_eq(g.u, 0.74f));
    assert(approx_eq(g.i, 2.2f));
    assert(approx_eq(g.inner_w, 95.6f));  /* 100 - 4.4 */
    assert(approx_eq(g.inner_h, 73.6f));  /* 78 - 4.4 */
    assert(approx_eq(g.sheen_h, 8.58f));  /* 78 * 0.11 */
    assert(approx_eq(g.sheen_y, 10.78f)); /* 2.2 + 8.58 */
    assert(approx_eq(g.sheen_line, 3.9f));/* 78 * 0.05 */
    assert(approx_eq(g.shadow_h, 7.8f));  /* 78 * 0.1 */
    assert(approx_eq(g.shadow_y, 68.0f)); /* 78 - 2.2 - 7.8 */
    assert(approx_eq(g.g, 48.36f));       /* min(100, 78) * 0.62 = 48.36 */
    assert(approx_eq(g.s, 0.4836f));
    assert(approx_eq(g.tx, 25.82f));      /* (100 - 48.36)/2 */
    assert(approx_eq(g.ty, 14.82f));      /* (78 - 48.36)/2 */

    /* Verify Play glyph geometry (1 triangle) */
    synthui_panel_button_glyph_geom_t gg;
    synthui_panel_button_get_glyph_geom(&g, SYNTHUI_PANEL_BUTTON_GLYPH_PLAY, &gg);
    assert(gg.num_triangles == 1);
    assert(gg.num_rects == 0);
    assert(gg.num_circles == 0);
    assert(approx_eq(gg.triangles[0].p[0].x, 25.82f + 0.4836f * 32.0f));
    assert(approx_eq(gg.triangles[0].p[0].y, 14.82f + 0.4836f * 18.0f));

    /* Verify Stop glyph geometry (1 rect) */
    synthui_panel_button_get_glyph_geom(&g, SYNTHUI_PANEL_BUTTON_GLYPH_STOP, &gg);
    assert(gg.num_triangles == 0);
    assert(gg.num_rects == 1);
    assert(gg.num_circles == 0);
    assert(approx_eq(gg.rects[0].w, 0.4836f * 44.0f));
    assert(approx_eq(gg.rects[0].h, 0.4836f * 44.0f));

    /* Verify Record glyph geometry (1 circle) */
    synthui_panel_button_get_glyph_geom(&g, SYNTHUI_PANEL_BUTTON_GLYPH_RECORD, &gg);
    assert(gg.num_triangles == 0);
    assert(gg.num_rects == 0);
    assert(gg.num_circles == 1);
    assert(approx_eq(gg.circles[0].r, 0.4836f * 30.0f));

    /* Verify Rewind glyph geometry (2 triangles) */
    synthui_panel_button_get_glyph_geom(&g, SYNTHUI_PANEL_BUTTON_GLYPH_REWIND, &gg);
    assert(gg.num_triangles == 2);

    /* Verify None glyph geometry */
    synthui_panel_button_get_glyph_geom(&g, SYNTHUI_PANEL_BUTTON_GLYPH_NONE, &gg);
    assert(gg.num_triangles == 0 && gg.num_rects == 0 && gg.num_circles == 0);

    /* Color definitions */
    assert(SYNTHUI_PANEL_BUTTON_ACCENT_GREEN == 0x48E070u);
    assert(SYNTHUI_PANEL_BUTTON_ACCENT_AMBER == 0xF0A030u);
    assert(SYNTHUI_PANEL_BUTTON_ACCENT_RED == 0xF04848u);
    assert(SYNTHUI_PANEL_BUTTON_ACCENT_BLUE == 0x90A8F0u);
    assert(SYNTHUI_PANEL_BUTTON_ACCENT_PALE == 0xD8F0F0u);
    assert(SYNTHUI_PANEL_BUTTON_ACCENT_NONE == 0x303048u);

    /* Degenerate dimensions */
    assert(!synthui_panel_button_compute_geom(0.0f, 58.0f, 0.62f, &g));
    assert(!synthui_panel_button_compute_geom(74.0f, 0.0f, 0.62f, &g));

    printf("panel_button_test: all PASS\n");
    return 0;
}
```

Add to `/Users/moolet/Development/SynthUI/tests/run.sh`:
```sh
cc -Wall -Wextra -Werror -o "$out/panel_button_test" tests/panel_button_test.c
"$out/panel_button_test"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/moolet/Development/SynthUI && ./tests/run.sh`
Expected: FAIL (`fatal error: '../src/synthui_panel_button_math.h' file not found`).

- [ ] **Step 3: Implement `src/synthui_panel_button_types.h` and `src/synthui_panel_button_math.h`**

Create `/Users/moolet/Development/SynthUI/src/synthui_panel_button_types.h`:
```c
/* synthui_panel_button_types.h - types and constants for SynthUI PanelButton.
 * Header-only and LVGL-free for host testing.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_PANEL_BUTTON_TYPES_H
#define SYNTHUI_PANEL_BUTTON_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNTHUI_PANEL_BUTTON_GLYPH_PLAY = 0,
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

#define SYNTHUI_PANEL_BUTTON_ACCENT_GREEN   0x48E070u
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

Create `/Users/moolet/Development/SynthUI/src/synthui_panel_button_math.h`:
```c
/* synthui_panel_button_math.h - pure geometry arithmetic for SynthUI PanelButton.
 * Header-only and LVGL-free for direct host unit testing.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_PANEL_BUTTON_MATH_H
#define SYNTHUI_PANEL_BUTTON_MATH_H

#include <stdbool.h>
#include <math.h>
#include "synthui_panel_button_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x, y;
} synthui_panel_button_point_t;

typedef struct {
    synthui_panel_button_point_t p[3];
} synthui_panel_button_triangle_t;

typedef struct {
    float x, y, w, h;
} synthui_panel_button_rect_t;

typedef struct {
    float cx, cy, r;
} synthui_panel_button_circle_t;

typedef struct {
    float w, h;
    float vw, vh, u;
    float i;
    float inner_w, inner_h;
    float sheen_h, sheen_y, sheen_line;
    float shadow_y, shadow_h;
    float g, s, tx, ty;
    float glow_w;
} synthui_panel_button_geom_t;

typedef struct {
    uint8_t num_triangles;
    uint8_t num_rects;
    uint8_t num_circles;
    synthui_panel_button_triangle_t triangles[2];
    synthui_panel_button_rect_t rects[1];
    synthui_panel_button_circle_t circles[1];
} synthui_panel_button_glyph_geom_t;

static inline bool synthui_panel_button_compute_geom(float w, float h, float glyph_scale,
                                                     synthui_panel_button_geom_t *g)
{
    if (w <= 0.0f || h <= 0.0f) return false;
    if (glyph_scale <= 0.0f) glyph_scale = 0.62f;

    g->w = w;
    g->h = h;
    g->vw = 100.0f;
    g->vh = roundf((100.0f * h) / w);
    g->u = w / 100.0f;

    g->i = 2.2f;
    g->inner_w = g->vw - (g->i * 2.0f);
    g->inner_h = g->vh - (g->i * 2.0f);

    g->sheen_h = g->vh * 0.11f;
    g->sheen_y = g->i + g->sheen_h;
    g->sheen_line = g->vh * 0.05f;

    g->shadow_h = g->vh * 0.1f;
    g->shadow_y = g->vh - g->i - g->shadow_h;

    const float min_dim = g->vw < g->vh ? g->vw : g->vh;
    g->g = min_dim * glyph_scale;
    g->s = g->g / 100.0f;
    g->tx = (g->vw - g->g) * 0.5f;
    g->ty = (g->vh - g->g) * 0.5f;
    g->glow_w = 14.0f * g->s;

    return true;
}

static inline void synthui_panel_button_get_glyph_geom(const synthui_panel_button_geom_t *g,
                                                       synthui_panel_button_glyph_t glyph,
                                                       synthui_panel_button_glyph_geom_t *out)
{
    out->num_triangles = 0;
    out->num_rects = 0;
    out->num_circles = 0;

    const float tx = g->tx;
    const float ty = g->ty;
    const float s = g->s;

    switch (glyph) {
    case SYNTHUI_PANEL_BUTTON_GLYPH_PLAY:
        out->num_triangles = 1;
        out->triangles[0].p[0] = (synthui_panel_button_point_t){ tx + s * 32.0f, ty + s * 18.0f };
        out->triangles[0].p[1] = (synthui_panel_button_point_t){ tx + s * 84.0f, ty + s * 50.0f };
        out->triangles[0].p[2] = (synthui_panel_button_point_t){ tx + s * 32.0f, ty + s * 82.0f };
        break;

    case SYNTHUI_PANEL_BUTTON_GLYPH_STOP:
        out->num_rects = 1;
        out->rects[0] = (synthui_panel_button_rect_t){ tx + s * 28.0f, ty + s * 28.0f, s * 44.0f, s * 44.0f };
        break;

    case SYNTHUI_PANEL_BUTTON_GLYPH_RECORD:
        out->num_circles = 1;
        out->circles[0] = (synthui_panel_button_circle_t){ tx + s * 50.0f, ty + s * 50.0f, s * 30.0f };
        break;

    case SYNTHUI_PANEL_BUTTON_GLYPH_REWIND:
        out->num_triangles = 2;
        out->triangles[0].p[0] = (synthui_panel_button_point_t){ tx + s * 50.0f, ty + s * 22.0f };
        out->triangles[0].p[1] = (synthui_panel_button_point_t){ tx + s * 20.0f, ty + s * 50.0f };
        out->triangles[0].p[2] = (synthui_panel_button_point_t){ tx + s * 50.0f, ty + s * 78.0f };
        out->triangles[1].p[0] = (synthui_panel_button_point_t){ tx + s * 80.0f, ty + s * 22.0f };
        out->triangles[1].p[1] = (synthui_panel_button_point_t){ tx + s * 50.0f, ty + s * 50.0f };
        out->triangles[1].p[2] = (synthui_panel_button_point_t){ tx + s * 80.0f, ty + s * 78.0f };
        break;

    case SYNTHUI_PANEL_BUTTON_GLYPH_FORWARD:
        out->num_triangles = 2;
        out->triangles[0].p[0] = (synthui_panel_button_point_t){ tx + s * 50.0f, ty + s * 22.0f };
        out->triangles[0].p[1] = (synthui_panel_button_point_t){ tx + s * 80.0f, ty + s * 50.0f };
        out->triangles[0].p[2] = (synthui_panel_button_point_t){ tx + s * 50.0f, ty + s * 78.0f };
        out->triangles[1].p[0] = (synthui_panel_button_point_t){ tx + s * 20.0f, ty + s * 22.0f };
        out->triangles[1].p[1] = (synthui_panel_button_point_t){ tx + s * 50.0f, ty + s * 50.0f };
        out->triangles[1].p[2] = (synthui_panel_button_point_t){ tx + s * 20.0f, ty + s * 78.0f };
        break;

    case SYNTHUI_PANEL_BUTTON_GLYPH_UP:
        out->num_triangles = 1;
        out->triangles[0].p[0] = (synthui_panel_button_point_t){ tx + s * 50.0f, ty + s * 30.0f };
        out->triangles[0].p[1] = (synthui_panel_button_point_t){ tx + s * 74.0f, ty + s * 66.0f };
        out->triangles[0].p[2] = (synthui_panel_button_point_t){ tx + s * 26.0f, ty + s * 66.0f };
        break;

    case SYNTHUI_PANEL_BUTTON_GLYPH_DOWN:
        out->num_triangles = 1;
        out->triangles[0].p[0] = (synthui_panel_button_point_t){ tx + s * 50.0f, ty + s * 70.0f };
        out->triangles[0].p[1] = (synthui_panel_button_point_t){ tx + s * 74.0f, ty + s * 34.0f };
        out->triangles[0].p[2] = (synthui_panel_button_point_t){ tx + s * 26.0f, ty + s * 34.0f };
        break;

    case SYNTHUI_PANEL_BUTTON_GLYPH_BAR:
        out->num_rects = 1;
        out->rects[0] = (synthui_panel_button_rect_t){ tx + s * 18.0f, ty + s * 42.0f, s * 64.0f, s * 16.0f };
        break;

    case SYNTHUI_PANEL_BUTTON_GLYPH_DOT:
        out->num_circles = 1;
        out->circles[0] = (synthui_panel_button_circle_t){ tx + s * 50.0f, ty + s * 50.0f, s * 16.0f };
        break;

    case SYNTHUI_PANEL_BUTTON_GLYPH_NONE:
    default:
        break;
    }
}

#ifdef __cplusplus
}
#endif
#endif /* SYNTHUI_PANEL_BUTTON_MATH_H */
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd /Users/moolet/Development/SynthUI && ./tests/run.sh`
Expected: PASS (`panel_button_test: all PASS`).

- [ ] **Step 5: Commit in SynthUI**

```bash
cd /Users/moolet/Development/SynthUI
git add src/synthui_panel_button_types.h src/synthui_panel_button_math.h tests/panel_button_test.c tests/run.sh
git commit -m "synthui_panel_button: geometry math, types, and host unit tests (NEW-27)"
```

---

### Task 2: `synthui_panel_button.h` and `synthui_panel_button.cpp` (SynthUI repo)

**Files:**
- Create: `/Users/moolet/Development/SynthUI/src/synthui_panel_button.h`
- Create: `/Users/moolet/Development/SynthUI/src/synthui_panel_button.cpp`

**Interfaces:**
- Produces: `synthui_panel_button_class`, `synthui_panel_button_create()`, `synthui_panel_button_set_on()`, `synthui_panel_button_get_on()`, `synthui_panel_button_set_glyph()`, `synthui_panel_button_get_glyph()`, `synthui_panel_button_set_accent()`, `synthui_panel_button_get_accent()`, `synthui_panel_button_set_glyph_scale()`, `synthui_panel_button_get_glyph_scale()`

- [ ] **Step 1: Create `src/synthui_panel_button.h`**

Create `/Users/moolet/Development/SynthUI/src/synthui_panel_button.h`:
```c
/* synthui_panel_button.h - SynthUI PanelButton, LVGL 9 custom widget.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_PANEL_BUTTON_H
#define SYNTHUI_PANEL_BUTTON_H

#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>
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

- [ ] **Step 2: Create `src/synthui_panel_button.cpp`**

Create `/Users/moolet/Development/SynthUI/src/synthui_panel_button.cpp`:
```cpp
/* synthui_panel_button.cpp - SynthUI PanelButton, LVGL 9 custom widget.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#include "synthui_panel_button.h"
#include "synthui_panel_button_math.h"
#include <lvgl_private.h>
#include <math.h>

#define MY_CLASS (&synthui_panel_button_class)

typedef struct {
    lv_obj_t obj;
    uint32_t accent;
    synthui_panel_button_glyph_t glyph;
    float glyph_scale;
    bool on;
} synthui_panel_button_t;

static void btn_constructor(const lv_obj_class_t *cls, lv_obj_t *obj);
static void btn_destructor(const lv_obj_class_t *cls, lv_obj_t *obj);
static void btn_event(const lv_obj_class_t *cls, lv_event_t *e);
static void btn_draw(synthui_panel_button_t *btn, lv_layer_t *layer);

const lv_obj_class_t synthui_panel_button_class = {
    .base_class     = &lv_obj_class,
    .constructor_cb = btn_constructor,
    .destructor_cb  = btn_destructor,
    .event_cb       = btn_event,
    .name           = "synthui_panel_button",
    .width_def      = 74,
    .height_def     = 58,
    .instance_size  = sizeof(synthui_panel_button_t),
};

lv_obj_t *synthui_panel_button_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_class_create_obj(&synthui_panel_button_class, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

static void btn_constructor(const lv_obj_class_t *cls, lv_obj_t *obj)
{
    LV_UNUSED(cls);
    synthui_panel_button_t *btn = (synthui_panel_button_t *)obj;
    btn->accent = SYNTHUI_PANEL_BUTTON_ACCENT_DEFAULT;
    btn->glyph = SYNTHUI_PANEL_BUTTON_GLYPH_PLAY;
    btn->glyph_scale = 0.62f;
    btn->on = false;
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void btn_destructor(const lv_obj_class_t *cls, lv_obj_t *obj)
{
    LV_UNUSED(cls);
    LV_UNUSED(obj);
}

static void btn_event(const lv_obj_class_t *cls, lv_event_t *e)
{
    LV_UNUSED(cls);
    if (lv_obj_event_base(MY_CLASS, e) != LV_RESULT_OK) return;
    if (lv_event_get_code(e) == LV_EVENT_DRAW_MAIN) {
        btn_draw((synthui_panel_button_t *)lv_event_get_current_target_obj(e),
                 lv_event_get_layer(e));
    }
}

static void btn_draw(synthui_panel_button_t *btn, lv_layer_t *layer)
{
    lv_area_t a;
    lv_obj_get_coords((lv_obj_t *)btn, &a);
    const int32_t w = lv_area_get_width(&a);
    const int32_t h = lv_area_get_height(&a);
    if (w <= 0 || h <= 0) return;

    synthui_panel_button_geom_t g;
    if (!synthui_panel_button_compute_geom((float)w, (float)h, btn->glyph_scale, &g)) return;

    const lv_state_t st = lv_obj_get_state((const lv_obj_t *)btn);
    const bool disabled = (st & LV_STATE_DISABLED) != 0;

    /* 1. Bezel: #303048, radius round(2.0 * u), min 1 */
    int32_t bezel_r = (int32_t)lroundf(2.0f * g.u);
    if (bezel_r < 1) bezel_r = 1;
    lv_draw_rect_dsc_t bezel_dsc;
    lv_draw_rect_dsc_init(&bezel_dsc);
    bezel_dsc.bg_color = lv_color_hex(0x303048);
    bezel_dsc.bg_opa = LV_OPA_COVER;
    bezel_dsc.radius = bezel_r;
    lv_draw_rect(layer, &bezel_dsc, &a);

    /* 2. Body Face: inset by round(2.2 * u), vertical 2-stop gradient */
    const int32_t inset_px = (int32_t)lroundf(g.i * g.u);
    lv_area_t body_area;
    body_area.x1 = a.x1 + inset_px;
    body_area.y1 = a.y1 + inset_px;
    body_area.x2 = a.x2 - inset_px;
    body_area.y2 = a.y2 - inset_px;
    if (body_area.x1 > body_area.x2 || body_area.y1 > body_area.y2) return;

    const uint32_t top_c = btn->on ? 0xA0B4F4u : 0x90A8F0u;
    const uint32_t bot_c = 0x486090u;
    lv_draw_rect_dsc_t body_dsc;
    lv_draw_rect_dsc_init(&body_dsc);
    body_dsc.bg_opa = disabled ? LV_OPA_50 : LV_OPA_COVER;
    body_dsc.bg_grad.dir = LV_GRAD_DIR_VER;
    body_dsc.bg_grad.stops_count = 2;
    body_dsc.bg_grad.stops[0].color = lv_color_hex(top_c);
    body_dsc.bg_grad.stops[0].opa = LV_OPA_COVER;
    body_dsc.bg_grad.stops[0].frac = 0;
    body_dsc.bg_grad.stops[1].color = lv_color_hex(bot_c);
    body_dsc.bg_grad.stops[1].opa = LV_OPA_COVER;
    body_dsc.bg_grad.stops[1].frac = 255;
    body_dsc.radius = 0;
    lv_draw_rect(layer, &body_dsc, &body_area);

    /* 3. Sheen Band: #D8D8F0, opacity 191 (75%) or 102 (disabled) */
    const int32_t sheen_h_px = (int32_t)lroundf(g.sheen_h * g.u);
    if (sheen_h_px > 0) {
        lv_area_t sheen_area = body_area;
        sheen_area.y2 = body_area.y1 + sheen_h_px - 1;
        if (sheen_area.y2 > body_area.y2) sheen_area.y2 = body_area.y2;
        lv_draw_rect_dsc_t sheen_dsc;
        lv_draw_rect_dsc_init(&sheen_dsc);
        sheen_dsc.bg_color = lv_color_hex(0xD8D8F0);
        sheen_dsc.bg_opa = disabled ? 102 : 191;
        sheen_dsc.radius = 0;
        lv_draw_rect(layer, &sheen_dsc, &sheen_area);
    }

    /* 4. Sheen Line: #F0F0F0, opacity 230 (90%) or 128 (disabled) */
    int32_t sheen_line_h = (int32_t)lroundf(g.sheen_line * g.u);
    if (sheen_line_h < 1) sheen_line_h = 1;
    const int32_t sheen_y_px = a.y1 + (int32_t)lroundf(g.sheen_y * g.u);
    lv_area_t line_area = body_area;
    line_area.y1 = sheen_y_px;
    line_area.y2 = sheen_y_px + sheen_line_h - 1;
    if (line_area.y2 <= body_area.y2 && line_area.y1 >= body_area.y1) {
        lv_draw_rect_dsc_t line_dsc;
        lv_draw_rect_dsc_init(&line_dsc);
        line_dsc.bg_color = lv_color_hex(0xF0F0F0);
        line_dsc.bg_opa = disabled ? 128 : 230;
        line_dsc.radius = 0;
        lv_draw_rect(layer, &line_dsc, &line_area);
    }

    /* 5. Shadow Band: #303048, opacity 128 (50%) or 76 (disabled) */
    const int32_t shadow_h_px = (int32_t)lroundf(g.shadow_h * g.u);
    if (shadow_h_px > 0) {
        const int32_t shadow_y_px = a.y1 + (int32_t)lroundf(g.shadow_y * g.u);
        lv_area_t shadow_area = body_area;
        shadow_area.y1 = shadow_y_px;
        if (shadow_area.y1 <= body_area.y2) {
            lv_draw_rect_dsc_t shadow_dsc;
            lv_draw_rect_dsc_init(&shadow_dsc);
            shadow_dsc.bg_color = lv_color_hex(0x303048);
            shadow_dsc.bg_opa = disabled ? 76 : 128;
            shadow_dsc.radius = 0;
            lv_draw_rect(layer, &shadow_dsc, &shadow_area);
        }
    }

    /* 6. Wash Overlay (if on and not disabled): accent, opacity 31 (12%) */
    const lv_color_t accent_color = lv_color_hex(btn->accent);
    if (btn->on && !disabled) {
        lv_draw_rect_dsc_t wash_dsc;
        lv_draw_rect_dsc_init(&wash_dsc);
        wash_dsc.bg_color = accent_color;
        wash_dsc.bg_opa = 31;
        wash_dsc.radius = 0;
        lv_draw_rect(layer, &wash_dsc, &body_area);
    }

    /* Get glyph geometry */
    synthui_panel_button_glyph_geom_t gg;
    synthui_panel_button_get_glyph_geom(&g, btn->glyph, &gg);

    const int32_t glow_w_px = (int32_t)lroundf(g.glow_w * g.u);
    const int32_t half_glow_px = (int32_t)lroundf((g.glow_w * 0.5f) * g.u);

    /* 7. Glyph Glow (if on and not disabled): accent, opacity 82 (32%) */
    if (btn->on && !disabled && glow_w_px > 0) {
        /* Triangles: draw lines along edges with rounded caps */
        for (int i = 0; i < gg.num_triangles; i++) {
            for (int e_idx = 0; e_idx < 3; e_idx++) {
                int next = (e_idx + 1) % 3;
                lv_draw_line_dsc_t line_dsc;
                lv_draw_line_dsc_init(&line_dsc);
                line_dsc.color = accent_color;
                line_dsc.opa = 82;
                line_dsc.width = glow_w_px;
                line_dsc.round_start = 1;
                line_dsc.round_end = 1;
                line_dsc.p1.x = (lv_value_precise_t)lroundf(a.x1 + gg.triangles[i].p[e_idx].x * g.u);
                line_dsc.p1.y = (lv_value_precise_t)lroundf(a.y1 + gg.triangles[i].p[e_idx].y * g.u);
                line_dsc.p2.x = (lv_value_precise_t)lroundf(a.x1 + gg.triangles[i].p[next].x * g.u);
                line_dsc.p2.y = (lv_value_precise_t)lroundf(a.y1 + gg.triangles[i].p[next].y * g.u);
                lv_draw_line(layer, &line_dsc);
            }
        }

        /* Rectangles: expand by half_glow */
        for (int i = 0; i < gg.num_rects; i++) {
            lv_area_t glow_r;
            glow_r.x1 = a.x1 + (int32_t)lroundf(gg.rects[i].x * g.u) - half_glow_px;
            glow_r.y1 = a.y1 + (int32_t)lroundf(gg.rects[i].y * g.u) - half_glow_px;
            glow_r.x2 = a.x1 + (int32_t)lroundf((gg.rects[i].x + gg.rects[i].w) * g.u) - 1 + half_glow_px;
            glow_r.y2 = a.y1 + (int32_t)lroundf((gg.rects[i].y + gg.rects[i].h) * g.u) - 1 + half_glow_px;
            lv_draw_rect_dsc_t glow_rect_dsc;
            lv_draw_rect_dsc_init(&glow_rect_dsc);
            glow_rect_dsc.bg_color = accent_color;
            glow_rect_dsc.bg_opa = 82;
            glow_rect_dsc.radius = half_glow_px > 1 ? half_glow_px : 1;
            lv_draw_rect(layer, &glow_rect_dsc, &glow_r);
        }

        /* Circles: expand radius by half_glow */
        for (int i = 0; i < gg.num_circles; i++) {
            const int32_t cx_px = a.x1 + (int32_t)lroundf(gg.circles[i].cx * g.u);
            const int32_t cy_px = a.y1 + (int32_t)lroundf(gg.circles[i].cy * g.u);
            const int32_t r_px = (int32_t)lroundf(gg.circles[i].r * g.u) + half_glow_px;
            lv_area_t glow_c;
            glow_c.x1 = cx_px - r_px;
            glow_c.y1 = cy_px - r_px;
            glow_c.x2 = cx_px + r_px - 1;
            glow_c.y2 = cy_px + r_px - 1;
            lv_draw_rect_dsc_t glow_circ_dsc;
            lv_draw_rect_dsc_init(&glow_circ_dsc);
            glow_circ_dsc.bg_color = accent_color;
            glow_circ_dsc.bg_opa = 82;
            glow_circ_dsc.radius = LV_RADIUS_CIRCLE;
            lv_draw_rect(layer, &glow_circ_dsc, &glow_c);
        }
    }

    /* 8. Glyph Core: on ? accent : #303048 */
    const lv_color_t glyph_color = btn->on ? accent_color : lv_color_hex(0x303048);
    const lv_opa_t glyph_opa = disabled ? 102 : LV_OPA_COVER;

    /* Triangles */
    for (int i = 0; i < gg.num_triangles; i++) {
        lv_draw_triangle_dsc_t tri_dsc;
        lv_draw_triangle_dsc_init(&tri_dsc);
        tri_dsc.color = glyph_color;
        tri_dsc.opa = glyph_opa;
        for (int p_idx = 0; p_idx < 3; p_idx++) {
            tri_dsc.p[p_idx].x = (lv_value_precise_t)lroundf(a.x1 + gg.triangles[i].p[p_idx].x * g.u);
            tri_dsc.p[p_idx].y = (lv_value_precise_t)lroundf(a.y1 + gg.triangles[i].p[p_idx].y * g.u);
        }
        lv_draw_triangle(layer, &tri_dsc);
    }

    /* Rectangles */
    for (int i = 0; i < gg.num_rects; i++) {
        lv_area_t r_area;
        r_area.x1 = a.x1 + (int32_t)lroundf(gg.rects[i].x * g.u);
        r_area.y1 = a.y1 + (int32_t)lroundf(gg.rects[i].y * g.u);
        r_area.x2 = a.x1 + (int32_t)lroundf((gg.rects[i].x + gg.rects[i].w) * g.u) - 1;
        r_area.y2 = a.y1 + (int32_t)lroundf((gg.rects[i].y + gg.rects[i].h) * g.u) - 1;
        lv_draw_rect_dsc_t rect_dsc;
        lv_draw_rect_dsc_init(&rect_dsc);
        rect_dsc.bg_color = glyph_color;
        rect_dsc.bg_opa = glyph_opa;
        rect_dsc.radius = 0;
        lv_draw_rect(layer, &rect_dsc, &r_area);
    }

    /* Circles */
    for (int i = 0; i < gg.num_circles; i++) {
        const int32_t cx_px = a.x1 + (int32_t)lroundf(gg.circles[i].cx * g.u);
        const int32_t cy_px = a.y1 + (int32_t)lroundf(gg.circles[i].cy * g.u);
        const int32_t r_px = (int32_t)lroundf(gg.circles[i].r * g.u);
        lv_area_t circ_area;
        circ_area.x1 = cx_px - r_px;
        circ_area.y1 = cy_px - r_px;
        circ_area.x2 = cx_px + r_px - 1;
        circ_area.y2 = cy_px + r_px - 1;
        lv_draw_rect_dsc_t circ_dsc;
        lv_draw_rect_dsc_init(&circ_dsc);
        circ_dsc.bg_color = glyph_color;
        circ_dsc.bg_opa = glyph_opa;
        circ_dsc.radius = LV_RADIUS_CIRCLE;
        lv_draw_rect(layer, &circ_dsc, &circ_area);
    }
}

void synthui_panel_button_set_on(lv_obj_t *obj, bool on)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    synthui_panel_button_t *btn = (synthui_panel_button_t *)obj;
    if (btn->on == on) return;
    btn->on = on;
    lv_obj_invalidate(obj);
}

bool synthui_panel_button_get_on(const lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    const synthui_panel_button_t *btn = (const synthui_panel_button_t *)obj;
    return btn->on;
}

void synthui_panel_button_set_glyph(lv_obj_t *obj, synthui_panel_button_glyph_t glyph)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    synthui_panel_button_t *btn = (synthui_panel_button_t *)obj;
    if (btn->glyph == glyph) return;
    btn->glyph = glyph;
    lv_obj_invalidate(obj);
}

synthui_panel_button_glyph_t synthui_panel_button_get_glyph(const lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    const synthui_panel_button_t *btn = (const synthui_panel_button_t *)obj;
    return btn->glyph;
}

void synthui_panel_button_set_accent(lv_obj_t *obj, uint32_t rgb_hex)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    synthui_panel_button_t *btn = (synthui_panel_button_t *)obj;
    if (btn->accent == rgb_hex) return;
    btn->accent = rgb_hex;
    lv_obj_invalidate(obj);
}

uint32_t synthui_panel_button_get_accent(const lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    const synthui_panel_button_t *btn = (const synthui_panel_button_t *)obj;
    return btn->accent;
}

void synthui_panel_button_set_glyph_scale(lv_obj_t *obj, float scale)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    synthui_panel_button_t *btn = (synthui_panel_button_t *)obj;
    if (btn->glyph_scale == scale) return;
    btn->glyph_scale = scale;
    lv_obj_invalidate(obj);
}

float synthui_panel_button_get_glyph_scale(const lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    const synthui_panel_button_t *btn = (const synthui_panel_button_t *)obj;
    return btn->glyph_scale;
}
```

- [ ] **Step 3: Run host tests**

Run: `cd /Users/moolet/Development/SynthUI && ./tests/run.sh`
Expected: PASS.

- [ ] **Step 4: Commit in SynthUI**

```bash
cd /Users/moolet/Development/SynthUI
git add src/synthui_panel_button.h src/synthui_panel_button.cpp
git commit -m "synthui_panel_button: LVGL 9 custom widget implementation (NEW-27)"
```

---

### Task 3: `examples/display/synthui_panel_button_test` (rt1176-evkb repo)

**Files:**
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_panel_button_test/CMakeLists.txt`
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_panel_button_test/synthui_panel_button_test.cpp`

**Interfaces:**
- Consumes: `synthui_panel_button.h`, `lvgl_mipi_panel.h`, `Display.h`
- Produces: `synthui_panel_button_test.elf`

- [ ] **Step 1: Create `CMakeLists.txt`**

Create `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_panel_button_test/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.24)
project(synthui_panel_button_test)

add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
import_evkb_synthui()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(synthui_panel_button_test
    synthui_panel_button_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp
)
teensy_target_link_libraries(synthui_panel_button_test cores MipiDisplay PXP)

target_link_libraries(synthui_panel_button_test.elf SynthUI LVGL stdc++)

target_include_directories(synthui_panel_button_test.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(PANEL_BUTTON_EYEBALL_HOLD)
    add_compile_definitions(PANEL_BUTTON_EYEBALL_HOLD=${PANEL_BUTTON_EYEBALL_HOLD})
endif()
```

- [ ] **Step 2: Create `synthui_panel_button_test.cpp`**

Create `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_panel_button_test/synthui_panel_button_test.cpp`:
- Configures 16-button test scene (4×4 grid on 720×1280 RK055 panel) exercising all 10 glyphs, accents, sizes, and states.
- Double-buffered pipeline via `lvgl_mipi_panel_create_db(Display)`.
- Phase A1: Full initial render emitting `panel_button_crc=0x...` over the presented buffer.
- Phase A2: 64-step deterministic delta sequence driven by fixed-seed LCG toggling buttons across the grid.
- Delta-equality check comparing delta CRC with fresh full-render CRC (`panel_button_delta_crc == panel_button_fresh_crc`).
- Damage engagement check (`panel_button_damage max=... total=...`).
- Tokens: `SYNTHUI_PANEL_BUTTON_BEGIN`, `PANEL_OK`, `panel_button_scene=16 grid=4x4`, `LVGL_FLUSHED=PASS`, `LVGL_BYTES=3686400`, `panel_button_crc=0x...`, `panel_button_delta_crc=0x...`, `panel_button_fresh_crc=0x...`, `panel_button_delta_eq=PASS`, `panel_button_damage max=...`, `panel_button_vsync flips=... isrs=... timeouts=0`, `crc_done`, `PASS: SynthUI panel_button render verified`.
- Phase B: Animated running loop measuring frame intervals (`panel_button_fps`).

- [ ] **Step 3: Build the example**

```bash
mkdir -p /Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_panel_button_test/build
cd /Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_panel_button_test/build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../../../../cmake/toolchain-arm-none-eabi.cmake
cmake --build .
```
Expected: `synthui_panel_button_test.elf` created successfully.

- [ ] **Step 4: Commit scaffold**

```bash
cd /Users/moolet/Development/rt1170/rt1176-evkb
git add examples/display/synthui_panel_button_test/CMakeLists.txt examples/display/synthui_panel_button_test/synthui_panel_button_test.cpp
git commit -m "synthui_panel_button_test: example scaffolding and 16-button test scene (NEW-27)"
```

---

### Task 4: QEMU Gate Runner & Golden Verification (rt1176-evkb repo)

**Files:**
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_panel_button_test/run_qemu.sh`
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_panel_button_test/transcript_qemu.txt`

- [ ] **Step 1: Create `run_qemu.sh`**

Create `examples/display/synthui_panel_button_test/run_qemu.sh`:
- Uses `tools/qrun` to execute `synthui_panel_button_test.elf`.
- Verifies `PANEL_OK`
- Verifies `LVGL_FLUSHED=PASS`
- Verifies `LVGL_BYTES=3686400`
- Verifies pinned golden `panel_button_crc=0x...`
- Verifies `panel_button_delta_eq=PASS`
- Verifies `panel_button_damage max <= 10000`
- Verifies `panel_button_vsync ... timeouts=0`
- Verifies `crc_done` and `PASS: SynthUI panel_button render verified`

- [ ] **Step 2: Run in QEMU to record initial golden CRC**

Run QEMU across two consecutive boots, confirm bit-identical golden CRC, and pin it in `run_qemu.sh`.

- [ ] **Step 3: Demonstrate Tripwires RED**

Temporarily flip the expected CRC in `run_qemu.sh` to demonstrate that a checksum mismatch fails the gate. Restore the true golden.

- [ ] **Step 4: Verify gate passes GREEN**

Run: `cd /Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_panel_button_test && ./run_qemu.sh`
Expected: `PASS: SynthUI panel_button render verified`. Save output to `transcript_qemu.txt`.

- [ ] **Step 5: Commit gate and test results**

```bash
cd /Users/moolet/Development/rt1170/rt1176-evkb
git add examples/display/synthui_panel_button_test/run_qemu.sh examples/display/synthui_panel_button_test/transcript_qemu.txt
git commit -m "synthui_panel_button_test: QEMU gate verified green with golden CRC (NEW-27)"
```
