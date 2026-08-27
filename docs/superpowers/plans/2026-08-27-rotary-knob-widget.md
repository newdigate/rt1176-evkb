# synthui_rotary_knob (NEW-20 Phase 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the production RotaryKnob widget (`synthui_rotary_knob`, notch
variant, vector/gpu with sw fallback) in SynthUI and replace the old knob in
its three real consumers, re-goldened end to end.

**Architecture:** Widget core is a pure-LVGL custom widget (well + rotor sw
drawing, old knob's input layer verbatim); an opt-in GPU compositor TU (three
cached `vg_lite` paths, RENDER_READY pass, one finish per refresh) connects
through one bool + one instance list declared in a private header, so the core
never references GPU symbols. Palette is a pure header shared by sw, gpu and a
host test. Spec: `docs/superpowers/specs/2026-08-27-rotary-knob-widget-design.md`.

**Tech Stack:** LVGL 9.4 (sw renderer only), NXP VGLite on GC355 via direct
calls, SynthUI sibling repo, QEMU gates + FNV-1a goldens, LinkServer/EVKB.

**Repos:** `~/Development/SynthUI` (widget) and this repo (`~/Development/rt1170/evkb`,
branch `nicnewdigate/new-20-phase2-rotary-knob-widget`). SynthUI resolves
local-first, so SynthUI edits are visible to evkb builds immediately; the pin
bump happens at the end (Task 10).

---

### Task 1: Palette header + host test (SynthUI)

**Files:**
- Create: `~/Development/SynthUI/src/synthui_rotary_palette.h`
- Create: `~/Development/SynthUI/tests/rotary_palette_test.c`
- Modify: `~/Development/SynthUI/tests/run.sh`

- [ ] **Step 1: Write the failing test** — `tests/rotary_palette_test.c`:

```c
/* rotary_palette_test.c - pins the RotaryKnob.dc.html THEME/state mapping.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include "../src/synthui_rotary_palette.h"

static int fails = 0;
#define CHECK(what, got, want) do { \
    if ((got) != (want)) { \
        printf("FAIL %s: got %06x want %06x\n", what, (unsigned)(got), (unsigned)(want)); \
        fails++; \
    } } while (0)

/* One row of the DC table: theme, state flags, accent in; five hexes out. */
static void row(const char *name, int dark, int dis, int act, int foc,
                uint32_t accent, uint32_t well, uint32_t stroke,
                uint32_t body, uint32_t inner, uint32_t index_)
{
    synthui_rotary_palette_t p;
    synthui_rotary_palette(dark, dis, act, foc, accent, &p);
    char buf[64];
    snprintf(buf, sizeof buf, "%s well", name);   CHECK(buf, p.well, well);
    snprintf(buf, sizeof buf, "%s stroke", name); CHECK(buf, p.well_stroke, stroke);
    snprintf(buf, sizeof buf, "%s body", name);   CHECK(buf, p.body, body);
    snprintf(buf, sizeof buf, "%s inner", name);  CHECK(buf, p.inner, inner);
    snprintf(buf, sizeof buf, "%s index", name);  CHECK(buf, p.index, index_);
}

int main(void)
{
    const uint32_t N = SYNTHUI_ROTARY_ACCENT_DEFAULT;
    /* light */
    row("l/idle",     0,0,0,0, N, 0xdcdce6,0xb6b8cc, 0x282b60,0x333871,0xfcfbf6);
    row("l/active",   0,0,1,0, N, 0xdcdce6,0xb6b8cc, 0x31356f,0x3d4283,0xfcfbf6);
    row("l/focus",    0,0,0,1, N, 0xdcdce6,0xfcfbf6, 0x282b60,0x333871,0xfcfbf6);
    row("l/disabled", 0,1,0,0, N, 0xe4e4ea,0xb6b8cc, 0x9a9cae,0xa6a8b8,0xdcdce6);
    row("l/accent",   0,0,0,0, 0xffd24a, 0xdcdce6,0xb6b8cc, 0x282b60,0x333871,0xffd24a);
    /* accent is IGNORED when disabled (renderVals: off ? indexOff : accent) */
    row("l/acc+dis",  0,1,0,0, 0xffd24a, 0xe4e4ea,0xb6b8cc, 0x9a9cae,0xa6a8b8,0xdcdce6);
    /* focus ring is the THEME index, not the accent (renderVals: base.index) */
    row("l/acc+foc",  0,0,0,1, 0x5be0a0, 0xdcdce6,0xfcfbf6, 0x282b60,0x333871,0x5be0a0);
    /* dark */
    row("d/idle",     1,0,0,0, N, 0x14141c,0x34344a, 0x3c4176,0x4a5090,0xffd24a);
    row("d/active",   1,0,1,0, N, 0x14141c,0x34344a, 0x464c88,0x565da4,0xffd24a);
    row("d/focus",    1,0,0,1, N, 0x14141c,0xffd24a, 0x3c4176,0x4a5090,0xffd24a);
    row("d/disabled", 1,1,0,0, N, 0x101016,0x34344a, 0x2a2a36,0x32323f,0x55555f);
    /* disabled wins over active AND focus (DC state is exclusive; LVGL is not) */
    row("l/dis+all",  0,1,1,1, 0xff6a52, 0xe4e4ea,0xb6b8cc, 0x9a9cae,0xa6a8b8,0xdcdce6);
    if (fails) { printf("%d FAILURES\n", fails); return 1; }
    printf("rotary_palette_test: all rows match the DC table\n");
    return 0;
}
```

- [ ] **Step 2: Add it to the runner** — `tests/run.sh`, after the
  knob_math lines append:

```sh
cc -Wall -Wextra -Werror -o "$out/rotary_palette_test" tests/rotary_palette_test.c
"$out/rotary_palette_test"
```

- [ ] **Step 3: Run to verify it fails** —
  `cd ~/Development/SynthUI && tests/run.sh` — expected: cc error, no such
  file `synthui_rotary_palette.h`.

- [ ] **Step 4: Write the header** — `src/synthui_rotary_palette.h`:

```c
/* synthui_rotary_palette.h - RotaryKnob.dc.html THEME/state mapping, pure.
 * LVGL-free and header-only so a host compiler can unit-test it and both the
 * sw draw and the GPU compositor share one source of truth.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_ROTARY_PALETTE_H
#define SYNTHUI_ROTARY_PALETTE_H
#include <stdint.h>

/* set_accent() sentinel: "no accent, use the theme's index color". */
#define SYNTHUI_ROTARY_ACCENT_DEFAULT 0xFFFFFFFFu

typedef struct {
    uint32_t well, well_stroke, body, inner, index;
} synthui_rotary_palette_t;

/* Hexes verbatim from RotaryKnob.dc.html renderVals() (fetched 2026-08-27).
 * disabled wins; active/focus apply only when not disabled (the DC state enum
 * is exclusive, LVGL states are not -- same resolution the old knob used).
 * Note two DC subtleties this preserves: accent is IGNORED when disabled
 * (index -> indexOff), and the focus ring is the THEME index color, never the
 * accent (renderVals uses base.index for wellStroke). */
static inline void synthui_rotary_palette(int dark, int disabled, int active,
                                          int focus, uint32_t accent,
                                          synthui_rotary_palette_t *p)
{
    if (disabled) { active = 0; focus = 0; }
    if (!dark) {
        p->well        = disabled ? 0xe4e4eau : 0xdcdce6u;
        p->well_stroke = focus    ? 0xfcfbf6u : 0xb6b8ccu;
        p->body        = disabled ? 0x9a9caeu : (active ? 0x31356fu : 0x282b60u);
        p->inner       = disabled ? 0xa6a8b8u : (active ? 0x3d4283u : 0x333871u);
        p->index       = disabled ? 0xdcdce6u
                                  : (accent != SYNTHUI_ROTARY_ACCENT_DEFAULT
                                         ? accent : 0xfcfbf6u);
    } else {
        p->well        = disabled ? 0x101016u : 0x14141cu;
        p->well_stroke = focus    ? 0xffd24au : 0x34344au;
        p->body        = disabled ? 0x2a2a36u : (active ? 0x464c88u : 0x3c4176u);
        p->inner       = disabled ? 0x32323fu : (active ? 0x565da4u : 0x4a5090u);
        p->index       = disabled ? 0x55555fu
                                  : (accent != SYNTHUI_ROTARY_ACCENT_DEFAULT
                                         ? accent : 0xffd24au);
    }
}
#endif /* SYNTHUI_ROTARY_PALETTE_H */
```

- [ ] **Step 5: Run tests to verify pass** — `tests/run.sh` → both binaries
  run, `rotary_palette_test: all rows match the DC table`.

