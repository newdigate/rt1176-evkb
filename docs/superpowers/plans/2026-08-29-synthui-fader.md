# SynthUI Fader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the DC Fader as `synthui_fader` (LVGL 9, sw delta rendering), verified by a new gated example `examples/display/synthui_fader_test` (sweep 122 → 123) and an early silicon fps checkpoint.

**Architecture:** Spec: `docs/superpowers/specs/2026-08-29-synthui-fader-design.md`. One widget TU in SynthUI (rotary pattern: all painting in DRAW_MAIN, no local styles, manual invalidation), delta damage = one cap-extent union rect per `set_value`, 1:1 anchor-based drag. No GPU TU — the silicon checkpoint (Task 8) decides if one is ever needed. The example runs the db pipeline and carries the delta-equality guard, engagement check, and vsync witness.

**Tech Stack:** LVGL 9.4.0 (vendored, `~/Development/LVGL`), SynthUI (`~/Development/SynthUI`, local-first), evkb CMake + QEMU gates, LinkServer for silicon.

**House rules that bind every task:** `./run_qemu.sh`, never `sh run_qemu.sh`. Goldens are RECORDED from two bit-identical consecutive QEMU runs, never derived, and the frames get LOOKED AT once via the eyeball hold. Fixtures are captured AFTER the gate runs. A demonstrated-RED is required for every new guard. `LV_GRADIENT_MAX_STOPS` is 2 in `LVGL/port/lv_conf.h` — do not touch it (lv_conf hard-defines beat `-D`); the cap gradient is two stacked 2-stop rects by design.

---

### Task 1: `synthui_fader_math.h` + host test (SynthUI repo)

**Files:**
- Create: `~/Development/SynthUI/tests/fader_math_test.c`
- Create: `~/Development/SynthUI/src/synthui_fader_math.h`
- Modify: `~/Development/SynthUI/tests/run.sh` (append two lines)

- [ ] **Step 1: Write the failing test**

Create `~/Development/SynthUI/tests/fader_math_test.c`:

```c
/* Host-compiled unit test for the fader's pure drag math -- no LVGL, no
 * target.  Build: tests/run.sh
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#undef NDEBUG          /* assertions must survive a -DNDEBUG build; BEFORE every
                        * include, so no header can pull in assert.h first */
#include "../src/synthui_fader_math.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static int approx_eq(float a, float b) { return fabsf(a - b) < 0.001f; }

int main(void)
{
    /* sign: an upward drag (positive dy_up_px) increases the value */
    assert(approx_eq(synthui_fader_drag(0.5f,  40.0f, 160.0f), 0.75f));
    assert(approx_eq(synthui_fader_drag(0.5f, -40.0f, 160.0f), 0.25f));

    /* scaling: the TRAVEL, not a hardcoded stroke, sets the sweep -- a
     * 200 px travel needs 100 px of drag for half the range */
    assert(approx_eq(synthui_fader_drag(0.0f, 160.0f, 160.0f), 1.0f));
    assert(approx_eq(synthui_fader_drag(0.0f, 100.0f, 200.0f), 0.5f));

    /* clamping at both rails */
    assert(approx_eq(synthui_fader_drag(0.9f,  500.0f, 160.0f), 1.0f));
    assert(approx_eq(synthui_fader_drag(0.1f, -500.0f, 160.0f), 0.0f));

    /* ANCHOR-TOTAL mapping (spec section 8): overshoot past the rail and a
     * partial return lands where the POSITION says, not where an
     * accumulator would -- this case distinguishes the two designs */
    assert(approx_eq(synthui_fader_drag(0.0f, 200.0f, 160.0f), 1.0f));
    assert(approx_eq(synthui_fader_drag(0.0f,  80.0f, 160.0f), 0.5f));

    /* degenerate travel: anchor unchanged (spec section 11) */
    assert(approx_eq(synthui_fader_drag(0.3f, 50.0f,  0.0f), 0.3f));
    assert(approx_eq(synthui_fader_drag(0.3f, 50.0f, -5.0f), 0.3f));

    /* NaN displacement: anchor unchanged */
    assert(approx_eq(synthui_fader_drag(0.3f, nanf(""), 160.0f), 0.3f));

    /* no motion */
    assert(approx_eq(synthui_fader_drag(0.42f, 0.0f, 160.0f), 0.42f));

    printf("fader_math: all PASS\n");
    return 0;
}
```

Append to `~/Development/SynthUI/tests/run.sh` (after the rotary_palette lines):

```sh
cc -Wall -Wextra -Werror -o "$out/fader_math_test" tests/fader_math_test.c
"$out/fader_math_test"
```

- [ ] **Step 2: Run to verify it fails**

Run: `~/Development/SynthUI/tests/run.sh`
Expected: FAIL — `fatal error: '../src/synthui_fader_math.h' file not found` (knob_math and rotary_palette still pass first).

- [ ] **Step 3: Write the header**

Create `~/Development/SynthUI/src/synthui_fader_math.h`:

```c
/* synthui_fader_math.h - pure drag arithmetic for the fader's input layer.
 * Header-only and LVGL-free so a host compiler can unit-test it directly;
 * synthui_fader.cpp includes it for the real widget.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_FADER_MATH_H
#define SYNTHUI_FADER_MATH_H

/* 1:1 ANCHOR-TOTAL mapping (spec 2026-08-29 section 8): the widget anchors
 * value and press-y on PRESSED, and every PRESSING event maps the TOTAL
 * displacement -- dy_up_px = press_y - current_y (positive = finger moved
 * up) -- over the travel length.  Dragging the full travel sweeps the full
 * 0..1 range; the cap never jumps to the finger.  There is no per-poll
 * accumulator here on purpose: total mapping is what makes an overshoot
 * behave like a physical cap (see the host test's overshoot case).
 * A degenerate travel (<= 0) and a NaN displacement return the anchor. */
static inline float synthui_fader_drag(float anchor, float dy_up_px,
                                       float travel_px)
{
    if (!(dy_up_px == dy_up_px) || travel_px <= 0.0f) return anchor;
    float v = anchor + dy_up_px / travel_px;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

#endif /* SYNTHUI_FADER_MATH_H */
```

- [ ] **Step 4: Run to verify it passes**

Run: `~/Development/SynthUI/tests/run.sh`
Expected: `knob_math: all PASS`, `rotary_palette: all PASS`, `fader_math: all PASS`, exit 0.

- [ ] **Step 5: Commit (SynthUI repo)**

```bash
cd ~/Development/SynthUI && git add src/synthui_fader_math.h tests/fader_math_test.c tests/run.sh && git commit -m "fader: 1:1 anchor-total drag math + host tests (NEW-23)"
```

---

### Task 2: the `synthui_fader` widget (SynthUI repo)

**Files:**
- Create: `~/Development/SynthUI/src/synthui_fader.h`
- Create: `~/Development/SynthUI/src/synthui_fader.cpp`
- Modify: `~/Development/SynthUI/README.md` (status paragraph)

Compile check rides the EXISTING `synthui_knob_test` build: `import_evkb_synthui()` globs `src/*.cpp` with `CONFIGURE_DEPENDS`, so the new TU compiles into libSynthUI with no CMake change, and the knob gate then proves the addition regressed nothing.

- [ ] **Step 1: Write the public header**

Create `~/Development/SynthUI/src/synthui_fader.h`:

```c
/* synthui_fader.h - SynthUI Fader, LVGL 9 custom widget.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_FADER_H
#define SYNTHUI_FADER_H

#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The DC panel greys (Fader.dc.html panel options); set_panel takes any
 * rgb, so these are a convenience, not an enum. */
#define SYNTHUI_FADER_PANEL_DEFAULT 0x6D7A85u
#define SYNTHUI_FADER_PANEL_DARK    0x5B6570u
#define SYNTHUI_FADER_PANEL_LIGHT   0x7D8994u
#define SYNTHUI_FADER_PANEL_DEEP    0x4A535Cu

extern const lv_obj_class_t synthui_fader_class;

lv_obj_t *synthui_fader_create(lv_obj_t *parent);

/* Programmatic state changes (lv_obj_add_state/remove_state) need a manual
 * lv_obj_invalidate(): no local styles, so LVGL's style-diff repaint never
 * fires (same contract as the rotary).  LV_STATE_PRESSED selects the active
 * cap palette, LV_STATE_DISABLED the disabled palette.  The input layer
 * (1:1 anchor-total vertical drag over the travel length) emits
 * LV_EVENT_VALUE_CHANGED. */
void  synthui_fader_set_value(lv_obj_t *obj, float v01);   /* clamp 0..1; NaN ignored; delta-invalidates */
float synthui_fader_get_value(const lv_obj_t *obj);
void  synthui_fader_set_ticks(lv_obj_t *obj, uint8_t n);   /* clamp 2..33, default 13 */
void  synthui_fader_set_center(lv_obj_t *obj, bool on);    /* default false */
void  synthui_fader_set_panel(lv_obj_t *obj, uint32_t rgb);/* default SYNTHUI_FADER_PANEL_DEFAULT */

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Write the widget TU**

Create `~/Development/SynthUI/src/synthui_fader.cpp`:

```c
/* synthui_fader.cpp - SynthUI Fader, LVGL 9 custom widget.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Clean-room build from the written description of Fader.dc.html in the
 * design spec (evkb docs/superpowers/specs/2026-08-29-synthui-fader-design.md
 * section 4).  All geometry is evaluated in viewBox-unit space (width 100,
 * height vh = 100*H/W, u = W/100 px per unit) and rounded to px only at
 * draw time -- identically in full and delta paints, because both run this
 * one draw function.
 *
 * Delta damage (spec section 7): set_value invalidates ONE rect, the union
 * of the old and new cap extents (pure vertical motion, so the union is
 * exact).  Correctness of the partial repaint is proved per boot by the
 * consuming gate's delta-equality guard, not assumed here. */
#include "synthui_fader.h"
#include "synthui_fader_math.h"
/* lv_obj_t by value needs the complete private type (the rotary's note). */
#include <lvgl_private.h>
#include <math.h>

#define MY_CLASS (&synthui_fader_class)

typedef struct {
    lv_obj_t obj;
    float value;          /* 0..1; 1 = cap at the top */
    float press_anchor;   /* value at PRESSED (anchor-total drag) */
    float press_y;        /* screen y at PRESSED */
    uint32_t panel;
    uint8_t ticks;
    bool center;
} synthui_fader_t;

/* value-independent geometry, all in viewBox units */
typedef struct {
    float u;       /* px per unit */
    float vh;      /* viewBox height in units */
    float cap_h, top, travel;
} fd_geom_t;

static void fd_constructor(const lv_obj_class_t *cls, lv_obj_t *obj);
static void fd_event(const lv_obj_class_t *cls, lv_event_t *e);
static void fd_input_pressed(lv_event_t *e);
static void fd_input_pressing(lv_event_t *e);
static void fd_input_state(lv_event_t *e);

const lv_obj_class_t synthui_fader_class = {
    .base_class     = &lv_obj_class,
    .constructor_cb = fd_constructor,
    .event_cb       = fd_event,
    /* designators must follow lv_obj_class_private.h declaration order --
     * name declares before width_def (the rotary's note). */
    .name           = "synthui_fader",
    .width_def      = 78,       /* the DC defaults */
    .height_def     = 210,
    .instance_size  = sizeof(synthui_fader_t),
};

lv_obj_t *synthui_fader_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_class_create_obj(&synthui_fader_class, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

static void fd_constructor(const lv_obj_class_t *cls, lv_obj_t *obj)
{
    LV_UNUSED(cls);
    synthui_fader_t *f = (synthui_fader_t *)obj;
    f->value = 0.5f;                       /* DC default */
    f->press_anchor = 0.5f;
    f->press_y = 0.0f;
    f->panel = SYNTHUI_FADER_PANEL_DEFAULT;
    f->ticks = 13;
    f->center = false;
    /* same scroll rationale as the rotary (and lv_slider): a vertical drag
     * inside a scrollable container would move the value AND scroll. */
    lv_obj_remove_flag(obj, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                            LV_OBJ_FLAG_SCROLL_CHAIN_VER));
    lv_obj_add_event_cb(obj, fd_input_pressed,  LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(obj, fd_input_pressing, LV_EVENT_PRESSING,   NULL);
    lv_obj_add_event_cb(obj, fd_input_state,    LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(obj, fd_input_state,    LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(obj, fd_input_state,    LV_EVENT_PRESS_LOST, NULL);
}

/* false = degenerate size (avoids the W=0 division); value stays settable,
 * drawing and delta invalidation are skipped. */
static bool fd_geom(const synthui_fader_t *f, fd_geom_t *g, lv_area_t *coords)
{
    lv_obj_get_coords((lv_obj_t *)&f->obj, coords);
    const float W = (float)lv_area_get_width(coords);
    const float H = (float)lv_area_get_height(coords);
    if (W < 1.0f || H < 1.0f) return false;
    g->u = W / 100.0f;
    g->vh = 100.0f * H / W;
    g->cap_h = fmaxf(14.0f, 0.11f * g->vh);
    g->top = 0.06f * g->vh;
    g->travel = g->vh - 2.0f * g->top - g->cap_h;
    if (g->travel < 0.0f) g->travel = 0.0f;
    return true;
}

static float fd_cap_y(const fd_geom_t *g, float value)
{
    return g->top + (1.0f - value) * g->travel;
}

/* Cap extent (spec section 7): union of the stroked body and the offset
 * shadow -- x 3.2..94 units, y capY-0.8 .. capY+capH+2.5 -- rounded outward
 * and inflated 2 px.  This is the ONLY damage a value change produces. */
static bool fd_cap_extent(const synthui_fader_t *f, float value, lv_area_t *a)
{
    lv_area_t coords; fd_geom_t g;
    if (!fd_geom(f, &g, &coords)) return false;
    const float cy = fd_cap_y(&g, value);
    a->x1 = coords.x1 + (int32_t)floorf(3.2f * g.u) - 2;
    a->x2 = coords.x1 + (int32_t)ceilf(94.0f * g.u) + 2;
    a->y1 = coords.y1 + (int32_t)floorf((cy - 0.8f) * g.u) - 2;
    a->y2 = coords.y1 + (int32_t)ceilf((cy + g.cap_h + 2.5f) * g.u) + 2;
    return true;
}

void synthui_fader_set_value(lv_obj_t *obj, float v01)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    synthui_fader_t *f = (synthui_fader_t *)obj;
    if (!(v01 == v01)) return;             /* NaN ignored (spec section 11) */
    if (v01 < 0.0f) v01 = 0.0f;
    if (v01 > 1.0f) v01 = 1.0f;
    if (f->value == v01) return;
    lv_area_t a_old, a_new;
    const bool ok = fd_cap_extent(f, f->value, &a_old) &&
                    fd_cap_extent(f, v01, &a_new);
    f->value = v01;
    if (!ok) return;                       /* degenerate size: value only */
    lv_area_t un = { LV_MIN(a_old.x1, a_new.x1), LV_MIN(a_old.y1, a_new.y1),
                     LV_MAX(a_old.x2, a_new.x2), LV_MAX(a_old.y2, a_new.y2) };
    lv_obj_invalidate_area(obj, &un);
}

float synthui_fader_get_value(const lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    return ((const synthui_fader_t *)obj)->value;
}

#define FD_SETTER(obj, field, val) do { \
    synthui_fader_t *f = (synthui_fader_t *)obj; \
    if (f->field == (val)) return; \
    f->field = (val); \
    lv_obj_invalidate(obj); } while (0)

void synthui_fader_set_ticks(lv_obj_t *obj, uint8_t n)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    if (n < 2) n = 2;
    if (n > 33) n = 33;
    FD_SETTER(obj, ticks, n);
}
void synthui_fader_set_center(lv_obj_t *obj, bool on)
{ LV_ASSERT_OBJ(obj, MY_CLASS); FD_SETTER(obj, center, on); }
void synthui_fader_set_panel(lv_obj_t *obj, uint32_t rgb)
{ LV_ASSERT_OBJ(obj, MY_CLASS); FD_SETTER(obj, panel, rgb); }