- [ ] **Step 6: Commit (SynthUI)** —
  `git add src/synthui_rotary_palette.h tests/rotary_palette_test.c tests/run.sh && git commit -m "rotary: palette header pinning the DC THEME/state table"`

### Task 2: Widget core (SynthUI)

**Files:**
- Create: `~/Development/SynthUI/src/synthui_rotary_knob.h` (public API — §3 of the spec, verbatim)
- Create: `~/Development/SynthUI/src/synthui_rotary_knob_private.h`
- Create: `~/Development/SynthUI/src/synthui_rotary_knob.cpp`

- [ ] **Step 1: Public header** — `src/synthui_rotary_knob.h`:

```c
/* synthui_rotary_knob.h - SynthUI RotaryKnob (notch), LVGL 9 custom widget.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_ROTARY_KNOB_H
#define SYNTHUI_ROTARY_KNOB_H

#include <lvgl.h>
#include "synthui_rotary_palette.h"   /* SYNTHUI_ROTARY_ACCENT_DEFAULT */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNTHUI_ROTARY_MODE_ENDLESS = 0,   /* well ring */
    SYNTHUI_ROTARY_MODE_BOUNDED,       /* well disc + min..max arc track */
} synthui_rotary_mode_t;

typedef enum {
    SYNTHUI_ROTARY_THEME_LIGHT = 0,
    SYNTHUI_ROTARY_THEME_DARK,
} synthui_rotary_theme_t;

extern const lv_obj_class_t synthui_rotary_knob_class;

lv_obj_t *synthui_rotary_knob_create(lv_obj_t *parent);

/* Programmatic state changes (lv_obj_add_state/remove_state) need a manual
 * lv_obj_invalidate(): no local styles, so LVGL's style-diff repaint never
 * fires (same contract as the old knob). The input layer (vertical drag,
 * 200 px = full sweep, unsnapped accumulator) emits LV_EVENT_VALUE_CHANGED. */
void  synthui_rotary_knob_set_angle(lv_obj_t *obj, float deg);       /* default 0 */
void  synthui_rotary_knob_set_mode(lv_obj_t *obj, synthui_rotary_mode_t m);
void  synthui_rotary_knob_set_theme(lv_obj_t *obj, synthui_rotary_theme_t t);
/* rgb hex (0xRRGGBB); SYNTHUI_ROTARY_ACCENT_DEFAULT reverts to theme index. */
void  synthui_rotary_knob_set_accent(lv_obj_t *obj, uint32_t rgb_hex);
void  synthui_rotary_knob_set_range(lv_obj_t *obj, float min_deg, float max_deg); /* default -150..150 */
void  synthui_rotary_knob_set_detent_step(lv_obj_t *obj, float deg); /* <=0 = continuous (default 0) */
float synthui_rotary_knob_get_angle(const lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Private header** — `src/synthui_rotary_knob_private.h`:

```c
/* synthui_rotary_knob_private.h - shared between the widget core and the
 * optional GPU compositor TU (src/vglite/). NOT part of the public API.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_ROTARY_KNOB_PRIVATE_H
#define SYNTHUI_ROTARY_KNOB_PRIVATE_H

#include "synthui_rotary_knob.h"
/* lv_obj_t by value needs the complete private type (old knob's note). */
#include <lvgl_private.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct synthui_rotary_knob_t {
    lv_obj_t obj;
    float angle, min_deg, max_deg, detent_step;
    float drag_pos;               /* unsnapped accumulator (knob_math) */
    uint32_t accent;              /* SYNTHUI_ROTARY_ACCENT_DEFAULT = none */
    synthui_rotary_mode_t  mode;
    synthui_rotary_theme_t theme;
    /* Set by DRAW_MAIN when the GPU hook is enabled: "my well was painted
     * this frame, my rotor still needs compositing". Cleared by the
     * compositor. Never set for hidden/other-screen objects, because
     * DRAW_MAIN does not run for them. */
    bool gpu_pending;
    struct synthui_rotary_knob_t *prev, *next;   /* instance registry */
} synthui_rotary_knob_t;

/* Registry of live instances (constructor links, destructor unlinks) and the
 * one flag the GPU TU flips on successful synthui_rotary_gpu_begin(). Both
 * are defined in the core so the core never references GPU symbols; a build
 * without src/vglite/ simply leaves the flag false forever. */
extern synthui_rotary_knob_t *synthui_rotary_knob_list;
extern bool synthui_rotary_gpu_enabled;

/* Resolve the palette for one instance from its LVGL state (shared by the sw
 * draw and the compositor so the two engines cannot disagree on color). */
void synthui_rotary_knob_palette(const synthui_rotary_knob_t *k,
                                 synthui_rotary_palette_t *p);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 3: Core implementation** — `src/synthui_rotary_knob.cpp`.
  Structure mirrors `synthui_knob.cpp` (constructor/event/input split); the
  drawing is the DC §2 contract; the arc fold is the old knob's
  `draw_arc_seg` with a `rounded` parameter added:

```cpp
/* synthui_rotary_knob.cpp - SynthUI RotaryKnob (notch), LVGL 9 custom widget.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Clean-room port of RotaryKnob.dc.html renderVals() (design project
 * 79ec272e, fetched 2026-08-27): 0..100 viewBox scaled to min(w,h), 0 deg =
 * 12 o'clock, clockwise. Notch variant only (NEW-20 Phase-2 scope decision);
 * geometry identical to the bench's rk_geometry, whose silicon numbers chose
 * the render strategy. Input layer carried over from synthui_knob verbatim.
 *
 * SVG-vs-LVGL divergence, accepted and golden-absorbed (bench §6): SVG
 * strokes straddle the radius, LVGL borders sit inside it; the r43-centred
 * track is drawn as outer radius 44.5, width 3. */
#include "synthui_rotary_knob.h"
#include "synthui_rotary_knob_private.h"
#include "synthui_knob_math.h"
#include <math.h>

#define RK_DEG (3.14159265358979f / 180.0f)
#define MY_CLASS (&synthui_rotary_knob_class)

synthui_rotary_knob_t *synthui_rotary_knob_list = NULL;
bool synthui_rotary_gpu_enabled = false;

static void rk_constructor(const lv_obj_class_t *cls, lv_obj_t *obj);
static void rk_destructor(const lv_obj_class_t *cls, lv_obj_t *obj);
static void rk_event(const lv_obj_class_t *cls, lv_event_t *e);
static void rk_input_pressed(lv_event_t *e);
static void rk_input_pressing(lv_event_t *e);
static void rk_input_state(lv_event_t *e);

const lv_obj_class_t synthui_rotary_knob_class = {
    .base_class     = &lv_obj_class,
    .constructor_cb = rk_constructor,
    .destructor_cb  = rk_destructor,
    .event_cb       = rk_event,
    /* designators must follow lv_obj_class_private.h declaration order --
     * name declares before width_def (the old knob's note). */
    .name           = "synthui_rotary_knob",
    .width_def      = 120,
    .height_def     = 120,
    .instance_size  = sizeof(synthui_rotary_knob_t),
};

lv_obj_t *synthui_rotary_knob_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_class_create_obj(&synthui_rotary_knob_class, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

static void rk_constructor(const lv_obj_class_t *cls, lv_obj_t *obj)
{
    LV_UNUSED(cls);
    synthui_rotary_knob_t *k = (synthui_rotary_knob_t *)obj;
    k->angle = 0.0f;
    k->min_deg = -150.0f; k->max_deg = 150.0f;   /* DC defaults */
    k->detent_step = 0.0f;
    k->drag_pos = 0.0f;
    k->accent = SYNTHUI_ROTARY_ACCENT_DEFAULT;
    k->mode = SYNTHUI_ROTARY_MODE_ENDLESS;
    k->theme = SYNTHUI_ROTARY_THEME_LIGHT;
    k->gpu_pending = false;
    /* head-insert into the registry */
    k->prev = NULL;
    k->next = synthui_rotary_knob_list;
    if (k->next) k->next->prev = k;
    synthui_rotary_knob_list = k;
    /* same scroll rationale as the old knob (and lv_slider) */
    lv_obj_remove_flag(obj, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                            LV_OBJ_FLAG_SCROLL_CHAIN_VER));
    lv_obj_add_event_cb(obj, rk_input_pressed,  LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(obj, rk_input_pressing, LV_EVENT_PRESSING,   NULL);
    lv_obj_add_event_cb(obj, rk_input_state,    LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(obj, rk_input_state,    LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(obj, rk_input_state,    LV_EVENT_PRESS_LOST, NULL);
}

static void rk_destructor(const lv_obj_class_t *cls, lv_obj_t *obj)
{
    LV_UNUSED(cls);
    synthui_rotary_knob_t *k = (synthui_rotary_knob_t *)obj;
    if (k->prev) k->prev->next = k->next;
    else synthui_rotary_knob_list = k->next;
    if (k->next) k->next->prev = k->prev;
    k->prev = k->next = NULL;
}

#define RK_SETTER(obj, field, val) do { \
    synthui_rotary_knob_t *k = (synthui_rotary_knob_t *)obj; \
    if (k->field == (val)) return; \
    k->field = (val); \
    lv_obj_invalidate(obj); } while (0)

void synthui_rotary_knob_set_angle(lv_obj_t *obj, float deg)
{ LV_ASSERT_OBJ(obj, MY_CLASS); RK_SETTER(obj, angle, deg); }
void synthui_rotary_knob_set_mode(lv_obj_t *obj, synthui_rotary_mode_t m)
{ LV_ASSERT_OBJ(obj, MY_CLASS); RK_SETTER(obj, mode, m); }
void synthui_rotary_knob_set_theme(lv_obj_t *obj, synthui_rotary_theme_t t)
{ LV_ASSERT_OBJ(obj, MY_CLASS); RK_SETTER(obj, theme, t); }
void synthui_rotary_knob_set_accent(lv_obj_t *obj, uint32_t rgb_hex)
{ LV_ASSERT_OBJ(obj, MY_CLASS); RK_SETTER(obj, accent, rgb_hex); }
void synthui_rotary_knob_set_detent_step(lv_obj_t *obj, float deg)
{ LV_ASSERT_OBJ(obj, MY_CLASS); RK_SETTER(obj, detent_step, deg < 0.0f ? 0.0f : deg); }
void synthui_rotary_knob_set_range(lv_obj_t *obj, float min_deg, float max_deg)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    synthui_rotary_knob_t *k = (synthui_rotary_knob_t *)obj;
    if (k->min_deg == min_deg && k->max_deg == max_deg) return;
    k->min_deg = min_deg; k->max_deg = max_deg;
    lv_obj_invalidate(obj);
}
float synthui_rotary_knob_get_angle(const lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    return ((const synthui_rotary_knob_t *)obj)->angle;
}

void synthui_rotary_knob_palette(const synthui_rotary_knob_t *k,
                                 synthui_rotary_palette_t *p)
{
    const lv_state_t st = lv_obj_get_state((const lv_obj_t *)&k->obj);
    synthui_rotary_palette(k->theme == SYNTHUI_ROTARY_THEME_DARK,
                           (st & LV_STATE_DISABLED) != 0,
                           (st & LV_STATE_PRESSED)  != 0,
                           (st & LV_STATE_FOCUSED)  != 0,
                           k->accent, p);
}

/* ---- input layer: verbatim port of synthui_knob's ---- */
static void rk_input_pressed(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    synthui_rotary_knob_t *k = (synthui_rotary_knob_t *)obj;
    k->drag_pos = k->angle;   /* seed from where the knob points NOW */
}

static void rk_input_pressing(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == NULL) return;
    lv_point_t vect;
    lv_indev_get_vect(indev, &vect);
    if (vect.y == 0) return;

    synthui_rotary_knob_t *k = (synthui_rotary_knob_t *)obj;
    k->drag_pos = synthui_knob_drag(k->drag_pos, k->min_deg, k->max_deg, vect.y);
    /* detent snap applies whenever a step is set (the old DETENTS mode is a
     * behavior here, not a visual mode); the accumulator stays unsnapped */
    const float next = synthui_knob_snap(k->drag_pos, k->min_deg, k->max_deg,
                                         k->detent_step);
    if (next == k->angle) return;
    synthui_rotary_knob_set_angle(obj, next);
    lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL);
}

static void rk_input_state(lv_event_t *e)
{   /* palette changes on PRESSED/RELEASED; no local styles -> manual repaint */
    lv_obj_invalidate(lv_event_get_current_target_obj(e));
}

/* ---- drawing ---- */
static void polar(float cx, float cy, float S, float r, float deg,
                  float *x, float *y)
{
    *x = cx + r * S * sinf(deg * RK_DEG);
    *y = cy - r * S * cosf(deg * RK_DEG);
}

static void draw_disc(lv_layer_t *l, float x, float y, float rpx,
                      const lv_draw_rect_dsc_t *dsc)
{
    lv_area_t a = { (int32_t)lroundf(x - rpx), (int32_t)lroundf(y - rpx),
                    (int32_t)lroundf(x + rpx), (int32_t)lroundf(y + rpx) };
    lv_draw_rect(l, dsc, &a);
}

/* Annulus sector, radius names the OUTER edge, width extends inward. The
 * fold is the old knob's lesson: LVGL's sw arc clamps a negative start to 0
 * and renders a truncated wedge, so fold the start into [0,360) and carry
 * the span. */
static void draw_arc_seg(lv_layer_t *layer, float cx, float cy, float S,
                         float r_outer, float w, float a1, float a2,
                         uint32_t hex, bool rounded)
{
    lv_draw_arc_dsc_t a; lv_draw_arc_dsc_init(&a);
    a.center.x = (int32_t)lroundf(cx); a.center.y = (int32_t)lroundf(cy);
    a.radius = (uint16_t)lroundf(r_outer * S);
    a.width  = (int32_t)lroundf(w * S); if (a.width < 1) a.width = 1;
    float span = a2 - a1;
    if (span <= 0.0f) return;
    if (span > 360.0f) span = 360.0f;
    float s0 = fmodf(a1 - 90.0f, 360.0f);   /* LVGL measures from 3 o'clock */
    if (s0 < 0.0f) s0 += 360.0f;
    a.start_angle = (lv_value_precise_t)s0;
    a.end_angle   = (lv_value_precise_t)(s0 + span);
    a.color = lv_color_hex(hex); a.opa = LV_OPA_COVER;
    a.rounded = rounded ? 1 : 0;
    lv_draw_arc(layer, &a);
}

static void rk_draw(synthui_rotary_knob_t *k, lv_layer_t *layer)
{
    lv_obj_t *obj = &k->obj;
    lv_area_t coords; lv_obj_get_coords(obj, &coords);
    const float W = (float)lv_area_get_width(&coords);
    const float H = (float)lv_area_get_height(&coords);
    const float S = (W < H ? W : H) / 100.0f;
    const float cx = (float)coords.x1 + W * 0.5f;
    const float cy = (float)coords.y1 + H * 0.5f;

    synthui_rotary_palette_t pal;
    synthui_rotary_knob_palette(k, &pal);
    const bool focus = (lv_obj_get_state(obj) & LV_STATE_FOCUSED) &&
                       !(lv_obj_get_state(obj) & LV_STATE_DISABLED);

    /* well: r39 disc; ring mode strokes it (focus: index color, w3),
     * bounded mode leaves it strokeless and adds the r43 track */
    {
        lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
        d.radius = LV_RADIUS_CIRCLE;
        d.bg_color = lv_color_hex(pal.well); d.bg_opa = LV_OPA_COVER;
        if (k->mode != SYNTHUI_ROTARY_MODE_BOUNDED) {
            d.border_color = lv_color_hex(pal.well_stroke);
            d.border_opa = LV_OPA_COVER;
            d.border_width = (int32_t)lroundf((focus ? 3.0f : 1.6f) * S);
            if (d.border_width < 1) d.border_width = 1;
        }
        draw_disc(layer, cx, cy, 39.0f * S, &d);
    }
    if (k->mode == SYNTHUI_ROTARY_MODE_BOUNDED)
        draw_arc_seg(layer, cx, cy, S, 44.5f, 3.0f, k->min_deg, k->max_deg,
                     pal.well_stroke, true);

    /* rotor: GPU-composited post-render when the hook is live */
    if (synthui_rotary_gpu_enabled) { k->gpu_pending = true; return; }

    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE; d.bg_opa = LV_OPA_COVER;
    d.bg_color = lv_color_hex(pal.body);
    draw_disc(layer, cx, cy, 36.0f * S, &d);
    d.bg_color = lv_color_hex(pal.inner);
    draw_disc(layer, cx, cy, 27.0f * S, &d);
    draw_arc_seg(layer, cx, cy, S, 36.0f, 20.0f,
                 k->angle - 8.0f, k->angle + 8.0f, pal.index, false);
    (void)polar;   /* kept for variants Phase 3 adds; silence -Wunused */
}

static void rk_event(const lv_obj_class_t *cls, lv_event_t *e)
{
    LV_UNUSED(cls);
    if (lv_obj_event_base(MY_CLASS, e) != LV_RESULT_OK) return;
    if (lv_event_get_code(e) == LV_EVENT_DRAW_MAIN)
        rk_draw((synthui_rotary_knob_t *)lv_event_get_current_target_obj(e),
                lv_event_get_layer(e));
}
```

  ★ Before committing, check `lv_obj_class` designator order against
  `lv_obj_class_private.h` in the vendored LVGL (destructor_cb's position) and
  that `lv_draw_arc_dsc_t` has a `rounded` field in 9.4; adjust to match.
  ★ Drop the `polar()` helper entirely if nothing references it — the
  `(void)polar` cast is only acceptable if a variant needs it this phase
  (it does not; delete both).

- [ ] **Step 4: Compile-check via the smallest SynthUI consumer** —
  `cd ~/Development/rt1170/evkb/examples/display/synthui_step_test && cmake --build build 2>&1 | tail -5`
  (the SynthUI glob picks the new core up). Expected: clean build.

- [ ] **Step 5: Run SynthUI host tests still green** — `tests/run.sh`.

- [ ] **Step 6: Commit (SynthUI)** —
  `git add src/synthui_rotary_knob.h src/synthui_rotary_knob_private.h src/synthui_rotary_knob.cpp && git commit -m "rotary: widget core -- DC notch geometry, old knob's input layer"`

### Task 3: GPU compositor TU (SynthUI) + CMake flavor (evkb)

**Files:**
- Create: `~/Development/SynthUI/src/vglite/synthui_rotary_knob_gpu.h`
- Create: `~/Development/SynthUI/src/vglite/synthui_rotary_knob_gpu.cpp`
- Modify: `~/Development/rt1170/evkb/evkb.cmake` (import_evkb_synthui)

- [ ] **Step 1: GPU public header** — `src/vglite/synthui_rotary_knob_gpu.h`:

```c
/* synthui_rotary_knob_gpu.h - opt-in GC355 rotor compositor for
 * synthui_rotary_knob. Compiled only by import_evkb_synthui(VGLITE); without
 * it the widget is fully software and this header must not be included.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_ROTARY_KNOB_GPU_H
#define SYNTHUI_ROTARY_KNOB_GPU_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call AFTER vg_lite_init() succeeded (app owns the probe -- vg_lite_init
 * SPINS on absent hardware, so gate it on the chip-ID read) and AFTER the
 * LVGL display exists. Wraps + maps the framebuffer as the vg_lite target,
 * builds the notch path set once, hooks LV_EVENT_RENDER_READY, and switches
 * every synthui_rotary_knob to well-sw/rotor-gpu drawing. Returns false --
 * and changes nothing -- on any failure. */
bool synthui_rotary_gpu_begin(void *framebuffer, int32_t w, int32_t h,
                              int32_t stride_bytes);

/* Cumulative count of vg_lite_* calls that did not return VG_LITE_SUCCESS.
 * A rejected draw paints nothing while everything else looks healthy, so
 * examples must print this and hardware transcripts must show 0. */
uint32_t synthui_rotary_gpu_errors(void);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Compositor implementation** —
  `src/vglite/synthui_rotary_knob_gpu.cpp`. The path build is the bench's
  `rk_geometry` vg-path emitter reduced to the notch set; the pass is the
  bench's `gpu_rotor_pass` generalized over the registry:

```cpp
/* synthui_rotary_knob_gpu.cpp - see synthui_rotary_knob_gpu.h.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * The bench's proven ordering (rotary_knob_bench, silicon 2026-08-27):
 * LVGL renders every damaged area in software (the widget paints its WELL
 * there), then LV_EVENT_RENDER_READY -- sent from refr_invalid_areas, which
 * structurally cannot fire on an empty refresh -- composites each pending
 * rotor straight onto the framebuffer and retires with ONE vg_lite_finish.
 * Paths are built once in centred viewBox units x16 (S32); per frame only
 * translate(center)*rotate(angle)*scale(S/16) changes. No blits anywhere, so
 * the GC355's 64-byte source-stride rule does not apply to this TU.
 * No D-cache maintenance, deliberately: the imxrt1176 core never enables the
 * D-cache (lvgl_mipi_panel.cpp's invariant). An rt1062 port makes this site
 * and that flush ONE change, not two. */
#include "synthui_rotary_knob_gpu.h"
#include "../synthui_rotary_knob_private.h"
#include <math.h>
#include <string.h>

extern "C" {
#include "vg_lite.h"
}

#define RK_DEG (3.14159265358979f / 180.0f)
#define RK_FIX 16.0f

static vg_lite_buffer_t s_target;
static vg_lite_path_t   s_paths[3];      /* body, inner, index wedge */
static bool             s_begun = false;
static uint32_t         s_err = 0;
#define GPU_TRY(call) do { if ((call) != VG_LITE_SUCCESS) s_err++; } while (0)

/* ---- notch path build (bench rk_geometry, reduced) ---- */
#define RK_ARENA_WORDS 256
static int32_t s_arena[RK_ARENA_WORDS];
static size_t  s_used;
static bool    s_overflow;

static void emit(int32_t w)
{
    if (s_used < RK_ARENA_WORDS) s_arena[s_used++] = w;
    else s_overflow = true;
}
static int32_t fx(float f) { return (int32_t)lroundf(f * RK_FIX); }
static void cpol(float r, float deg, float *x, float *y)
{
    *x = r * sinf(deg * RK_DEG);
    *y = -r * cosf(deg * RK_DEG);
}
/* cubics for the arc r, a1 -> a2; current point already at (r, a1).
 * k = (4/3)tan(step/4); a negative span flips sign via tan, so the ring's
 * reversed inner arc needs no special case. */
static void emit_arc(float r, float a1, float a2)
{
    const float span = a2 - a1;
    int nseg = (int)ceilf(fabsf(span) / 90.0f);
    if (nseg < 1) nseg = 1;
    const float step = span / (float)nseg;
    const float d = (4.0f / 3.0f) * tanf(step * RK_DEG / 4.0f) * r;
    for (int i = 0; i < nseg; i++) {
        const float b1 = a1 + (float)i * step, b2 = b1 + step;
        float x1, y1, x2, y2;
        cpol(r, b1, &x1, &y1);
        cpol(r, b2, &x2, &y2);
        emit(VLC_OP_CUBIC);
        emit(fx(x1 + d * cosf(b1 * RK_DEG))); emit(fx(y1 + d * sinf(b1 * RK_DEG)));
        emit(fx(x2 - d * cosf(b2 * RK_DEG))); emit(fx(y2 - d * sinf(b2 * RK_DEG)));
        emit(fx(x2)); emit(fx(y2));
    }
}
static void emit_circle(float r)
{
    float x, y;
    cpol(r, 0.0f, &x, &y);
    emit(VLC_OP_MOVE); emit(fx(x)); emit(fx(y));
    emit_arc(r, 0.0f, 360.0f);
    emit(VLC_OP_CLOSE);
}
static void emit_ring(float r0, float r1, float a1, float a2)
{
    float x, y;
    cpol(r1, a1, &x, &y);
    emit(VLC_OP_MOVE); emit(fx(x)); emit(fx(y));
    emit_arc(r1, a1, a2);
    cpol(r0, a2, &x, &y);
    emit(VLC_OP_LINE); emit(fx(x)); emit(fx(y));
    emit_arc(r0, a2, a1);               /* reversed inner edge closes it */
    emit(VLC_OP_CLOSE);
}
static void finish_path(vg_lite_path_t *p, size_t start)
{
    emit(VLC_OP_END);
    memset(p, 0, sizeof(*p));
    vg_lite_init_path(p, VG_LITE_S32, VG_LITE_HIGH,
                      (uint32_t)((s_used - start) * sizeof(int32_t)),
                      &s_arena[start],
                      -41.0f * RK_FIX, -41.0f * RK_FIX,
                      41.0f * RK_FIX, 41.0f * RK_FIX);
}
static bool build_paths(void)
{
    s_used = 0; s_overflow = false;
    size_t start;
    start = s_used; emit_circle(36.0f);                 finish_path(&s_paths[0], start);
    start = s_used; emit_circle(27.0f);                 finish_path(&s_paths[1], start);
    start = s_used; emit_ring(16.0f, 36.0f, -8.0f, 8.0f); finish_path(&s_paths[2], start);
    /* a truncated path set is a WRONG picture that still draws (bench rule) */
    return !s_overflow;
}

/* vg_lite_color_t is ABGR -- red in the LOW byte (vglite_probe, measured). */
static uint32_t abgr(uint32_t hex)
{
    return 0xFF000000u | ((hex & 0xFFu) << 16) | (hex & 0xFF00u)
           | ((hex >> 16) & 0xFFu);
}

static void render_ready_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    bool drew = false;
    for (synthui_rotary_knob_t *k = synthui_rotary_knob_list; k; k = k->next) {
        if (!k->gpu_pending) continue;
        k->gpu_pending = false;
        lv_area_t coords; lv_obj_get_coords(&k->obj, &coords);
        const float W = (float)lv_area_get_width(&coords);
        const float H = (float)lv_area_get_height(&coords);
        const float S = (W < H ? W : H) / 100.0f;
        synthui_rotary_palette_t pal;
        synthui_rotary_knob_palette(k, &pal);
        const uint32_t col[3] = { abgr(pal.body), abgr(pal.inner),
                                  abgr(pal.index) };
        vg_lite_matrix_t m; vg_lite_identity(&m);
        vg_lite_translate((float)coords.x1 + W * 0.5f,
                          (float)coords.y1 + H * 0.5f, &m);
        vg_lite_rotate(k->angle, &m);
        vg_lite_scale(S / RK_FIX, S / RK_FIX, &m);
        for (int p = 0; p < 3; p++)
            GPU_TRY(vg_lite_draw(&s_target, &s_paths[p], VG_LITE_FILL_NON_ZERO,
                                 &m, VG_LITE_BLEND_SRC_OVER, col[p]));
        drew = true;
    }
    /* Retire before anyone (flush already happened; checksums, scanout)
     * reads the framebuffer -- checksumming earlier races the hardware. */
    if (drew) GPU_TRY(vg_lite_finish());
}

bool synthui_rotary_gpu_begin(void *framebuffer, int32_t w, int32_t h,
                              int32_t stride_bytes)
{
    if (s_begun) return true;
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL || framebuffer == NULL) return false;
    if (!build_paths()) return false;
    memset(&s_target, 0, sizeof(s_target));
    s_target.width   = w;
    s_target.height  = h;
    s_target.stride  = stride_bytes;
    s_target.tiled   = VG_LITE_LINEAR;
    s_target.format  = VG_LITE_BGRA8888;   /* = panel XRGB8888 memory order */
    s_target.memory  = framebuffer;
    s_target.address = (uint32_t)(uintptr_t)framebuffer;
    /* REGISTER the target with the driver or every draw "succeeds" and
     * changes nothing (vglite_probe, measured on silicon). */
    if (vg_lite_map(&s_target, VG_LITE_MAP_USER_MEMORY, 0) != VG_LITE_SUCCESS) {
        s_err++;
        return false;
    }
    lv_display_add_event_cb(disp, render_ready_cb, LV_EVENT_RENDER_READY, NULL);
    synthui_rotary_gpu_enabled = true;
    s_begun = true;
    return true;
}

uint32_t synthui_rotary_gpu_errors(void) { return s_err; }
```

- [ ] **Step 3: CMake flavor** — in `evkb.cmake`, replace the body of
  `import_evkb_synthui` with (comment block kept, plus the new flavor note):

```cmake
macro(import_evkb_synthui)
    if(NOT TARGET SynthUI)
        import_evkb_lvgl()
        evkb_library_dir(SynthUI _evkb_synthui_dir)
        file(GLOB _evkb_synthui_src CONFIGURE_DEPENDS
             "${_evkb_synthui_dir}/src/*.cpp")
        add_library(SynthUI STATIC ${_evkb_synthui_src})
        target_include_directories(SynthUI PUBLIC "${_evkb_synthui_dir}/src")
        target_link_libraries(SynthUI PUBLIC LVGL m)
        target_link_libraries(SynthUI PRIVATE teensy_flags)
        # Optional flavor: import_evkb_synthui(VGLITE) additionally compiles
        # src/vglite/ (the synthui_rotary_knob GPU compositor) and links the
        # VGLite driver. The plain call is byte-for-byte the old behavior --
        # the core glob is non-recursive, so src/vglite/ is invisible to it,
        # and the core references the compositor only through the one bool it
        # sets, so neither build direction has undefined symbols. LVGL stays
        # the SOFTWARE renderer either way (LV_USE_DRAW_VG_LITE untouched):
        # the GPU is reached by direct vg_lite calls behind a runtime probe,
        # the one-ELF-both-outcomes pattern rotary_knob_bench proved.
        if("VGLITE" IN_LIST ARGN)
            import_evkb_vglite()
            file(GLOB _evkb_synthui_gpu_src CONFIGURE_DEPENDS
                 "${_evkb_synthui_dir}/src/vglite/*.cpp")
            target_sources(SynthUI PRIVATE ${_evkb_synthui_gpu_src})
            target_include_directories(SynthUI PUBLIC
                 "${_evkb_synthui_dir}/src/vglite")
            target_link_libraries(SynthUI PUBLIC VGLite)
        endif()
    endif()