/* ---- palette (spec section 5) -- one pure function so a future second
 * engine shares it (the rotary's two-engines-one-palette seam, prepared
 * but not built) ---- */
typedef struct {
    uint32_t cap_top, cap_mid, cap_low, ticks, center;
    lv_opa_t gloss_opa;
} synthui_fader_palette_t;

static void fd_palette(bool disabled, bool pressed, synthui_fader_palette_t *p)
{
    p->cap_top = disabled ? 0xD2D5D4 : (pressed ? 0xFFFFFF : 0xF4F5F4);
    p->cap_mid = disabled ? 0xB4B8B8 : 0xDCDEDD;
    p->cap_low = disabled ? 0x9AA0A1 : (pressed ? 0xC8CBCA : 0xB6BABA);
    p->ticks   = disabled ? 0xC8CDD0 : 0xE8EEF0;
    p->center  = disabled ? 0x8F9598 : 0x20262A;
    p->gloss_opa = disabled ? 77 : 191;    /* 0.30 / 0.75 of 255 */
}

/* ---- input layer (spec section 8) ---- */
static void fd_input_pressed(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    synthui_fader_t *f = (synthui_fader_t *)obj;
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == NULL) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    f->press_anchor = f->value;
    f->press_y = (float)p.y;
}

static void fd_input_pressing(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    synthui_fader_t *f = (synthui_fader_t *)obj;
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == NULL) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t coords; fd_geom_t g;
    if (!fd_geom(f, &g, &coords)) return;
    const float next = synthui_fader_drag(f->press_anchor,
                                          f->press_y - (float)p.y,
                                          g.travel * g.u);
    if (next == f->value) return;
    synthui_fader_set_value(obj, next);
    lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL);
}

/* Press/release changes ONLY cap colors (spec section 3), so the state
 * repaint is the cap extent, not the whole widget. */
static void fd_input_state(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    synthui_fader_t *f = (synthui_fader_t *)obj;
    lv_area_t a;
    if (fd_cap_extent(f, f->value, &a)) lv_obj_invalidate_area(obj, &a);
}

/* ---- drawing (spec sections 4 & 6) ---- */

/* Rect in unit space; x2/y2 are LVGL-inclusive, hence the -1. */
static void fd_rect(lv_layer_t *layer, const lv_draw_rect_dsc_t *d,
                    float x0, float y0, float x, float y, float w, float h,
                    float u)
{
    lv_area_t a = { (int32_t)lroundf(x0 + x * u),
                    (int32_t)lroundf(y0 + y * u),
                    (int32_t)lroundf(x0 + (x + w) * u) - 1,
                    (int32_t)lroundf(y0 + (y + h) * u) - 1 };
    lv_draw_rect(layer, d, &a);
}

static void fd_grad_rect(lv_layer_t *layer, float x0, float y0, float x,
                         float y, float w, float h, float u,
                         uint32_t c_top, uint32_t c_bot)
{
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.bg_grad.dir = LV_GRAD_DIR_VER;
    d.bg_grad.stops_count = 2;
    d.bg_grad.stops[0].color = lv_color_hex(c_top);
    d.bg_grad.stops[0].opa = LV_OPA_COVER;
    d.bg_grad.stops[0].frac = 0;
    d.bg_grad.stops[1].color = lv_color_hex(c_bot);
    d.bg_grad.stops[1].opa = LV_OPA_COVER;
    d.bg_grad.stops[1].frac = 255;
    fd_rect(layer, &d, x0, y0, x, y, w, h, u);
}

static void fd_hline(lv_layer_t *layer, float x0, float y0, float xa,
                     float xb, float y, float w_units, float u,
                     uint32_t hex, lv_opa_t opa)
{
    lv_draw_line_dsc_t l; lv_draw_line_dsc_init(&l);
    l.color = lv_color_hex(hex);
    l.opa = opa;
    l.width = (int32_t)lroundf(w_units * u);
    if (l.width < 1) l.width = 1;
    l.p1.x = x0 + xa * u; l.p1.y = y0 + y * u;
    l.p2.x = x0 + xb * u; l.p2.y = y0 + y * u;
    lv_draw_line(layer, &l);
}

static void fd_draw(synthui_fader_t *f, lv_layer_t *layer)
{
    lv_area_t c; fd_geom_t g;
    if (!fd_geom(f, &g, &c)) return;
    const float u = g.u;
    const float x0 = (float)c.x1, y0 = (float)c.y1;
    const lv_state_t st = lv_obj_get_state(&f->obj);
    synthui_fader_palette_t pal;
    fd_palette((st & LV_STATE_DISABLED) != 0,
               (st & LV_STATE_PRESSED) != 0, &pal);

    /* panel */
    {
        lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(f->panel); d.bg_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &d, &c);
    }

    /* ticks: x 8..92, every 4th brighter (0.62 vs 0.34 -> 158 vs 87) */
    {
        const float tick_w = fmaxf(1.4f, 0.012f * g.vh);
        const int n = f->ticks;
        for (int i = 0; i < n; i++) {
            const float ty = g.top + g.cap_h * 0.5f
                             + (float)i * g.travel / (float)(n - 1);
            fd_hline(layer, x0, y0, 8.0f, 92.0f, ty, tick_w, u,
                     pal.ticks, (i % 4 == 0) ? 158 : 87);
        }
    }

    /* rod (the slot): x 46.5 w 7, r 1.5, spanning the cap-center travel +-2 */
    {
        lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(0x14181B); d.bg_opa = LV_OPA_COVER;
        d.radius = (int32_t)lroundf(1.5f * u);
        fd_rect(layer, &d, x0, y0, 46.5f, g.top + g.cap_h * 0.5f - 2.0f,
                7.0f, g.travel + 4.0f, u);
    }

    /* center-detent line (option) */
    if (f->center)
        fd_hline(layer, x0, y0, 4.0f, 96.0f,
                 g.top + g.cap_h * 0.5f + g.travel * 0.5f, 2.4f, u,
                 pal.center, LV_OPA_COVER);

    /* cap */
    {
        const float cy = fd_cap_y(&g, f->value);
        const float ch = g.cap_h;
        const float bw = 1.6f;             /* body stroke, units */

        /* shadow: solid dark rect at +2/+2.5, 45% (the DC uses no blur) */
        lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(0x1B1F22); d.bg_opa = 115;
        d.radius = (int32_t)lroundf(2.0f * u);
        fd_rect(layer, &d, x0, y0, 6.0f, cy + 2.5f, 88.0f, ch, u);

        /* body base: capMid fill + stroke; the gradients are inset inside
         * the border with square corners -- the corner pixels keep capMid,
         * sub-pixel at r ~= 1.6 px (spec section 6) */
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(pal.cap_mid); d.bg_opa = LV_OPA_COVER;
        d.radius = (int32_t)lroundf(2.0f * u);
        d.border_color = lv_color_hex(0x20262A);
        d.border_opa = LV_OPA_COVER;
        d.border_width = (int32_t)lroundf(bw * u);
        if (d.border_width < 1) d.border_width = 1;
        fd_rect(layer, &d, x0, y0, 4.0f, cy, 88.0f, ch, u);

        /* 3-stop gradient as two stacked 2-stop rects, split at 0.46 */
        fd_grad_rect(layer, x0, y0, 4.0f + bw, cy + bw,
                     88.0f - 2.0f * bw, 0.46f * ch - bw, u,
                     pal.cap_top, pal.cap_mid);
        fd_grad_rect(layer, x0, y0, 4.0f + bw, cy + 0.46f * ch,
                     88.0f - 2.0f * bw, 0.54f * ch - bw, u,
                     pal.cap_mid, pal.cap_low);

        /* groove across the middle: y 0.43..0.57 of capH */
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(0x20262A); d.bg_opa = LV_OPA_COVER;
        fd_rect(layer, &d, x0, y0, 4.0f, cy + 0.43f * ch, 88.0f,
                0.14f * ch, u);

        /* two gloss strips */
        const float gh = fmaxf(1.5f, 0.12f * ch);
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(0xFFFFFF); d.bg_opa = pal.gloss_opa;
        fd_rect(layer, &d, x0, y0, 9.0f, cy + 0.16f * ch, 78.0f, gh, u);
        fd_rect(layer, &d, x0, y0, 9.0f, cy + 0.68f * ch, 78.0f, gh, u);
    }
}