endmacro()
```

  ★ `IN_LIST ARGN` inside a macro: ARGN is not a real variable — if this
  errors or misbehaves, use `set(_evkb_synthui_args ${ARGN})` then
  `if("VGLITE" IN_LIST _evkb_synthui_args)`. Verify by configuring both a
  plain consumer and the VGLITE consumer in Task 4.

- [ ] **Step 4: Plain-flavor regression check** — reconfigure + build
  `synthui_step_test` (plain SynthUI import): `cmake --build examples/display/synthui_step_test/build` — clean, and its gate `./run_qemu.sh` still
  PASSES with the OLD golden (the cross-widget control, first reading).

- [ ] **Step 5: Commit (SynthUI, then evkb)** —
  SynthUI: `git add src/vglite && git commit -m "rotary: opt-in GC355 rotor compositor (cached paths, RENDER_READY pass)"`;
  evkb: `git add evkb.cmake && git commit -m "evkb.cmake: import_evkb_synthui(VGLITE) flavor for the rotary knob compositor"`

### Task 4: Rewrite `synthui_knob_test` as the rotary widget's test

**Files:**
- Modify: `examples/display/synthui_knob_test/CMakeLists.txt`
- Rewrite: `examples/display/synthui_knob_test/synthui_knob_test.cpp`

- [ ] **Step 1: CMakeLists** — add after `import_evkb_lvgl()`:
  change `import_evkb_synthui()` → `import_evkb_synthui(VGLITE)`, and the
  final link line → `target_link_libraries(synthui_knob_test.elf SynthUI LVGL VGLite stdc++)`.
  (LVGL import stays plain — software renderer; the bench's CMake comment
  explains why that is what makes one ELF safe everywhere.)

- [ ] **Step 2: Rewrite the sketch** — `synthui_knob_test.cpp`:

```cpp
/* synthui_knob_test - synthui_rotary_knob (NEW-20 Phase 2) on the RK055
 * (720x1280 XRGB8888, DIRECT render), checksummed per feature axis.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * ONE ELF, BOTH ENGINES (rotary_knob_bench's pattern): LVGL is always the
 * software renderer; the GC355 is reached only by the widget's opt-in
 * compositor behind the chip-ID probe. QEMU has no GC355, so there
 * rk_engine=sw and the six goldens below are the software renderer's --
 * asserted by the gate. On silicon rk_engine=gpu and the SAME tokens carry
 * the GPU sums, recorded in transcript_hw_evkb.txt (two golden sets, never
 * reconciled -- hardware AA is not LVGL mask arithmetic).
 *
 * Scene order (all tokens before anything animates -- goldens deterministic):
 *   1. 4x4 grid: rows {endless/light, bounded/light, endless/dark,
 *      bounded/dark} x cols states {idle, active, focus, disabled}.
 *      PANEL_OK, RK_CHIP_ID, rk_engine, LVGL_FLUSHED, LVGL_BYTES,
 *      KNOB_SUM_ALL.
 *   2. One screen per row config: KNOB_SUM_{ENDLESS,BOUNDED}_{LIGHT,DARK}.
 *   3. Accent screen (the DC's four accent options): KNOB_SUM_ACCENT.
 *   4. rk_gpu_err (gpu only), SYNTHUI_KNOB_DONE, then the hero spin
 *      (eyes-on-glass only, signed angles, never checksummed).
 */
#include <Arduino.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_rotary_knob.h"
#include "synthui_rotary_knob_gpu.h"

extern "C" {
#include "vg_lite.h"
#include "vg_lite_platform.h"
}

/* Same pool siting and reasoning as the bench: EXTMEM, not DMAMEM. */
#define VGLITE_POOL_BYTES (2u * 1024u * 1024u)
EXTMEM __attribute__((aligned(64))) static uint8_t vglite_pool[VGLITE_POOL_BYTES];
#define TESS_W 256
#define TESS_H 256

static bool s_gpu = false;

static const float      col_angle[4] = { -105.0f, -35.0f, 35.0f, 105.0f };
static const lv_state_t col_state[4] = { LV_STATE_DEFAULT, LV_STATE_PRESSED,
                                         LV_STATE_FOCUSED, LV_STATE_DISABLED };
static const synthui_rotary_mode_t  row_mode[4]  = {
    SYNTHUI_ROTARY_MODE_ENDLESS, SYNTHUI_ROTARY_MODE_BOUNDED,
    SYNTHUI_ROTARY_MODE_ENDLESS, SYNTHUI_ROTARY_MODE_BOUNDED };
static const synthui_rotary_theme_t row_theme[4] = {
    SYNTHUI_ROTARY_THEME_LIGHT, SYNTHUI_ROTARY_THEME_LIGHT,
    SYNTHUI_ROTARY_THEME_DARK,  SYNTHUI_ROTARY_THEME_DARK };
static const char *row_name[4] = { "ENDLESS_LIGHT", "BOUNDED_LIGHT",
                                   "ENDLESS_DARK",  "BOUNDED_DARK" };
/* The DC accent options (first = unset: the default-index path is a feature) */
static const uint32_t accent_opt[4] = { SYNTHUI_ROTARY_ACCENT_DEFAULT,
                                        0xffd24a, 0x5be0a0, 0xff6a52 };

static lv_obj_t *hero;

static void opaque_bg(lv_obj_t *scr)
{   /* Opaque ground forces LVGL to paint every pixel: fully-defined frames. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
}

static lv_obj_t *make_knob(lv_obj_t *parent, synthui_rotary_mode_t m,
                           synthui_rotary_theme_t th, lv_state_t st,
                           float angle, int32_t size)
{
    lv_obj_t *k = synthui_rotary_knob_create(parent);
    lv_obj_set_size(k, size, size);
    synthui_rotary_knob_set_mode(k, m);
    synthui_rotary_knob_set_theme(k, th);
    synthui_rotary_knob_set_angle(k, angle);
    if (st != LV_STATE_DEFAULT) lv_obj_add_state(k, st);
    return k;
}

static lv_obj_t *build_grid(void)
{
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SynthUI RotaryKnob");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            lv_obj_t *k = make_knob(scr, row_mode[r], row_theme[r],
                                    col_state[c], col_angle[c], 150);
            lv_obj_set_pos(k, 15 + c * 175, 120 + r * 175);
        }
    return scr;
}

static lv_obj_t *build_row_screen(int r)
{
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, row_name[r]);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x9FD4FF), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 460);
    for (int c = 0; c < 4; c++) {
        lv_obj_t *k = make_knob(scr, row_mode[r], row_theme[r], col_state[c],
                                col_angle[c], 150);
        lv_obj_set_pos(k, 15 + c * 175, 560);
    }
    return scr;
}

static lv_obj_t *build_accent_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "ACCENT");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x9FD4FF), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 460);
    for (int c = 0; c < 4; c++) {
        lv_obj_t *k = make_knob(scr, SYNTHUI_ROTARY_MODE_ENDLESS,
                                SYNTHUI_ROTARY_THEME_LIGHT, LV_STATE_DEFAULT,
                                col_angle[c], 150);
        synthui_rotary_knob_set_accent(k, accent_opt[c]);
        lv_obj_set_pos(k, 15 + c * 175, 560);
    }
    return scr;
}

/* Load a screen, render it synchronously, checksum the whole framebuffer.
 * With the compositor attached, RENDER_READY fires INSIDE lv_refr_now() and
 * vg_lite_finish() retires before it returns, so the sum always includes the
 * rotors. */
static uint32_t sum_screen(lv_obj_t *scr)
{
    lv_screen_load(scr);
    lv_obj_invalidate(scr);
    lv_refr_now(NULL);
    lvgl_sum_reset();
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);
    return lvgl_sum_value();
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("SYNTHUI_KNOB_BEGIN");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) {
        Serial1.println("SYNTHUI_KNOB_DONE");
        return;
    }
    Display.fillScreen(0x0000);

    /* ASK BEFORE COMMITTING (vglite_probe): vg_lite_init() SPINS on absent
     * hardware, so the chip-ID read is what makes QEMU a clean negative. */
    vg_lite_init_mem(VGLITE_RT1176_REGISTER_BASE, 0u, vglite_pool,
                     VGLITE_POOL_BYTES);
    const uint32_t chip_id = vg_lite_hal_probe_chip_id();
    Serial1.printf("RK_CHIP_ID=0x%08lX\n", (unsigned long)chip_id);
    const bool vg_up =
        (chip_id != 0u) && (vg_lite_init(TESS_W, TESS_H) == VG_LITE_SUCCESS);

    lvgl_rt1176_begin();
    lvgl_mipi_panel_create(Display);

    /* Attach the compositor only when the GPU is genuinely up; a false here
     * leaves the widget fully software -- the honest negative the gate pins. */
    if (vg_up)
        s_gpu = synthui_rotary_gpu_begin(Display.framebuffer(),
                                         Display.width(), Display.height(),
                                         Display.width() * PANEL_BYTES_PER_PIXEL);
    Serial1.printf("rk_engine=%s\n", s_gpu ? "gpu" : "sw");

    /* Phase 1: grid is the FIRST refresh, so LVGL_BYTES pins a whole-screen
     * paint (same contract as before). */
    lv_screen_load(build_grid());
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000)
        lvgl_rt1176_loop();
    lvgl_sum_reset();
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);
    Serial1.printf("LVGL_FLUSHED=%s\n",
                   lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    Serial1.printf("LVGL_BYTES=%lu\n",
                   (unsigned long)(lvgl_mipi_panel_flushed_px() * PANEL_BYTES_PER_PIXEL));
    Serial1.printf("KNOB_SUM_ALL=0x%08lX\n", (unsigned long)lvgl_sum_value());

    /* Phase 2: one golden per feature axis (the acid-bass lesson). */
    for (int r = 0; r < 4; r++)
        Serial1.printf("KNOB_SUM_%s=0x%08lX\n", row_name[r],
                       (unsigned long)sum_screen(build_row_screen(r)));
    Serial1.printf("KNOB_SUM_ACCENT=0x%08lX\n",
                   (unsigned long)sum_screen(build_accent_screen()));

    /* gpu lines NEVER appear in a sw run -- the gate tripwires on them. */
    if (s_gpu)
        Serial1.printf("rk_gpu_err=%lu\n",
                       (unsigned long)synthui_rotary_gpu_errors());
    Serial1.println("SYNTHUI_KNOB_DONE");

    /* Phase 3: hero spin, glass-only, after every token. */
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    hero = make_knob(scr, SYNTHUI_ROTARY_MODE_ENDLESS,
                     SYNTHUI_ROTARY_THEME_LIGHT, LV_STATE_DEFAULT, 0.0f, 360);
    lv_obj_center(hero);
    lv_screen_load(scr);
}