static void fd_event(const lv_obj_class_t *cls, lv_event_t *e)
{
    LV_UNUSED(cls);
    if (lv_obj_event_base(MY_CLASS, e) != LV_RESULT_OK) return;
    if (lv_event_get_code(e) == LV_EVENT_DRAW_MAIN)
        fd_draw((synthui_fader_t *)lv_event_get_current_target_obj(e),
                lv_event_get_layer(e));
}
```

- [ ] **Step 3: Compile via the existing knob-test build**

Run: `cd ~/Development/rt1170/evkb/examples/display/synthui_knob_test && cmake --build build 2>&1 | tail -3`
Expected: clean build (the `CONFIGURE_DEPENDS` glob picks the new TU up). Fix any compile error before moving on.

- [ ] **Step 4: Prove the addition regressed nothing**

Run: `cd ~/Development/rt1170/evkb/examples/display/synthui_knob_test && ./run_qemu.sh`
Expected: `PASS: SynthUI knob render verified` — every knob golden untouched with the fader TU linked into libSynthUI.

- [ ] **Step 5: README status touch (spec section 12)**

In `~/Development/SynthUI/README.md`, replace the stale status paragraph (lines 8–10, which still name the deleted `src/synthui_knob`) with:

```markdown
Status: reference material plus three widgets — `src/synthui_rotary_knob`
(two-engine: LVGL-sw + optional GC355 compositor in `src/vglite/`),
`src/synthui_step`, and `src/synthui_fader` (LVGL-sw, delta rendering).
Drag math is host-tested in `tests/`, run via `tests/run.sh`.
```

Also update the provenance bullet on line 36 that says "`src/synthui_knob` is built that way" to "`src/` widgets are built that way".

- [ ] **Step 6: Commit (SynthUI repo)**

```bash
cd ~/Development/SynthUI && git add src/synthui_fader.h src/synthui_fader.cpp README.md && git commit -m "fader: synthui_fader widget -- DC Fader clean-room, sw delta rendering (NEW-23)"
```

---

### Task 3: `examples/display/synthui_fader_test` — scene + Phase A tokens (evkb repo)

**Files:**
- Create: `examples/display/synthui_fader_test/CMakeLists.txt`
- Create: `examples/display/synthui_fader_test/synthui_fader_test.cpp`

- [ ] **Step 1: Write CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.24)
project(synthui_fader_test)

# XRGB8888 exactly like synthui_knob_test. Directory scope on purpose: the
# definitions must reach the LVGL, MipiDisplay AND SynthUI objects.
add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
# Plain import, no VGLITE: the fader is sw-only BY DESIGN (spec section 1) --
# the silicon checkpoint decides whether a GPU TU ever exists.
import_evkb_synthui()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)    # Display::fillScreen() paints via the PXP

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(synthui_fader_test
    synthui_fader_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp)
teensy_target_link_libraries(synthui_fader_test cores MipiDisplay PXP)
target_link_libraries(synthui_fader_test.elf SynthUI LVGL stdc++)

# Diagnostic eyeball build (separate build dir, NEVER a golden):
#   cmake -B build-eye -DFD_EYEBALL_HOLD=<1..3> -DCMAKE_TOOLCHAIN_FILE=...
# holds after the n-th checksummed frame for a QEMU-monitor pmemsave
# (recipe in acid_box/transcript_qemu.txt). 1=bank, 2=delta result, 3=fresh.
if(FD_EYEBALL_HOLD)
    add_compile_definitions(FD_EYEBALL_HOLD=${FD_EYEBALL_HOLD})
endif()
```

- [ ] **Step 2: Write the sketch**

Create `examples/display/synthui_fader_test/synthui_fader_test.cpp`:

```c
/* synthui_fader_test - synthui_fader (NEW-23) on the RK055 (720x1280
 * XRGB8888, db pipeline), checksummed.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Scene: the spec section-9 16-fader bank, 2x8 at 78x210, config axes
 * exercised inside the ONE golden (values i/15; center on 4..7; 12
 * DISABLED; 13 PRESSED, so all three palettes are golden-pinned; panel
 * greys cycled i%4; ticks 33 on 8, 5 on 15, 13 elsewhere).
 *
 * Phase A (gated): fd_crc golden -> 64-step LCG delta sequence ->
 * fd_delta_crc vs fd_fresh_crc (the gate compares them -- never
 * re-goldened), fd_damage engagement, fd_vsync, crc_done.
 * Phase B (after crc_done, ungated -- QEMU timing is meaningless): 60 s
 * all-16 sine animation -> fd_fps, then a 10 s full-invalidate run ->
 * fd_fps_fullinv, the honest what-delta-buys baseline (spec section 10).
 * The bank keeps animating forever afterwards for the eyes/camera pass. */
#include <Arduino.h>
#include <string.h>
#include <math.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_fader.h"

static const uint32_t panel_opt[4] = {
    SYNTHUI_FADER_PANEL_DEFAULT, SYNTHUI_FADER_PANEL_DARK,
    SYNTHUI_FADER_PANEL_LIGHT,   SYNTHUI_FADER_PANEL_DEEP };

static lv_obj_t *g_fader[16];
static float g_vals[16];

#ifdef FD_EYEBALL_HOLD
static void eyeball_hold(int n)
{
    if (n != FD_EYEBALL_HOLD) return;
    Serial1.printf("FD_EYEBALL_HOLD=%d\n", n);
    for (;;) { }
}
#else
#define eyeball_hold(n) ((void)0)
#endif

static void opaque_bg(lv_obj_t *scr)
{   /* Opaque ground forces LVGL to paint every pixel: fully-defined frames. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
}

/* The one bank builder both the golden and the fresh reference use -- the
 * equality guard depends on the two paths sharing this code. */
static lv_obj_t *build_bank(const float *vals, bool pressed5)
{
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SynthUI Fader");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    for (int i = 0; i < 16; i++) {
        lv_obj_t *fd = synthui_fader_create(scr);
        lv_obj_set_size(fd, 78, 210);
        lv_obj_set_pos(fd, 12 + (i % 8) * 88, 120 + (i / 8) * 280);
        synthui_fader_set_value(fd, vals[i]);
        synthui_fader_set_panel(fd, panel_opt[i % 4]);
        if (i >= 4 && i <= 7) synthui_fader_set_center(fd, true);
        if (i == 8)  synthui_fader_set_ticks(fd, 33);
        if (i == 15) synthui_fader_set_ticks(fd, 5);
        if (i == 12) lv_obj_add_state(fd, LV_STATE_DISABLED);
        if (i == 13) lv_obj_add_state(fd, LV_STATE_PRESSED);
        if (pressed5 && i == 5) lv_obj_add_state(fd, LV_STATE_PRESSED);
        g_fader[i] = fd;
    }
    return scr;
}

/* Checksum the buffer that was PRESENTED (flipped to glass) -- in db mode
 * Display.framebuffer() is only one of the two buffers. flip_sync() first,
 * so a pending flip has retired and scanned_fb() names the front buffer. */
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

/* --- delta guards (spec section 9): per-invalidate area recorder, the
 * knob test's mechanism verbatim (LV_EVENT_INVALIDATE_AREA is a DISPLAY
 * event, sent per lv_inv_area with the clipped area). */
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

/* Fixed-seed LCG -> value steps in [-0.06, +0.06] (spec section 9; covers
 * Phase B's ~0.052 max animation step, so the guard exercises what the
 * bench runs). */
static uint32_t s_lcg;
static float lcg_delta(void)
{
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return ((float)(s_lcg >> 8) * (1.0f / 16777215.0f) - 0.5f) * 0.12f;
}

#define FD_DELTA_STEPS 64

/* Runs ON the already-rendered golden bank; evolves g_vals in place. */
static uint32_t delta_run_sequence(void)
{
    lv_display_add_event_cb(lv_display_get_default(), delta_inv_cb,
                            LV_EVENT_INVALIDATE_AREA, NULL);
    s_lcg = 0x5EEDF00Du;
    s_delta_maxarea = 0; s_delta_total = 0; s_delta_record = true;
    for (int step = 0; step < FD_DELTA_STEPS; step++) {
        for (int i = 0; i < 16; i++) {
            float v = g_vals[i] + lcg_delta();
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            g_vals[i] = v;
            synthui_fader_set_value(g_fader[i], v);
        }
        lv_refr_now(NULL);
    }
    s_delta_record = false;   /* the state toggle below SHOULD be big */
    lv_obj_add_state(g_fader[5], LV_STATE_PRESSED);
    lv_obj_invalidate(g_fader[5]);     /* programmatic-state contract */
    lv_refr_now(NULL);
    for (int i = 0; i < 16; i++) {     /* two more steps on the new state */
        float v = g_vals[i] + lcg_delta();
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        g_vals[i] = v;
        synthui_fader_set_value(g_fader[i], v);
    }
    lv_refr_now(NULL);
    return sum_active_screen();
}

/* --- fd_fps: knob-test machinery (REFR_START->REFR_READY, damage-gated by
 * RENDER_READY, screen-load frame skipped), run for a fixed wall time. */
#define FD_FPS_MAX 4096
static uint32_t g_fps_us[FD_FPS_MAX];
static volatile uint32_t g_fps_n = 0, g_fps_frames = 0;
static volatile bool g_fps_timing = false, g_fps_skip = false;
static volatile bool g_fps_rendered = false;
static volatile uint32_t g_fps_t0 = 0;
static bool g_anim_full_inv = false;
static uint32_t g_anim_step = 0;

static void fps_refr_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:  g_fps_t0 = micros(); break;
    case LV_EVENT_RENDER_READY: g_fps_rendered = true; break;
    case LV_EVENT_REFR_READY:
        if (g_fps_rendered && g_fps_timing) {
            if (g_fps_skip) g_fps_skip = false;
            else {
                g_fps_frames++;
                if (g_fps_n < FD_FPS_MAX)
                    g_fps_us[g_fps_n++] = micros() - g_fps_t0;
            }
        }
        g_fps_rendered = false;
        break;
    default: break;
    }
}

static void fd_anim_cb(lv_timer_t *t)
{
    (void)t;
    g_anim_step++;
    const float tt = (float)g_anim_step * 0.015f;   /* 15 ms timer */
    for (int i = 0; i < 16; i++) {
        synthui_fader_set_value(g_fader[i],
            0.5f + 0.5f * sinf(6.2831853f * 0.5f * tt
                               + (float)i * 0.3926991f));  /* 2*pi/16 */
        if (g_anim_full_inv) lv_obj_invalidate(g_fader[i]);
    }
}

static void fd_fps_phase(const char *tag, bool full_inv, uint32_t run_ms)
{
    static bool cbs_added = false;
    if (!cbs_added) {
        lv_display_t *disp = lv_display_get_default();
        lv_display_add_event_cb(disp, fps_refr_cb, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(disp, fps_refr_cb, LV_EVENT_RENDER_READY, NULL);
        lv_display_add_event_cb(disp, fps_refr_cb, LV_EVENT_REFR_READY, NULL);
        cbs_added = true;
    }
    g_fps_n = 0; g_fps_frames = 0; g_fps_skip = true; g_fps_timing = true;
    g_anim_full_inv = full_inv;
    lv_timer_t *anim = lv_timer_create(fd_anim_cb, 15, NULL);
    const uint32_t t0 = millis();
    while (millis() - t0 < run_ms) lvgl_rt1176_loop();
    const uint32_t elapsed = millis() - t0;
    g_fps_timing = false;
    lv_timer_delete(anim);
    uint32_t s[FD_FPS_MAX];
    const uint32_t n = g_fps_n ? g_fps_n : 1;
    memcpy(s, (const void *)g_fps_us, n * sizeof(uint32_t));
    for (uint32_t i = 1; i < n; i++) {
        const uint32_t v = s[i]; int32_t j = (int32_t)i - 1;
        while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
        s[j + 1] = v;
    }
    Serial1.printf("%s frames=%lu secs=%lu fps_avg=%lu mfps_med=%lu"
                   " us_med=%lu us_max=%lu\n", tag,
                   (unsigned long)g_fps_frames,
                   (unsigned long)(elapsed / 1000u),
                   (unsigned long)((uint64_t)g_fps_frames * 1000u / elapsed),
                   (unsigned long)(1000000000ull / (s[n / 2] ? s[n / 2] : 1)),
                   (unsigned long)s[n / 2], (unsigned long)s[n - 1]);
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("SYNTHUI_FADER_BEGIN");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) {
        /* Safe-but-only-just: with no lv_init(), lvgl_rt1176_loop() in
         * loop() returns immediately (the knob test's contract). */
        Serial1.println("crc_done");
        return;
    }
    Display.fillScreen(0x0000);

    lvgl_rt1176_begin();
    /* db pipeline: LVGL renders off-screen, the LCDIFv2 flips at vsync --
     * complete frames only, the tear-free-by-construction property the
     * checksums cannot see (spec section 1). */
    lvgl_mipi_panel_create_db(Display);

    Serial1.println("fd_scene=16 grid=2x8 size=78x210");

    /* Phase A1: the bank is the FIRST refresh, so LVGL_BYTES pins a
     * whole-screen paint. */
    for (int i = 0; i < 16; i++) g_vals[i] = (float)i / 15.0f;
    lv_screen_load(build_bank(g_vals, false));
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000)
        lvgl_rt1176_loop();
    Serial1.printf("LVGL_FLUSHED=%s\n",
                   lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    Serial1.printf("LVGL_BYTES=%lu\n",
                   (unsigned long)(lvgl_mipi_panel_flushed_px()
                                   * PANEL_BYTES_PER_PIXEL));
    Serial1.printf("fd_crc=0x%08lX\n", (unsigned long)sum_active_screen());
    eyeball_hold(1);

    /* Phase A2: delta sequence vs fresh full render.  The gate compares
     * the two sums -- gate-compared, never re-goldened. */
    const uint32_t d_seq = delta_run_sequence();
    eyeball_hold(2);
    const uint32_t d_full = sum_screen(build_bank(g_vals, true));
    eyeball_hold(3);
    Serial1.printf("fd_delta_crc=0x%08lX\n", (unsigned long)d_seq);
    Serial1.printf("fd_fresh_crc=0x%08lX\n", (unsigned long)d_full);
    Serial1.printf("fd_delta_eq=%s\n", d_seq == d_full ? "PASS" : "FAIL");
    Serial1.printf("fd_damage max=%ld total=%ld steps=%d\n",
                   (long)s_delta_maxarea, s_delta_total, FD_DELTA_STEPS);

    /* vsync-fence health (db mode): a timeout means the fence silently
     * degraded to unfenced behaviour -- gated in QEMU. */
    Serial1.printf("fd_vsync flips=%lu isrs=%lu timeouts=%lu\n",
                   (unsigned long)lvgl_mipi_panel_flips(),
                   (unsigned long)lvgl_mipi_panel_vsync_isrs(),
                   (unsigned long)lvgl_mipi_panel_vsync_timeouts());
    Serial1.println("crc_done");

    /* Phase B (ungated; silicon is where these numbers answer NEW-23).
     * A fresh bank so the animation starts from a known state. */
    lv_screen_load(build_bank(g_vals, false));
    lv_refr_now(NULL);
    fd_fps_phase("fd_fps", false, 60000u);
    fd_fps_phase("fd_fps_fullinv", true, 10000u);
    /* leave the bank animating for the eyes/camera pass + soak */
    g_anim_full_inv = false;
    lv_timer_create(fd_anim_cb, 15, NULL);
}

void loop()
{
    lvgl_rt1176_loop();
}
```