void loop()
{
    static uint32_t last = 0;
    static float a = 0.0f;
    const uint32_t now = millis();
    /* hero == NULL is ALSO the LVGL-uninitialised guard (PANEL_FAIL path). */
    if (hero && now - last >= 16) {
        last = now;
        /* signed angles on purpose: glass-only coverage of the negative-angle
         * fold in both engines (sw arc fold; gpu rotate takes it natively). */
        a += 1.8f; if (a >= 360.0f) a -= 720.0f;
        synthui_rotary_knob_set_angle(hero, a);
    }
    lvgl_rt1176_loop();
}
```

- [ ] **Step 3: Build** — `cd examples/display/synthui_knob_test && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build`. Expected: clean; this also
  compile-proves the GPU TU (first VGLITE-flavor consumer).

- [ ] **Step 4: Run the OLD gate to watch it fail honestly** —
  `./run_qemu.sh` — expected: FAIL on the grid checksum (old goldens, new
  widget). This is evidence the gate sees the change; do NOT touch goldens yet.

- [ ] **Step 5: Commit** —
  `git add CMakeLists.txt synthui_knob_test.cpp && git commit -m "synthui_knob_test: rewrite for synthui_rotary_knob (both engines, one ELF)"`

### Task 5: Re-golden the `synthui_knob_test` gate (+ tripwire, RED demos)

**Files:**
- Modify: `examples/display/synthui_knob_test/run_qemu.sh`
- Re-capture: `examples/display/synthui_knob_test/transcript_qemu.txt`

- [ ] **Step 1: Capture twice, compare** — run the gate's QEMU invocation
  twice (or `./run_qemu.sh` twice, ignoring its exit) and diff the six
  `KNOB_SUM_*` lines + `rk_engine` between runs:
  identical or stop and debug (a determinism check certifies reproducibility,
  not correctness — correctness comes from Task 11's glass check).
  Also assert `rk_engine=sw` and `RK_CHIP_ID=0x00000000` appear, and that no
  `rk_gpu_err=` line exists.

- [ ] **Step 2: Update the gate** — in `run_qemu.sh`: replace the five old
  `KNOB_SUM_*` greps with the six new tokens/values from Step 1; add after
  the PANEL_OK grep:

```sh
# Engine honesty: QEMU has no GC355, so the run must SAY software. A GPU
# claim with no GPU present -- or a gpu error counter appearing at all --
# must fail by name (the bench's tripwire discipline).
grep -qE "rk_engine=sw\r?$" "$OUT" || { echo "FAIL: engine line missing or not sw"; exit 1; }
grep -q  "rk_engine=gpu" "$OUT" && { echo "FAIL: TRIPWIRE gpu engine claimed in QEMU"; exit 1; }
grep -q  "rk_gpu_err="   "$OUT" && { echo "FAIL: TRIPWIRE gpu error counter in QEMU"; exit 1; }
```

  Update the provenance comment: recorded date, SynthUI SHA (local, pre-push
  — note Task 10 re-verifies via the pin), vendored LVGL 9.4.0, and that
  hardware confirmation lands in Task 11. Keep the "do NOT paste in whatever
  the board printed" warning.

- [ ] **Step 3: Gate green** — `./run_qemu.sh` → PASS.

- [ ] **Step 4: Demonstrate RED twice** — (a) flip one golden's last hex
  digit → run → fails naming that mode → restore; (b)
  `cp` the capture, append a fake `rk_engine=gpu` line, run the gate's grep
  block against it (or temporarily point OUT at it) → tripwire fires →
  restore. Quote both demonstrations in the gate header comment.

- [ ] **Step 5: Re-capture the vacuity fixture** —
  `cp "$(ls build/../*.uart 2>/dev/null || echo build/synthui_knob.uart)" transcript_qemu.txt`
  — use the gate's actual capture path (`gate_capture_path "$DIR" synthui_knob.uart`;
  run `./run_qemu.sh` once more and copy that file), replacing the stale
  fixture (2026-08-25 lesson).

- [ ] **Step 6: Commit** —
  `git add run_qemu.sh transcript_qemu.txt && git commit -m "synthui_knob_test: re-golden for the rotary widget; engine tripwires; fixture re-captured"`

### Task 6: Swap the knobs in `acid_box`

**Files:**
- Modify: `examples/display/acid_box/acid_box.cpp`
- Modify: `examples/display/acid_box/run_qemu.sh` (one golden)
- Re-capture: `examples/display/acid_box/transcript_qemu.txt`

- [ ] **Step 1: Header swap** — `#include "synthui_knob.h"` →
  `#include "synthui_rotary_knob.h"`.

- [ ] **Step 2: `mkknob()`** — replace the create/mode/angle lines with:

```c
    lv_obj_t *k = synthui_rotary_knob_create(scr);
    lv_obj_set_size(k, 150, 150);
    lv_obj_set_pos(k, 15 + col * 175, 90 + row * 185);
    synthui_rotary_knob_set_mode(k, SYNTHUI_ROTARY_MODE_BOUNDED);
    /* The DC default range is ±150; every angle<->param map in this file
     * hardcodes ±140, so the range is stated here instead of inherited. */
    synthui_rotary_knob_set_range(k, -140.0f, 140.0f);
    synthui_rotary_knob_set_angle(k, boot01 * 280.0f - 140.0f);
```

- [ ] **Step 3: pitch knob** — replace its create/mode/detent lines with:

```c
    pitchKnob = synthui_rotary_knob_create(scr);
    lv_obj_set_size(pitchKnob, 120, 120);
    lv_obj_set_pos(pitchKnob, 15, 470);
    synthui_rotary_knob_set_mode(pitchKnob, SYNTHUI_ROTARY_MODE_BOUNDED);
    synthui_rotary_knob_set_range(pitchKnob, -140.0f, 140.0f);
    /* detents are input behavior now (no visual mode): 24 semitone stops */
    synthui_rotary_knob_set_detent_step(pitchKnob, 280.0f / 24.0f);
```

- [ ] **Step 4: remaining call sites** — `synthui_knob_get_angle` →
  `synthui_rotary_knob_get_angle` (the `knob01` helper and `cbPitch`),
  `synthui_knob_set_angle(pitchKnob, …)` → `synthui_rotary_knob_set_angle`.
  `grep -n "synthui_knob" acid_box.cpp` afterwards must return ONLY comment
  lines (fix any comment that now lies, e.g. ones naming the old widget's
  files).

- [ ] **Step 5: Build + old gate red** — `cmake --build build`, `./run_qemu.sh`
  → expected FAIL on `ACIDBOX_UI_SUM` only; every behavioral assertion
  (PLAYING, STEP2, CUTOFF drag presence + order) must still PASS. If a
  behavioral assertion fails, that is a real input-layer regression — stop
  and debug before any re-goldening.

- [ ] **Step 6: Re-golden** — capture twice, compare the sum, update
  `ACIDBOX_UI_SUM` in `run_qemu.sh` with a provenance note, gate green,
  demonstrate red once (flip a digit, run, restore), re-capture
  `transcript_qemu.txt` from the gate's capture file.

- [ ] **Step 7: Commit** —
  `git add acid_box.cpp run_qemu.sh transcript_qemu.txt && git commit -m "acid_box: adopt synthui_rotary_knob (bounded ±140, detent pitch); re-golden UI sum"`

### Task 7: Swap the knobs in `vglite_lvgl_test`

**Files:**
- Modify: `examples/display/vglite_lvgl_test/vglite_lvgl_test.cpp`
- Modify: `examples/display/vglite_lvgl_test/run_qemu.sh` (sw golden)
- Re-capture: `examples/display/vglite_lvgl_test/transcript_qemu.txt`

- [ ] **Step 1: Sketch swap** — include → `synthui_rotary_knob.h`; the
  row/col tables become:

```c
static const float      col_angle[4] = { -105.0f, -35.0f, 35.0f, 105.0f };
static const lv_state_t col_state[4] = { LV_STATE_DEFAULT, LV_STATE_PRESSED,
                                         LV_STATE_FOCUSED, LV_STATE_DISABLED };
static const synthui_rotary_mode_t  row_mode[4]  = {
    SYNTHUI_ROTARY_MODE_ENDLESS, SYNTHUI_ROTARY_MODE_BOUNDED,
    SYNTHUI_ROTARY_MODE_ENDLESS, SYNTHUI_ROTARY_MODE_BOUNDED };
static const synthui_rotary_theme_t row_theme[4] = {
    SYNTHUI_ROTARY_THEME_LIGHT, SYNTHUI_ROTARY_THEME_LIGHT,
    SYNTHUI_ROTARY_THEME_DARK,  SYNTHUI_ROTARY_THEME_DARK };
```

  the real `build_grid()` knob loop becomes:

```c
            lv_obj_t *k = synthui_rotary_knob_create(scr);
            lv_obj_set_size(k, 150, 150);
            synthui_rotary_knob_set_mode(k, row_mode[r]);
            synthui_rotary_knob_set_theme(k, row_theme[r]);
            synthui_rotary_knob_set_angle(k, col_angle[c]);
```

  `fpsbench_anim_cb` setter → `synthui_rotary_knob_set_angle`. The file
  header's "4x4 synthui_knob grid" and the fps history note get one added
  sentence: the 2.83/2.45 fps figures are the OLD knob's; Phase-2 numbers are
  re-measured on silicon (Task 11) and recorded in the hw transcript. No
  compositor attach — this example tests LVGL's own VG_LITE draw unit, and a
  second direct client in LVGL's command stream is the uncontrolled mix the
  bench avoided.

- [ ] **Step 2: Build both flavors compile** — sw: `cmake --build build`;
  GPU: `cmake --build build-vglite` (exists from Phase 1; if its cache is the
  pre-2026-08-14 stale kind, `rm -rf` and reconfigure with
  `-DEVKB_VGLITE=ON` per the example's CMakeLists header). GPU build is
  compile-only here; its golden re-records on silicon in Task 11.

- [ ] **Step 3: Old gate red, then re-golden** — `./run_qemu.sh` → FAIL on
  `KNOB_GRID_SUM_SW` only; capture twice, compare, pin the new sw sum (keep
  the all-zero-FNV rejection line — it is checksum-independent), gate green,
  red demo (flip digit, restore), re-capture `transcript_qemu.txt`.

- [ ] **Step 4: Commit** —
  `git add vglite_lvgl_test.cpp run_qemu.sh transcript_qemu.txt && git commit -m "vglite_lvgl_test: adopt synthui_rotary_knob (mode x theme rows); re-golden sw build"`

### Task 8: Delete the old knob from SynthUI

**Files:**
- Delete: `~/Development/SynthUI/src/synthui_knob.h`, `~/Development/SynthUI/src/synthui_knob.cpp`

- [ ] **Step 1: Prove nothing consumes it** —
  `grep -rn "synthui_knob\b" ~/Development/rt1170/evkb/examples --include=*.cpp --include=*.h | grep -v build | grep -v rotary` → only comments/prose
  (fix stragglers first). `synthui_knob_math.h` and `synthui_step.*` stay.

- [ ] **Step 2: Delete + rebuild all four SynthUI consumers** —
  `git -C ~/Development/SynthUI rm src/synthui_knob.h src/synthui_knob.cpp`;
  rebuild `synthui_knob_test`, `synthui_step_test`, `acid_box`,
  `vglite_lvgl_test` (build + build-vglite) — all clean (the glob re-runs via
  CONFIGURE_DEPENDS).

- [ ] **Step 3: SynthUI host tests** — `tests/run.sh` still green (knob_math
  + palette).

- [ ] **Step 4: Commit (SynthUI)** —
  `git commit -m "knob: remove the old widget -- replaced by synthui_rotary_knob (NEW-20 Phase 2); knob_math stays"`

### Task 9: Tree-wide verification (gates, vacuity, sweep, audit, CLAUDE.md)

**Files:**
- Modify: `CLAUDE.md` (two spots)
- No other source changes expected.

- [ ] **Step 1: The four touched/control gates, idle** —
  `synthui_knob_test`, `acid_box`, `vglite_lvgl_test`, `synthui_step_test`
  each `./run_qemu.sh` → 4× PASS. step_test passing UNCHANGED is the
  cross-widget control the spec names.

- [ ] **Step 2: Vacuity suite** — build prerequisites already exist; run
  `tools/gate-vacuity.test.sh` → all green (it replays the three re-captured
  fixtures; a stale fixture fails here and nowhere else).

- [ ] **Step 3: Full sweep, once, captured** —
  `./tools/run-all-qemu-gates.sh` from the repo root (93-byte path — fits the
  `sun_path` cap), output to a file, ONE instance (the 2026-08-27 void-sweep
  lesson). Expected: **122 gates**, with only the documented exceptions
  plausible (`cm4_audio_test` nondeterminism; `m2_hci_probe[hci]` red while
  the BT bench build-dir configuration stands — check
  `docs/KNOWN-BROKEN-GATES.md` first, per the standing rule). Any other red:
  re-run idle before believing it, then debug.

- [ ] **Step 4: License audit** — `tools/license-audit.sh` → PASS (GATES
  entries unchanged; the walk picks up SynthUI's new sources and knob_test's
  VGLite edge).

- [ ] **Step 5: CLAUDE.md** — (a) in the `vglite_lvgl_test` starred note,
  update the golden hexes to the Task-7/Task-11 values and mark the
  2.83/2.45 fps verdict as the OLD knob's numbers, superseded by
  NEW-20 §13 + Phase 2; (b) in the NEW-20 paragraph add one sentence:
  Phase 2 shipped `synthui_rotary_knob` (notch, vector/gpu + sw fallback),
  old knob deleted, three consumers re-goldened, count still 122.

- [ ] **Step 6: Commit** —
  `git add CLAUDE.md && git commit -m "docs: NEW-20 Phase 2 close-out notes (CLAUDE.md)"`

### Task 10: Push SynthUI, bump the pin, fresh-user verify

**Files:**
- Modify: `evkb.cmake` (SynthUI pin line)

- [ ] **Step 1: Push SynthUI** — `git -C ~/Development/SynthUI push` (its
  remote exists; history is the post-rewrite public one). Record the new tip
  SHA.

- [ ] **Step 2: Bump the pin** — `evkb.cmake` line 131: replace
  `f6309669b634eee3050254c709a2d7517141b6a1` with the new SHA; update that
  line's comment date.

- [ ] **Step 3: Fresh-user verify — build AND gate** — scratch dir:
  `cmake -B /private/tmp/.../rkw-fetch -DEVKB_FORCE_FETCH=ON -DCMAKE_TOOLCHAIN_FILE=... examples/display/synthui_knob_test`, build, then run
  `run_qemu.sh` against that ELF (symlink `build` → fetch dir, run, restore
  — the 2026-08-24 m2_hci_probe procedure). A configure proves resolution;
  only the gate run proves the fetched code behaves.

- [ ] **Step 4: Commit** —
  `git add evkb.cmake && git commit -m "evkb.cmake: bump SynthUI pin to <sha> (synthui_rotary_knob)"`

### Task 11: Silicon verification (EVKB + RK055)

**Files:**
- Update: `examples/display/synthui_knob_test/transcript_hw_evkb.txt`
- Update: `examples/display/vglite_lvgl_test/transcript_hw_evkb.txt`
- Possibly update: `examples/display/acid_box/transcript_hw_evkb.txt`

Bench discipline (CLAUDE.md): kill stale probe daemons; flash load → verify
(VCOM free) → attach `tools/rt1170-console.py <port> 115200` → reset via
backgrounded `LinkServer run`. pyocd cannot reset this target (NEW-20 bench
note) — if a reset is needed beyond LinkServer's, it is SW4 (a hand on the
board) and the run stops there with a report.

- [ ] **Step 1: `synthui_knob_test`** — flash, capture a full boot:
  `RK_CHIP_ID` non-zero, `rk_engine=gpu`, `rk_gpu_err=0` (non-zero voids the
  run — the drew-nothing hazard), six GPU sums; reboot once and compare sums
  (stability); eyes on glass: grid, dark rows, accent colors, hero spinning
  smoothly with hardware AA. Write the capture to
  `transcript_hw_evkb.txt` with a dated header noting the two-golden-sets
  rule.
- [ ] **Step 2: `vglite_lvgl_test` build-vglite** — flash the GPU build,
  record `KNOB_GRID_SUM_SW` (GPU value) in its transcript + gate comment
  (silicon-only golden, per that example's convention); optional FPSBENCH
  build run for the new knob's LVGL-draw-unit numbers, recorded in the
  transcript as context (not gated).
- [ ] **Step 3: `acid_box`** — flash, watch it run (UI renders with the new
  knobs, audio plays, a knob drag works by touch); append a dated note to its
  hw transcript.
- [ ] **Step 4: Commit transcripts** —
  `git add ... && git commit -m "NEW-20 Phase 2: silicon verification -- gpu engine goldens + glass checks"`

### Task 12: Close out

- [ ] **Step 1: Merge** — merge the branch to master (this repo's convention:
  merge commit summarizing the phase), push.
- [ ] **Step 2: Linear** — comment on NEW-20: Phase 2 shipped (what, where,
  goldens re-pinned, silicon evidence), and mark the issue Done (both phases
  complete).
- [ ] **Step 3: Memory** — update
  `memory/new20-rotary-knob-bench.md` (Phase 2 complete + key traps) and
  `MEMORY.md` index line.

---

## Self-review notes

- Spec §5's "PUBLIC include dirs" for `src/vglite` is intentional: only the
  VGLITE flavor adds it, so a plain consumer including the GPU header fails
  at compile time — the desired failure mode.
- Golden values are RECORDED per this tree's convention (capture twice →
  compare → pin → red-demo); the plan gives the exact procedure, not
  placeholder values, because the sums cannot exist before the code runs.
- Type names cross-checked across tasks: `synthui_rotary_knob_t`,
  `synthui_rotary_palette_t`, `synthui_rotary_gpu_begin/errors`,
  `synthui_rotary_gpu_enabled`, `synthui_rotary_knob_list`,
  `synthui_rotary_knob_palette` are used identically in Tasks 2, 3, 4.
- LVGL API details flagged for verification at implementation time (not
  assumptions): `lv_obj_class` designator order with `destructor_cb`,
  `lv_draw_arc_dsc_t.rounded`, `LV_EVENT_RENDER_READY` name — all exist in
  the vendored 9.4 per the bench and old knob, but check before compiling.