- [ ] **Step 3: Configure and build**

```bash
cd ~/Development/rt1170/evkb/examples/display/synthui_fader_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```

Expected: `synthui_fader_test.elf` produced.

- [ ] **Step 4: First QEMU run, twice — record the goldens**

```bash
cd ~/Development/rt1170/evkb && ./tools/rt1170-qemu.sh examples/display/synthui_fader_test/build/synthui_fader_test.elf 30 | tee /tmp/fd_run1.txt
./tools/rt1170-qemu.sh examples/display/synthui_fader_test/build/synthui_fader_test.elf 30 | tee /tmp/fd_run2.txt
```

(If `rt1170-qemu.sh` takes different arguments, check its header; the point is two full boots capturing UART through `crc_done`.)
Expected in both: `PANEL_OK`, `LVGL_FLUSHED=PASS`, `LVGL_BYTES=3686400`, `fd_crc=0x…`, `fd_delta_crc` == `fd_fresh_crc`, `fd_delta_eq=PASS`, `fd_damage max=` ≈ 2500–3500 (the section-9 analytic estimate is ~3000 for one union rect), `fd_vsync … timeouts=0`, `crc_done`.
Then: `diff <(grep -E "fd_crc|fd_delta|fd_fresh|LVGL_BYTES" /tmp/fd_run1.txt) <(grep -E "fd_crc|fd_delta|fd_fresh|LVGL_BYTES" /tmp/fd_run2.txt)`
Expected: no diff — bit-identical across consecutive runs. Record `fd_crc` and the measured `fd_damage max` for Task 4. If `fd_delta_eq=FAIL`, STOP and debug the extent/draw code (the guard is doing its job); do not proceed to the gate.

- [ ] **Step 5: Eyeball the golden frame once (house rule: goldens are looked at)**

```bash
cd ~/Development/rt1170/evkb/examples/display/synthui_fader_test
cmake -B build-eye -DFD_EYEBALL_HOLD=1 -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build-eye
```

Follow the pmemsave recipe in `examples/display/acid_box/transcript_qemu.txt`: boot `build-eye/synthui_fader_test.elf` with a QEMU monitor socket, wait for `FD_EYEBALL_HOLD=1`, read the framebuffer address from LCDIFv2 (`xp /1wx 0x4080820c` — do NOT reuse another example's address), `pmemsave` 3686400 bytes, convert XRGB8888→PNG (python one-liner in that recipe), and LOOK: 2×8 bank, caps at i/15 heights, center lines on faders 4–7, fader 12 muted, fader 13's white active cap, four panel greys cycling, dense ticks on 8 / sparse on 5, gradient + groove + gloss on every cap. Confirm the eyeball build printed the same `fd_crc` as Step 4 (that equality ties the inspected frame to the pinned value).

- [ ] **Step 6: Commit**

```bash
cd ~/Development/rt1170/evkb && git add examples/display/synthui_fader_test/CMakeLists.txt examples/display/synthui_fader_test/synthui_fader_test.cpp && git commit -m "synthui_fader_test: 16-bank scene, delta guards, fps phases (NEW-23)"
```

---

### Task 4: the QEMU gate + demonstrated REDs + fixture

**Files:**
- Create: `examples/display/synthui_fader_test/run_qemu.sh` (mode 755)
- Create: `examples/display/synthui_fader_test/transcript_qemu.txt` (captured, not written)

- [ ] **Step 1: Write the gate**

Create `examples/display/synthui_fader_test/run_qemu.sh` (then `chmod +x`). Replace `0xXXXXXXXX` with the `fd_crc` recorded in Task 3 Step 4, and `6000` with ~2× the measured `fd_damage max` if that measurement was not in the 2500–3500 band:

```sh
#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location (a
# hardcoded path silently loads a different tree's gate-lib.sh from a worktree).
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/synthui_fader_test.elf"
OUT=$(gate_capture_path "$DIR" synthui_fader.uart)
DBG=$(gate_capture_path "$DIR" synthui_fader.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# 20s: synthui_knob_test's 16s bring-up margin plus headroom for the 66
# lv_refr_now delta steps. Tokens land ~3s in on an idle machine.
sleep 20; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
# Panel chain first: a framebuffer no display owns would still checksum.
grep -q "PANEL_OK"          "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "fd_scene=16 grid=2x8" "$OUT" || { echo "FAIL: scene line missing"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
# Flushed AREA of the first refresh -- 720*1280*4 at XRGB8888. Value greps
# are ANCHORED (CR-tolerant \r?$): a golden must match the WHOLE value.
grep -qE "LVGL_BYTES=3686400\r?$" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }
# GOLDEN CHECKSUM -- FNV-1a over the whole 720x1280 PRESENTED buffer, the
# spec section-9 bank (all config axes inside the one scene: three states,
# center on/off, four panel greys, three tick counts). RECORDED, not
# derived -- bit-identical across two consecutive QEMU runs (Task 3), and
# the frame was LOOKED AT via -DFD_EYEBALL_HOLD=1 + pmemsave. sw-only
# widget: silicon must reproduce this exact value (no second golden set).
# On a mismatch work out WHICH of {SynthUI pin, LVGL pin, lv_conf.h, fonts,
# scene} changed; do NOT paste in whatever the board printed.
grep -qE "fd_crc=0xXXXXXXXX\r?$" "$OUT" || { echo "FAIL: bank checksum"; exit 1; }
# DELTA EQUALITY (spec section 9): a 66-step value sequence rendered via
# the widget's cap-extent delta damage must be PIXEL-IDENTICAL to a fresh
# full render of the final state. The GATE compares the two printed sums --
# never re-goldened. A too-tight extent (stale shadow/stroke pixels) fails
# here.
DSEQ=$(grep -a -oE "fd_delta_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
DFUL=$(grep -a -oE "fd_fresh_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DSEQ" ] && [ -n "$DFUL" ] || { echo "FAIL: delta guard tokens missing"; exit 1; }
[ "$DSEQ" = "$DFUL" ] || { echo "FAIL: delta render differs from full render ($DSEQ vs $DFUL)"; exit 1; }
# ENGAGEMENT: the largest single invalidated area during the recorded
# 64-step segment must stay cap-sized -- a change that quietly reverts
# set_value to full invalidation fails HERE and nowhere else (a whole
# 78x210 fader is 16380 px).
DAREA=$(grep -a -oE "fd_damage max=[0-9]+" "$OUT" | head -1 | grep -oE "[0-9]+$")
[ -n "$DAREA" ] && [ "$DAREA" -gt 0 ] || { echo "FAIL: delta damage guard missing or zero"; exit 1; }
[ "$DAREA" -le 6000 ] || { echo "FAIL: delta damage not engaged (max=$DAREA)"; exit 1; }
# vsync-fence health (db pipeline): a timeout means the tear-free property
# silently degraded with every golden still green.
grep -qE "fd_vsync flips=[0-9]+ isrs=[0-9]+ timeouts=0\r?$" "$OUT" || { echo "FAIL: vsync fence unhealthy or missing"; exit 1; }
grep -q "crc_done" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
echo "PASS: SynthUI fader render verified"
```

- [ ] **Step 2: Run the gate green**

Run: `cd examples/display/synthui_fader_test && ./run_qemu.sh`
Expected: `PASS: SynthUI fader render verified`, exit 0.

- [ ] **Step 3: Demonstrate RED #1 — corrupted golden fails by name**

Edit the gate's `fd_crc` value to end in a different digit, run `./run_qemu.sh`.
Expected: `FAIL: bank checksum`, exit 1. Restore the digit, re-run, green. Record the quote for the gate header.

- [ ] **Step 4: Demonstrate RED #2 — engagement catches a full-invalidate revert**

In `~/Development/SynthUI/src/synthui_fader.cpp`, temporarily replace the body of `synthui_fader_set_value` after the clamp with:

```c
    if (f->value == v01) return;
    f->value = v01;
    lv_obj_invalidate(obj);            /* SCRATCH: full invalidation */
```

Rebuild (`cmake --build build`) and run `./run_qemu.sh`.
Expected: `FAIL: delta damage not engaged (max=16380)` — and note `fd_delta_crc` still equals `fd_fresh_crc`, which is exactly why the engagement check exists. Revert the scratch edit (`git -C ~/Development/SynthUI checkout src/synthui_fader.cpp`), rebuild, re-run, green.

- [ ] **Step 5: Demonstrate RED #3 — equality catches a too-tight extent**

In `fd_cap_extent`, temporarily change `cy + g.cap_h + 2.5f` to `cy + g.cap_h + 0.5f` (excludes the shadow's bottom rows from the damage). Rebuild, run the gate.
Expected: `FAIL: delta render differs from full render (0x… vs 0x…)` — stale shadow pixels. Revert, rebuild, re-run, green.

- [ ] **Step 6: Record the three REDs in the gate header**

Add above the golden grep in `run_qemu.sh`:

```sh
# Demonstrated RED 2026-08-29, all three by name: (1) golden last digit
# flipped -> "FAIL: bank checksum"; (2) set_value scratch-reverted to
# lv_obj_invalidate -> "FAIL: delta damage not engaged (max=16380)" while
# the equality guard stayed GREEN (delta==fresh both full renders -- the
# engagement check is the only thing that sees this); (3) cap-extent shadow
# term 2.5 -> 0.5 -> "FAIL: delta render differs from full render".
```

(Adjust the max= number to what RED #2 actually printed.)

- [ ] **Step 7: Capture the fixture AFTER the gate runs (2026-08-25 staleness lesson)**

```bash
cd examples/display/synthui_fader_test && ./run_qemu.sh && cp "$(ls build/synthui_fader.uart 2>/dev/null || echo build/serial.uart)" transcript_qemu.txt
```

(The capture path is what `gate_capture_path "$DIR" synthui_fader.uart` resolved to — confirm with `ls build/`.)

- [ ] **Step 8: Commit**

```bash
cd ~/Development/rt1170/evkb && git add examples/display/synthui_fader_test/run_qemu.sh examples/display/synthui_fader_test/transcript_qemu.txt && git commit -m "synthui_fader_test: QEMU gate -- golden, delta equality, engagement, vsync; 3 REDs demonstrated (NEW-23)"
```

---

### Task 5: vacuity cases

**Files:**
- Modify: `tools/gate-vacuity.test.sh` (append a section before the final `exit $FAILED`)

- [ ] **Step 1: Add the three cases**

Append before `exit $FAILED` (pattern copied from section 7, the rotary bench block):

```sh
# --- 8. synthui_fader_test: green fixture passes; bad golden and a missing
# damage counter fail by name (NEW-23).  The missing-counter case is the
# spec's "absent counter must not read as proof" requirement.
FDT="examples/display/synthui_fader_test"
if [ -d "$EVKB/$FDT" ] && [ -f "$EVKB/$FDT/transcript_qemu.txt" ]; then
    run_gate "$FDT" "run_qemu.sh" "$EVKB/$FDT/transcript_qemu.txt"; rc=$?
    [ "$rc" -eq 0 ] && result=0 || result=1
    report "green_still_passes_synthui_fader_test" $result

    # A corrupted golden must fail naming the check, not pass or die silently.
    sed 's|^fd_crc=0x........|fd_crc=0xBADBADBA|' \
        "$EVKB/$FDT/transcript_qemu.txt" > "$WORK/fd_badcrc.txt"
    run_gate "$FDT" "run_qemu.sh" "$WORK/fd_badcrc.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "FAIL: bank checksum" || result=1
    report "fader_bad_golden_fails_by_name" $result

    # A capture with NO fd_damage line must fail as a missing guard --
    # an absent counter must never read as the good outcome.
    grep -v "^fd_damage " "$EVKB/$FDT/transcript_qemu.txt" > "$WORK/fd_nodmg.txt"
    run_gate "$FDT" "run_qemu.sh" "$WORK/fd_nodmg.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "delta damage guard missing" || result=1
    report "fader_missing_damage_counter_fails" $result
else
    echo "SKIP: synthui_fader_test vacuity (example or fixture missing)"
fi
```

(If the fixture's lines carry CRs, `grep -v "^fd_damage "` still matches — the prefix is CR-free. Verify the sed pattern matches by running it manually once.)

- [ ] **Step 2: Run the suite**

Run: `cd ~/Development/rt1170/evkb && ./tools/gate-vacuity.test.sh`
Expected: all prior cases still pass, plus `PASS green_still_passes_synthui_fader_test`, `PASS fader_bad_golden_fails_by_name`, `PASS fader_missing_damage_counter_fails`. Exit 0. (The suite needs the example built — it is, from Task 3.)

- [ ] **Step 3: Commit**

```bash
git add tools/gate-vacuity.test.sh && git commit -m "gate-vacuity: three synthui_fader_test cases (NEW-23)"
```

---

### Task 6: license-audit GATES entry

**Files:**
- Modify: `tools/license-audit.sh` (the GATES list, ~line 342)

- [ ] **Step 1: Add the entry**

In the GATES list, insert alphabetically (before the `synthui_knob_test` line):

```sh
examples/display/synthui_fader_test:synthui_fader_test \
```

- [ ] **Step 2: Run the audit**

Run: `cd ~/Development/rt1170/evkb && LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh`
Expected: `LICENSE-AUDIT: PASS`, with the new entry walked (a `synthui_fader_test` line reporting its dep-path count). The audit's drift check would have failed the sweep had this entry been forgotten — that is the check working, not a formality.

- [ ] **Step 3: Commit**

```bash
git add tools/license-audit.sh && git commit -m "license-audit: GATES entry for synthui_fader_test (NEW-23)"
```

---

### Task 7: full sweep (122 → 123) + CLAUDE.md arithmetic

- [ ] **Step 1: Verify the sweep symlink points at THIS tree**

Run: `readlink /tmp/ev`
If it is not `/Users/nicholasnewdigate/Development/rt1170/evkb`, create a fresh one: `ln -sfn ~/Development/rt1170/evkb /tmp/fd23` and use `/tmp/fd23` below. (The 2026-08-23 lesson: a stale symlink sweeps the wrong checkout and reports a plausible number for work that is not there.)

- [ ] **Step 2: Count first, then sweep — one sweep, output captured**

```bash
cd /tmp/ev && ./tools/run-all-qemu-gates.sh -l | tail -3
```

Expected: `(123 gate(s))` with `rt1176:display/synthui_fader_test` listed.

```bash
cd /tmp/ev && ./tools/run-all-qemu-gates.sh 2>&1 | tee /tmp/sweep-new23.log | tail -20
```

Expected: `gates: 123 passed`, exit 0 — or 122 passed + 1 failed where the failure is `rt1176:dualcore/cm4_audio_test` (the permitted nondeterministic red; re-run it idle before believing anything else) or a documented load-sensitive gate that passes idle. Note the BT bench caveat: if `m2_hci_probe[hci]` is red, check whether its build dir is still bench-configured (the 2026-08-27 class) before treating it as a regression. Any other red is caused by this work — stop and fix.

- [ ] **Step 3: Update CLAUDE.md**

In the gates paragraph: change "covers **122 gates**" to "covers **123 gates**", add one sentence after the NEW-20 gate-arithmetic block:

```
NEW-23 added ONE — `display/synthui_fader_test`, the SynthUI Fader's
sw-delta gate (bank golden, gate-compared delta equality, engagement bound,
vsync witness; spec 2026-08-29). 122 before it.
```

and update the target line "The target is **122 passed…**" to 123 (both variants). Add the measured-sweep entry at the top of the measurement chain following the existing convention (date, counts, notable gate timings, audit PASS).

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md && git commit -m "docs: sweep 123 measured -- NEW-23 fader gate added"
```

---

### Task 8: silicon checkpoint (spec section 10) — EARLY, before polish

**Files:**
- Create: `examples/display/synthui_fader_test/transcript_hw_evkb.txt` (captured)

STOP-RULE: if `fd_fps` misses 30, this task ends the plan — record the numbers in NEW-23, do NOT start a GPU TU here; that is its own brainstorm (spec section 10).

- [ ] **Step 1: Flash (VCOM free — the console reader attaches AFTER)**

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
cd ~/Development/rt1170/evkb/examples/display/synthui_fader_test
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/synthui_fader_test.elf
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build/synthui_fader_test.elf
```

Expected: load then verify succeed. If the DAP wedges (`Wire not connected` / DAPInfo errors while the VCOM still enumerates): replug the DEBUG USB — a board power cycle does NOT clear it.

- [ ] **Step 2: Attach the console, then reset by SW4**

```bash
python3 ../../../tools/rt1170-console.py /dev/cu.usbmodem* 115200 | tee transcript_hw_evkb.txt
```

Press SW4 on the board (pyocd/gdb resets cannot reboot this target — the NEW-20 bench procedure is SW4 presses with a persistent reader).
Expected within ~10 s: the full Phase A token block, then `fd_fps` after ~60 s and `fd_fps_fullinv` ~10 s later.

- [ ] **Step 3: Assert the four checkpoint facts**

1. **Bit-identity**: `fd_crc`, `fd_delta_crc`, `fd_fresh_crc` all equal the QEMU values exactly (sw-only widget — one golden set, unlike the knob's two). `fd_delta_eq=PASS`, `fd_damage max=` in band, `fd_vsync … timeouts=0`.
2. **Repeated-boot stability**: press SW4 twice more (3 boots total, reader kept attached); all three boots print identical checksums. One NEW-20 defect hid behind exactly one boot — three is the floor.
3. **The fps criterion**: `fd_fps … fps_avg=` ≥ 30 with `secs=60`. Record `fd_fps_fullinv` as the what-delta-buys baseline. Expectation from the knob's numbers: the vsync-locked pipeline measured 32.1 fps on a far heavier scene, so the fader should clear 30 with margin — but the measurement, not the expectation, is the answer.
4. **Eyes/camera**: watch the animating bank ≥60 s — no flashes, no torn caps, no stale slivers around moving caps. Checksums can never see scanout artifacts; if anything flashes, film it at 60 fps and extract frames (the NEW-20 instrument) before diagnosing.

- [ ] **Step 4: Commit the evidence**

Trim the transcript to one clean boot plus the two extra boots' checksum blocks and the fps lines, then:

```bash
cd ~/Development/rt1170/evkb && git add examples/display/synthui_fader_test/transcript_hw_evkb.txt && git commit -m "synthui_fader_test: silicon checkpoint -- goldens bit-identical to QEMU x3 boots, fd_fps recorded (NEW-23)"
```

(Put the actual fps numbers in the commit message body.)

---

### Task 9: close-out — push, pin bump, fresh-fetch verify, Linear

- [ ] **Step 1: Push SynthUI and bump the pin**

```bash
cd ~/Development/SynthUI && git push origin master && git rev-parse HEAD
```

In `evkb.cmake` line ~131, replace the SynthUI SHA with the new HEAD and append to the comment: `Bumped 2026-08-29 (synthui_fader widget + drag math, NEW-23).`

- [ ] **Step 2: Fresh-user verify — run the GATE against a fetched-source ELF**

A configure only proves the subdir resolves; only a gate run proves the fetched code behaves (the BT-1 rule):

```bash
cd ~/Development/rt1170/evkb/examples/display/synthui_fader_test
cmake -B build-fetch -DEVKB_FORCE_FETCH=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build-fetch
mv build build-local && ln -s build-fetch build
./run_qemu.sh; RC=$?
rm build && mv build-local build
[ $RC -eq 0 ] && echo FETCH-VERIFY-PASS
```

Expected: `PASS: SynthUI fader render verified` from the GitHub-fetched SynthUI, then `FETCH-VERIFY-PASS`. Remove `build-fetch` afterwards.

- [ ] **Step 3: Final green trio**

Run the sweep (`cd /tmp/ev && ./tools/run-all-qemu-gates.sh 2>&1 | tee /tmp/sweep-new23-final.log | tail -5`), the vacuity suite, and the license audit once more on the final tree.
Expected: `gates: 123 passed` (modulo the documented cm4_audio_test nondeterminism), vacuity all-PASS, `LICENSE-AUDIT: PASS`.

- [ ] **Step 4: Commit + push evkb**

```bash
cd ~/Development/rt1170/evkb && git add evkb.cmake && git commit -m "evkb.cmake: SynthUI pin bump (fader widget, NEW-23); fresh-fetch gate-run verified" && git push
```

- [ ] **Step 5: Update Linear NEW-23**

Comment with: the QEMU golden, the silicon `fd_fps` / `fd_fps_fullinv` numbers, the three-boot bit-identity, the camera-pass result, sweep 123, and the pin SHA. Move the issue to Done if every success criterion is met; if the fps checkpoint failed, leave it In Progress with the numbers and the escalation note (GPU TU brainstorm per spec section 10).

---

## Plan self-review notes

- Spec coverage: §3–§8 → Tasks 1–2; §9 → Tasks 3–5; audit/bookkeeping → Tasks 6–7; §10 → Task 8; §13 close-out → Task 9; §12 README touch → Task 2 Step 5. The §9 engagement bound is deliberately finalized from Task 3's measurement (the spec's "measured green run plus headroom").
- The gate's `fd_crc` placeholder `0xXXXXXXXX` is the ONE value that cannot be pre-written (goldens are recorded, never derived); Task 4 Step 1 names exactly where it comes from.
- Token names match between sketch, gate, and vacuity cases: `fd_crc`, `fd_delta_crc`, `fd_fresh_crc`, `fd_damage max=`, `fd_vsync`, `crc_done`.
