# SynthUI Knob Pilot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `synthui_knob` (the first SynthUI LVGL widget, ported from the DC Knob's `renderVals()` math) plus its RK055-panel example `examples/display/synthui_knob_test` with a QEMU golden-checksum gate and hardware verification.

**Architecture:** Widget code lives in the SynthUI sibling repo (`~/Development/SynthUI/src/`, committed on its own master — local-only repo we own). evkb-side work (evkb.cmake, the example, gate, docs) happens on a **new branch `synthui-knob` in a worktree off evkb `master`** — the main evkb checkout belongs to the live step-seq session and must not be touched. Rendering is LVGL 9 software draw; the gate is the house FNV-1a framebuffer oracle with one golden per mode.

**Tech Stack:** LVGL 9.4 (vendored sibling), MipiDisplay RK055 panel at XRGB8888, gate-lib.sh/qrun QEMU harness, LinkServer for hardware.

**Spec:** `docs/superpowers/specs/2026-08-15-synthui-knob-design.md` (evkb master `e78bfdc`). Read it first. One reconciliation: the spec's `DISPLAY_OK` token is realized as the RK055 siblings' house token **`PANEL_OK`** — same assertion, established name.

**Standing constraints:**
- evkb main checkout: DO NOT touch (live peer session on `step-seq`). All evkb work in the worktree.
- `~/Development/components` and SynthUI `reference/`: read-only. The widget is a clean-room port of the component's math (permitted explicitly by SynthUI's README provenance rules); no bytes from `reference/` are compiled.
- Goldens are RECORDED, never derived, and only after two consecutive identical QEMU runs; glass confirmation lands in the same commit (house ritual, see the rk055 gate's comment block).
- Run gates as `./run_qemu.sh`, never `sh run_qemu.sh`.

---

### Task 1: Worktree + branch for the evkb side

- [ ] **Step 1: Create the worktree** (use the platform worktree tool if available — superpowers:using-git-worktrees — otherwise:)

```bash
git -C /Users/nicholasnewdigate/Development/rt1170/evkb worktree add \
    /Users/nicholasnewdigate/Development/rt1170/evkb/.claude/worktrees/synthui-knob \
    -b synthui-knob master
```

Expected: `Preparing worktree (new branch 'synthui-knob')`, HEAD at `e78bfdc` or later master. **All evkb paths below are relative to this worktree root** (call it `$WT`). SynthUI paths are absolute.

- [ ] **Step 2: Confirm the peer checkout is untouched**

```bash
git -C /Users/nicholasnewdigate/Development/rt1170/evkb branch --show-current
```

Expected: `step-seq` (unchanged).

### Task 2: Widget API + class skeleton (no drawing yet)

**Files:**
- Create: `/Users/nicholasnewdigate/Development/SynthUI/src/synthui_knob.h`
- Create: `/Users/nicholasnewdigate/Development/SynthUI/src/synthui_knob.cpp`

- [ ] **Step 1: Write the header** — exactly the spec §4 API:

```c
/* synthui_knob.h - SynthUI rotary knob, LVGL 9 custom widget.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_KNOB_H
#define SYNTHUI_KNOB_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNTHUI_KNOB_MODE_ENDLESS = 0,
    SYNTHUI_KNOB_MODE_BOUNDED,
    SYNTHUI_KNOB_MODE_DETENTS,
    SYNTHUI_KNOB_MODE_ARC,
} synthui_knob_mode_t;

extern const lv_obj_class_t synthui_knob_class;

lv_obj_t *synthui_knob_create(lv_obj_t *parent);

void  synthui_knob_set_angle(lv_obj_t *obj, float deg);        /* default 0 */
void  synthui_knob_set_mode(lv_obj_t *obj, synthui_knob_mode_t m);
void  synthui_knob_set_sweep(lv_obj_t *obj, float deg);        /* default 215, clamped 30..340 */
void  synthui_knob_set_tick_count(lv_obj_t *obj, uint8_t n);   /* default 8, capped 24 */
void  synthui_knob_set_range(lv_obj_t *obj, float min_deg, float max_deg); /* default -140..140 */
void  synthui_knob_set_detent_step(lv_obj_t *obj, float deg);  /* default 35 */
float synthui_knob_get_angle(const lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Write the skeleton .cpp** — class, constructor with DC defaults, setters with `renderVals()`'s clamps, a draw stub:

```c
/* synthui_knob.cpp - SynthUI rotary knob, LVGL 9 custom widget.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Clean-room port of the DC Knob's renderVals() MATH (SynthUI
 * reference/dc/Knob.dc.html): geometry in a 0..100 viewBox scaled to
 * min(w,h).  v1 shading is deliberately flat (spec 2026-08-15-synthui-knob
 * section 5): one native vertical gradient on the face; the crescent's SVG
 * gradient is replaced by angle-driven luminance (section 5.3). */
#include "synthui_knob.h"
#include <math.h>

#define KNOB_DEG (3.14159265358979f / 180.0f)

typedef struct {
    lv_obj_t obj;
    float angle, sweep, min_deg, max_deg, detent_step;
    uint8_t tick_count;
    synthui_knob_mode_t mode;
} synthui_knob_t;

static void knob_constructor(const lv_obj_class_t *cls, lv_obj_t *obj);
static void knob_event(const lv_obj_class_t *cls, lv_event_t *e);

const lv_obj_class_t synthui_knob_class = {
    .base_class     = &lv_obj_class,
    .constructor_cb = knob_constructor,
    .event_cb       = knob_event,
    .width_def      = 120,
    .height_def     = 120,
    .instance_size  = sizeof(synthui_knob_t),
};

lv_obj_t *synthui_knob_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_class_create_obj(&synthui_knob_class, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

static void knob_constructor(const lv_obj_class_t *cls, lv_obj_t *obj)
{
    LV_UNUSED(cls);
    synthui_knob_t *k = (synthui_knob_t *)obj;
    k->angle = 0.0f; k->sweep = 215.0f;
    k->min_deg = -140.0f; k->max_deg = 140.0f;
    k->detent_step = 35.0f; k->tick_count = 8;
    k->mode = SYNTHUI_KNOB_MODE_ENDLESS;
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

#define KNOB_SETTER(field, expr) do { \
    synthui_knob_t *k = (synthui_knob_t *)obj; \
    k->field = (expr); \
    lv_obj_invalidate(obj); } while (0)

void synthui_knob_set_angle(lv_obj_t *obj, float deg) { KNOB_SETTER(angle, deg); }
void synthui_knob_set_mode(lv_obj_t *obj, synthui_knob_mode_t m) { KNOB_SETTER(mode, m); }
void synthui_knob_set_sweep(lv_obj_t *obj, float deg)
{   /* renderVals(): Math.max(30, Math.min(340, sweep)) */
    KNOB_SETTER(sweep, deg < 30.0f ? 30.0f : (deg > 340.0f ? 340.0f : deg));
}
void synthui_knob_set_tick_count(lv_obj_t *obj, uint8_t n) { KNOB_SETTER(tick_count, n > 24 ? 24 : n); }
void synthui_knob_set_detent_step(lv_obj_t *obj, float deg) { KNOB_SETTER(detent_step, deg); }
void synthui_knob_set_range(lv_obj_t *obj, float min_deg, float max_deg)
{
    synthui_knob_t *k = (synthui_knob_t *)obj;
    k->min_deg = min_deg; k->max_deg = max_deg;
    lv_obj_invalidate(obj);
}
float synthui_knob_get_angle(const lv_obj_t *obj) { return ((const synthui_knob_t *)obj)->angle; }

static void knob_draw(synthui_knob_t *k, lv_layer_t *layer); /* Task 5 */

static void knob_event(const lv_obj_class_t *cls, lv_event_t *e)
{
    LV_UNUSED(cls);
    if (lv_obj_event_base(&synthui_knob_class, e) != LV_RESULT_OK) return;
    if (lv_event_get_code(e) == LV_EVENT_DRAW_MAIN)
        knob_draw((synthui_knob_t *)lv_event_get_current_target_obj(e),
                  lv_event_get_layer(e));
}

/* Task 2 stub -- replaced by the full port in Task 5. */
static void knob_draw(synthui_knob_t *k, lv_layer_t *layer)
{
    LV_UNUSED(k); LV_UNUSED(layer);
}
```

Fallback noted once: if `width_def`/`height_def` are rejected by the compiler (field-name drift), delete those two initializers — the example sets sizes explicitly anyway.

- [ ] **Step 3: Commit (SynthUI repo)**

```bash
git -C /Users/nicholasnewdigate/Development/SynthUI add src/
git -C /Users/nicholasnewdigate/Development/SynthUI commit -m "knob: widget class, API and defaults (draw stub)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task 3: evkb.cmake — declare SynthUI + import macro

**Files:**
- Modify: `$WT/evkb.cmake` (declaration near line 126, macro after the LVGL block ~line 262)

- [ ] **Step 1: Add the declaration** directly below the LVGL `teensy_declare_library` line, pinned at current SynthUI HEAD (`git -C /Users/nicholasnewdigate/Development/SynthUI rev-parse HEAD`):

```cmake
teensy_declare_library(SynthUI        SynthUI              https://github.com/newdigate/SynthUI         <paste-synthui-head-sha> .) # LOCAL-ONLY (unpushed): resolves under TEENSY_LIB_ROOT; fresh clones fail here until SynthUI's first push -- same class as the qemu2-local gate deps. Not Arduino-layout for imports: use import_evkb_synthui().
```

- [ ] **Step 2: Add the macro** after the `import_evkb_lvgl` block:

```cmake
# --- SynthUI -----------------------------------------------------------------
# Plain STATIC target like LVGL/CMSIS-DSP: the widgets #include <lvgl.h>, whose
# include dirs only propagate through a real target_link_libraries edge --
# teensy_target_link_libraries() would rewrite the name to SynthUI.o and lose
# them.  PUBLIC LVGL so consumers get lvgl.h and -lm transitively.
macro(import_evkb_synthui)
    if(NOT TARGET SynthUI)
        import_evkb_lvgl()
        evkb_library_dir(SynthUI _evkb_synthui_dir)
        file(GLOB _evkb_synthui_src ${_evkb_synthui_dir}/src/*.cpp)
        add_library(SynthUI STATIC ${_evkb_synthui_src})
        target_include_directories(SynthUI PUBLIC ${_evkb_synthui_dir}/src)
        target_link_libraries(SynthUI PUBLIC LVGL)
        target_link_libraries(SynthUI PRIVATE teensy_flags)
    endif()
endmacro()
```

- [ ] **Step 3: Commit (worktree)**

```bash
git -C $WT add evkb.cmake
git -C $WT commit -m "build: declare SynthUI (local-only) + import_evkb_synthui()

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task 4: Example scaffold — build system proven before any drawing

**Files:**
- Create: `$WT/examples/display/synthui_knob_test/CMakeLists.txt`
- Create: `$WT/examples/display/synthui_knob_test/synthui_knob_test.cpp` (scaffold version)

- [ ] **Step 1: CMakeLists.txt** (pattern: `lvgl_rk055_panel_test`, XRGB8888, plus SynthUI):

```cmake
cmake_minimum_required(VERSION 3.24)
project(synthui_knob_test)

# XRGB8888 exactly like lvgl_rk055_panel_test (v7).  Directory scope on
# purpose: the definitions must reach the LVGL, MipiDisplay AND SynthUI
# objects created by the imports below, not just this target.
add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
import_evkb_synthui()
# panels/rk055 selects the panel; one MIPI-DSI host, one panel dir ever live.
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)    # Display::fillScreen() paints via the PXP

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(synthui_knob_test
    synthui_knob_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp)
teensy_target_link_libraries(synthui_knob_test cores MipiDisplay PXP)

# SynthUI and LVGL are plain CMake static-lib targets (see lvgl_rk055_panel_test
# for why teensy_target_link_libraries cannot link them).
target_link_libraries(synthui_knob_test.elf SynthUI LVGL stdc++)
```

- [ ] **Step 2: Scaffold .cpp** — panel up, ONE stub knob created (proves the widget links), tokens, no checksums yet:

```cpp
#include <Arduino.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_knob.h"

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("SYNTHUI_KNOB_BEGIN");
    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) { Serial1.println("SYNTHUI_KNOB_DONE"); return; }
    Display.fillScreen(0x0000);
    lvgl_rt1176_begin();
    lvgl_mipi_panel_create(Display);
    lv_obj_t *k = synthui_knob_create(lv_screen_active());
    lv_obj_center(k);
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000) lvgl_rt1176_loop();
    Serial1.printf("LVGL_FLUSHED=%s\n", lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    Serial1.println("SYNTHUI_KNOB_DONE");
}
void loop() { lvgl_rt1176_loop(); }
```

- [ ] **Step 3: Build**

```bash
cd $WT/examples/display/synthui_knob_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```

Expected: `synthui_knob_test.elf` produced, zero warnings from `synthui_knob.cpp`. A failure about `width_def` → apply the Task 2 fallback (delete the two lines in SynthUI, recommit as `knob: drop width_def (field drift)`), rebuild.

- [ ] **Step 4: Boot it in QEMU (no gate yet)**

```bash
$WT/tools/rt1170-qemu.sh $WT/examples/display/synthui_knob_test/build/synthui_knob_test.elf
```

Expected in UART: `SYNTHUI_KNOB_BEGIN`, `PANEL_OK`, `LVGL_FLUSHED=PASS`, `SYNTHUI_KNOB_DONE`. (Stub draw = blank knob; that's correct here.) If `PANEL_FAIL`, stop — that's an environment regression, not this plan.

- [ ] **Step 5: Commit (worktree)**

```bash
git -C $WT add examples/display/synthui_knob_test
git -C $WT commit -m "synthui_knob_test: scaffold -- panel + widget link + tokens

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task 5: The full draw port

**Files:**
- Modify: `/Users/nicholasnewdigate/Development/SynthUI/src/synthui_knob.cpp` (replace the stub `knob_draw` and add helpers ABOVE it)

- [ ] **Step 1: Add palette + helpers** (immediately above `knob_draw`):

```c
typedef struct {
    lv_color_t ring, tick, detent, stop, track, arc,
               face_from, face_to, cres_from, cres_to, pointer, cap;
    uint8_t gopa;                    /* SVG group alpha: 166 disabled, else 255 */
} knob_palette_t;

/* Hexes verbatim from renderVals(); DC 'state' maps onto LVGL states
 * (disabled wins, then active/pressed; focus only recolors the ring). */
static void knob_palette(lv_state_t st, knob_palette_t *p)
{
    const bool dis = st & LV_STATE_DISABLED;
    const bool act = (st & LV_STATE_PRESSED) && !dis;
    const bool foc = (st & LV_STATE_FOCUSED) && !dis;
    const lv_color_t ink = lv_color_hex(dis ? 0x8b8b93 : 0x2b2e5c);
    p->ring      = foc ? lv_color_hex(0x5b62b8) : ink;
    p->tick      = lv_color_hex(dis ? 0xb6b6bd : 0x3a3d6b);
    p->detent    = lv_color_hex(dis ? 0xb0b0b8 : 0x6f74bd);
    p->stop      = lv_color_hex(dis ? 0x8e8e98 : 0xc2543f);
    p->track     = lv_color_hex(dis ? 0xdcdce1 : 0xd8d9e8);
    p->arc       = lv_color_hex(dis ? 0xa8a8b2 : 0x5b62b8);
    p->face_from = lv_color_hex(dis ? 0xf4f4f4 : 0xfcfbf6);
    p->face_to   = lv_color_hex(dis ? 0xe6e6e9 : 0xe7e7f1);
    p->cres_from = lv_color_hex(dis ? 0xc8c8cf : (act ? 0x9aa0e0 : 0x8f96d4));
    p->cres_to   = lv_color_hex(dis ? 0x8e8e98 : 0x282b60);
    p->pointer   = lv_color_hex(dis ? 0xeeeef0 : 0xfdfdf9);
    p->cap       = lv_color_hex(dis ? 0xf2f2f4 : 0xfdfdf9);
    p->gopa      = dis ? 166 : 255;
}

/* P(r, th): 0 deg = 12 o'clock, clockwise -- the DC convention. */
static void polar(float cx, float cy, float S, float r, float deg,
                  lv_point_precise_t *out)
{
    out->x = (lv_value_precise_t)(cx + r * S * sinf(deg * KNOB_DEG));
    out->y = (lv_value_precise_t)(cy - r * S * cosf(deg * KNOB_DEG));
}

static void draw_ray(lv_layer_t *layer, float cx, float cy, float S,
                     float r1, float r2, float deg,
                     lv_color_t color, float w, uint8_t opa)
{
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    polar(cx, cy, S, r1, deg, &d.p1);
    polar(cx, cy, S, r2, deg, &d.p2);
    d.color = color; d.opa = opa;
    d.width = (int32_t)lroundf(w * S); if (d.width < 1) d.width = 1;
    lv_draw_line(layer, &d);
}

/* LVGL arcs are annulus sectors and measure from 3 o'clock: lv = dc - 90. */
static void draw_arc_seg(lv_layer_t *layer, float cx, float cy, float S,
                         float r, float w, float a1, float a2,
                         lv_color_t color, uint8_t opa)
{
    lv_draw_arc_dsc_t a; lv_draw_arc_dsc_init(&a);
    a.center.x = (int32_t)lroundf(cx); a.center.y = (int32_t)lroundf(cy);
    a.radius = (uint16_t)lroundf(r * S);
    a.width  = (int32_t)lroundf(w * S); if (a.width < 1) a.width = 1;
    a.start_angle = (lv_value_precise_t)(a1 - 90.0f);
    a.end_angle   = (lv_value_precise_t)(a2 - 90.0f);
    a.color = color; a.opa = opa;
    lv_draw_arc(layer, &a);
}

static void draw_disc(lv_layer_t *layer, float x, float y, float rpx,
                      const lv_draw_rect_dsc_t *dsc)
{
    lv_area_t a = { (int32_t)lroundf(x - rpx), (int32_t)lroundf(y - rpx),
                    (int32_t)lroundf(x + rpx), (int32_t)lroundf(y + rpx) };
    lv_draw_rect(layer, (lv_draw_rect_dsc_t *)dsc, &a);
}
```

- [ ] **Step 2: Replace the stub `knob_draw`** with the full port (element order = `renderVals()`'s):

```c
static void knob_draw(synthui_knob_t *k, lv_layer_t *layer)
{
    lv_obj_t *obj = &k->obj;
    lv_area_t coords; lv_obj_get_coords(obj, &coords);
    const float W = (float)lv_area_get_width(&coords);
    const float H = (float)lv_area_get_height(&coords);
    const float S = (W < H ? W : H) / 100.0f;
    const float cx = (float)coords.x1 + W * 0.5f;
    const float cy = (float)coords.y1 + H * 0.5f;

    knob_palette_t pal; knob_palette(lv_obj_get_state(obj), &pal);
    const uint8_t g = pal.gopa;

    /* tick ring -- fixed, never rotates; decorated modes keep only the top
     * orientation marker (renderVals' effTicks) */
    const uint8_t nticks =
        (k->mode == SYNTHUI_KNOB_MODE_ENDLESS) ? k->tick_count : 1;
    for (uint8_t i = 0; i < nticks; i++) {
        const float th = (float)i * 360.0f / (float)nticks;
        const bool major = (i == 0);
        draw_ray(layer, cx, cy, S, 37.5f, major ? 49.0f : 45.5f, th,
                 pal.tick, major ? 3.4f : 2.4f, g);
    }

    if (k->mode == SYNTHUI_KNOB_MODE_DETENTS && k->detent_step > 0.0f)
        for (float a = k->min_deg; a <= k->max_deg + 0.001f; a += k->detent_step)
            draw_ray(layer, cx, cy, S, 37.5f, 44.0f, a, pal.detent, 2.0f, g);

    if (k->mode == SYNTHUI_KNOB_MODE_BOUNDED ||
        k->mode == SYNTHUI_KNOB_MODE_DETENTS) {
        draw_ray(layer, cx, cy, S, 37.0f, 49.0f, k->min_deg, pal.stop, 3.4f, g);
        draw_ray(layer, cx, cy, S, 37.0f, 49.0f, k->max_deg, pal.stop, 3.4f, g);
    }

    if (k->mode == SYNTHUI_KNOB_MODE_ARC) {
        float v = k->angle;
        if (v < k->min_deg) v = k->min_deg;
        if (v > k->max_deg) v = k->max_deg;
        draw_arc_seg(layer, cx, cy, S, 38.5f, 3.2f, k->min_deg, k->max_deg,
                     pal.track, g);
        if (v > k->min_deg)
            draw_arc_seg(layer, cx, cy, S, 38.5f, 3.2f, k->min_deg, v,
                         pal.arc, g);
    }

    /* face: r=33 disc, vertical faceFrom->faceTo, ring border 2.6 */
    {
        lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
        d.radius = LV_RADIUS_CIRCLE;
        d.bg_opa = g;
        d.bg_grad.dir = LV_GRAD_DIR_VER;
        d.bg_grad.stops[0].color = pal.face_from;
        d.bg_grad.stops[0].opa = 255; d.bg_grad.stops[0].frac = 0;
        d.bg_grad.stops[1].color = pal.face_to;
        d.bg_grad.stops[1].opa = 255; d.bg_grad.stops[1].frac = 255;
        d.bg_grad.stops_count = 2;
        d.border_color = pal.ring; d.border_opa = g;
        d.border_width = (int32_t)lroundf(2.6f * S);
        if (d.border_width < 1) d.border_width = 1;
        draw_disc(layer, cx, cy, 33.0f * S, &d);
    }

    /* crescent: annulus r=21..30 spanning sweep centred on angle; solid color,
     * angle-driven luminance (spec 5.3) -- lightest pointing at the top-left
     * light (-45 deg), darkest opposite */
    {
        const float t = (1.0f - cosf((k->angle + 45.0f) * KNOB_DEG)) * 0.5f;
        const lv_color_t c = lv_color_mix(pal.cres_to, pal.cres_from,
                                          (uint8_t)lroundf(255.0f * (1.0f - t)));
        draw_arc_seg(layer, cx, cy, S, 30.0f, 9.0f,
                     k->angle - k->sweep * 0.5f, k->angle + k->sweep * 0.5f,
                     c, g);
    }

    /* pointer dot at P(25.5, angle), r=4 */
    {
        lv_point_precise_t pt; polar(cx, cy, S, 25.5f, k->angle, &pt);
        lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
        d.radius = LV_RADIUS_CIRCLE; d.bg_color = pal.pointer; d.bg_opa = g;
        draw_disc(layer, (float)pt.x, (float)pt.y, 4.0f * S, &d);
    }

    /* cap: r=20 at SVG opacity 0.55 (140/255), scaled by group alpha */
    {
        lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
        d.radius = LV_RADIUS_CIRCLE; d.bg_color = pal.cap;
        d.bg_opa = (uint8_t)((140u * g) >> 8);
        draw_disc(layer, cx, cy, 20.0f * S, &d);
    }
}
```

- [ ] **Step 3: Rebuild + boot** (same commands as Task 4 steps 3–4). Expected: same tokens, and the QEMU run is the first render of a real knob — no crash, `LVGL_FLUSHED=PASS`.

- [ ] **Step 4: Commit (SynthUI)**

```bash
git -C /Users/nicholasnewdigate/Development/SynthUI add src/synthui_knob.cpp
git -C /Users/nicholasnewdigate/Development/SynthUI commit -m "knob: full renderVals() draw port -- 4 modes, 4 states, angle-lit crescent

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

- [ ] **Step 5: Update the SynthUI pin in `$WT/evkb.cmake`** to the new SynthUI HEAD sha, and commit the worktree change (`build: bump SynthUI pin for the draw port`). The pin is documentary until first push, but keeping it current is the house discipline.

### Task 6: Full scene — grid + per-mode phases + hero

**Files:**
- Modify: `$WT/examples/display/synthui_knob_test/synthui_knob_test.cpp` (replace scaffold entirely)

- [ ] **Step 1: Write the full example:**

```cpp
/* synthui_knob_test - all four synthui_knob modes x four states on the RK055
 * (720x1280 XRGB8888, DIRECT render), checksummed PER MODE.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Phase order (all tokens before any animation -- goldens stay deterministic):
 *   1. 4x4 grid (rows=modes, cols=states): PANEL_OK, LVGL_FLUSHED, LVGL_BYTES,
 *      KNOB_SUM_ALL.  LVGL_BYTES is the flushed AREA of the FIRST refresh --
 *      720*1280*4 only if LVGL painted the whole screen (the partial-repaint
 *      guard, same contract as lvgl_rk055_panel_test).
 *   2. One screen per mode, rendered synchronously via lv_refr_now():
 *      KNOB_SUM_<MODE>.  One golden per mode is the acid-bass lesson: a single
 *      aggregate sum can freeze half the feature without changing color.
 *   3. SYNTHUI_KNOB_DONE, then a hero knob animates forever (eyes-on-glass only,
 *      never checksummed).
 */
#include <Arduino.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_knob.h"

static const float               col_angle[4] = { -105.0f, -35.0f, 35.0f, 105.0f };
static const lv_state_t          col_state[4] = { LV_STATE_DEFAULT, LV_STATE_PRESSED,
                                                  LV_STATE_FOCUSED, LV_STATE_DISABLED };
static const synthui_knob_mode_t row_mode[4]  = { SYNTHUI_KNOB_MODE_ENDLESS,
                                                  SYNTHUI_KNOB_MODE_BOUNDED,
                                                  SYNTHUI_KNOB_MODE_DETENTS,
                                                  SYNTHUI_KNOB_MODE_ARC };
static const char               *mode_name[4] = { "ENDLESS", "BOUNDED", "DETENTS", "ARC" };

static lv_obj_t *hero;

static void opaque_bg(lv_obj_t *scr)
{   /* Opaque ground forces LVGL to paint every pixel: fully-defined frames. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
}

static lv_obj_t *make_knob(lv_obj_t *parent, synthui_knob_mode_t m,
                           lv_state_t st, float angle, int32_t size)
{
    lv_obj_t *k = synthui_knob_create(parent);
    lv_obj_set_size(k, size, size);
    synthui_knob_set_mode(k, m);
    synthui_knob_set_angle(k, angle);
    if (st != LV_STATE_DEFAULT) lv_obj_add_state(k, st);
    return k;
}

static lv_obj_t *build_grid(void)
{
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SynthUI Knob");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            lv_obj_t *k = make_knob(scr, row_mode[r], col_state[c], col_angle[c], 150);
            lv_obj_set_pos(k, 15 + c * 175, 120 + r * 175);
        }
    return scr;
}

static lv_obj_t *build_mode_screen(int m)
{
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, mode_name[m]);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x9FD4FF), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 460);
    for (int c = 0; c < 4; c++) {
        lv_obj_t *k = make_knob(scr, row_mode[m], col_state[c], col_angle[c], 150);
        lv_obj_set_pos(k, 15 + c * 175, 560);
    }
    return scr;
}

/* Load a screen, render it synchronously, checksum the whole framebuffer. */
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
    if (!ok) { Serial1.println("SYNTHUI_KNOB_DONE"); return; }
    Display.fillScreen(0x0000);

    lvgl_rt1176_begin();
    lvgl_mipi_panel_create(Display);

    /* Phase 1: the grid is the FIRST refresh, so LVGL_BYTES pins a whole-
     * screen paint (same contract as lvgl_rk055_panel_test). */
    lv_screen_load(build_grid());
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000)
        lvgl_rt1176_loop();
    lvgl_sum_reset();
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);
    Serial1.printf("LVGL_FLUSHED=%s\n", lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    Serial1.printf("LVGL_BYTES=%lu\n",
                   (unsigned long)(lvgl_mipi_panel_flushed_px() * PANEL_BYTES_PER_PIXEL));
    Serial1.printf("KNOB_SUM_ALL=0x%08lX\n", (unsigned long)lvgl_sum_value());

    /* Phase 2: one golden per mode. */
    for (int m = 0; m < 4; m++)
        Serial1.printf("KNOB_SUM_%s=0x%08lX\n", mode_name[m],
                       (unsigned long)sum_screen(build_mode_screen(m)));

    Serial1.println("SYNTHUI_KNOB_DONE");

    /* Phase 3: hero spin, glass-only, after every token. */
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    hero = make_knob(scr, SYNTHUI_KNOB_MODE_ENDLESS, LV_STATE_DEFAULT, 0.0f, 360);
    lv_obj_center(hero);
    lv_screen_load(scr);
}

void loop()
{
    static uint32_t last = 0;
    static float a = 0.0f;
    const uint32_t now = millis();
    /* hero == NULL is ALSO the LVGL-uninitialised guard: on the PANEL_FAIL
     * early-return no screen exists and loop() must touch nothing. */
    if (hero && now - last >= 16) {
        last = now;
        /* wrap into [-360,360): half the cycle feeds NEGATIVE angles on
         * purpose -- glass-only coverage of the signed-angle path (Task 9's
         * eyes-on step should see continuous rotation, no jump). */
        a += 1.8f; if (a >= 360.0f) a -= 720.0f;
        synthui_knob_set_angle(hero, a);
    }
    lvgl_rt1176_loop();
}
```

- [ ] **Step 2: Rebuild + boot in QEMU** (Task 4 commands). Expected UART: all tokens through `SYNTHUI_KNOB_DONE`, with five `KNOB_SUM_*=0x…` hex values and `LVGL_BYTES=3686400`. Any `LVGL_BYTES` other than 3686400 means the grid was not the first full paint — fix ordering, don't touch the assertion.

- [ ] **Step 3: Determinism check — run QEMU twice more; sums must be identical across runs**

```bash
for i in 1 2; do $WT/tools/rt1170-qemu.sh $WT/examples/display/synthui_knob_test/build/synthui_knob_test.elf 2>&1 | grep "KNOB_SUM"; done
```

Expected: the two blocks of five sums are byte-identical. If not, something nondeterministic leaked into the scene (uninitialized state, time-derived content) — STOP and find it; do not record goldens.

- [ ] **Step 4: Commit (worktree)** — `synthui_knob_test: full grid + per-mode checksum phases + hero spin`.

### Task 7: The gate — failing first, then recorded goldens

**Files:**
- Create: `$WT/examples/display/synthui_knob_test/run_qemu.sh` (mode 755)
- Create: `$WT/examples/display/synthui_knob_test/transcript_qemu.txt`

- [ ] **Step 1: Write the gate with DELIBERATELY WRONG goldens** (`0xDEADBEEF`) — the failing-test-first step proving the assertions can fire:

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
ELF="$DIR/$(gate_build_dir)/synthui_knob_test.elf"; OUT="$DIR/synthui_knob.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/synthui_knob.dbg" &
P=$!; gate_pid $P
# 16s: the RK055 bring-up margin lvgl_rk055_panel_test uses (12s) plus four
# extra full-screen software renders for the per-mode phases.
sleep 16; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
# Panel chain first: in DIRECT mode a framebuffer no display owns would still
# checksum perfectly.
grep -q "PANEL_OK"          "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
# Flushed AREA of the first refresh -- 720*1280*4 at XRGB8888.  The partial-
# repaint guard: a corner repaint and a scene edit both just change the sums.
# Value greps are ANCHORED (CR-tolerant \r?$): 3686400 must not pass via a
# hypothetical 36864000, and a golden must match the WHOLE value.
grep -qE "LVGL_BYTES=3686400\r?$" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }
# GOLDEN CHECKSUMS -- FNV-1a over the whole 720x1280 framebuffer, ONE PER MODE
# plus the 4x4 grid.  Per-mode goldens are the acid-bass lesson: a single
# aggregate can silently stop testing half the feature.  RECORDED, not derived
# -- stable across two consecutive QEMU runs AND confirmed by a human eye on
# the RK055 glass in the SAME commit that records them (this panel's goldens
# are glass-confirmed, like lvgl_rk055_panel_test's and unlike the RPi gate's).
# On a mismatch work out WHICH of {SynthUI pin, LVGL pin, lv_conf.h, fonts,
# scene} changed; do NOT paste in whatever the board printed.
#
# Provenance: recorded YYYY-MM-DD against SynthUI <sha>, vendored LVGL 9.4.0,
# XRGB8888 (LV_COLOR_DEPTH=32), montserrat 14/28.
grep -qE "KNOB_SUM_ALL=0xDEADBEEF\r?$"     "$OUT" || { echo "FAIL: grid checksum"; exit 1; }
grep -qE "KNOB_SUM_ENDLESS=0xDEADBEEF\r?$" "$OUT" || { echo "FAIL: endless checksum"; exit 1; }
grep -qE "KNOB_SUM_BOUNDED=0xDEADBEEF\r?$" "$OUT" || { echo "FAIL: bounded checksum"; exit 1; }
grep -qE "KNOB_SUM_DETENTS=0xDEADBEEF\r?$" "$OUT" || { echo "FAIL: detents checksum"; exit 1; }
grep -qE "KNOB_SUM_ARC=0xDEADBEEF\r?$"     "$OUT" || { echo "FAIL: arc checksum"; exit 1; }
grep -q "SYNTHUI_KNOB_DONE"    "$OUT" || { echo "FAIL: no completion token"; exit 1; }
echo "PASS: SynthUI knob render verified"
```

```bash
chmod 755 $WT/examples/display/synthui_knob_test/run_qemu.sh
```

- [ ] **Step 2: Run it — expect FAIL at the first golden**

```bash
cd $WT/examples/display/synthui_knob_test && ./run_qemu.sh; echo "exit=$?"
```

Expected: `FAIL: grid checksum`, `exit=1`, with the real sums visible in the echoed UART. This proves a wrong render CANNOT pass.

- [ ] **Step 3: Record the goldens** — replace the five `0xDEADBEEF` with the sums from Task 6 Step 3 (already proven stable across two runs), fill the Provenance line (today's date, `git -C /Users/nicholasnewdigate/Development/SynthUI rev-parse --short HEAD`), re-run twice:

```bash
./run_qemu.sh && ./run_qemu.sh; echo "exit=$?"
```

Expected: `PASS: SynthUI knob render verified` twice, `exit=0`.

- [ ] **Step 4: Save the transcript** — copy the full PASS output (UART + assertions + PASS line) into `transcript_qemu.txt`, house style.

- [ ] **Step 5: Commit (worktree)** — `synthui_knob_test: QEMU gate with per-mode recorded goldens + transcript`.

### Task 8: License audit

**Files:**
- Modify: `$WT/tools/license-audit.sh` (GATES list, sorted among the `display/` entries)

- [ ] **Step 1: Add the entry** (format matches neighbors exactly):

```
examples/display/synthui_knob_test:synthui_knob_test \
```

- [ ] **Step 1b: Add SynthUI to the audit's REPOS roots** — `license-audit.sh`'s
  Part 2 sweeps every link-manifest dep path against its REPOS roots and
  hard-fails anything outside them ("OUTSIDE SWEPT ROOTS"). SynthUI enters link
  manifests for the first time with this example, so add `$LIB_ROOT/SynthUI`
  alongside the existing roots (top of the script, ~lines 29-35). This is
  bookkeeping, NOT an allowlist entry: SynthUI is MIT-clean (pre-verified —
  zero copyleft hits, zero tracked binaries), and adding the root puts it
  UNDER the sweep rather than excusing it from one.

- [ ] **Step 2: Run the audit**

```bash
cd $WT && ./tools/license-audit.sh; echo "exit=$?"
```

Expected: `LICENSE-AUDIT: PASS`, `exit=0`, with the new entry's depfile walk included (SynthUI's MIT sources are first-party; nothing from `reference/` is in any depfile). Any FAIL naming SynthUI files: STOP and read the failure — do not allowlist. (An "OUTSIDE SWEPT ROOTS" failure means Step 1b was missed — that fix is the REPOS root, which is expected bookkeeping, not an allowlist.)

- [ ] **Step 3: Commit (worktree)** — `tools: license-audit GATES entry for synthui_knob_test`.

### Task 9: Hardware verification (RK055 glass)

**Files:**
- Create: `$WT/examples/display/synthui_knob_test/transcript_hw_evkb.txt`

- [ ] **Step 1: Flash — VCOM must be free** (the console reader panics the Mac if attached during LinkServer ops — see memory):

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load $WT/examples/display/synthui_knob_test/build/synthui_knob_test.elf
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify $WT/examples/display/synthui_knob_test/build/synthui_knob_test.elf
```

- [ ] **Step 2: Attach the console, then reset via a backgrounded run**

```bash
python3 $WT/tools/rt1170-console.py /dev/cu.usbmodem* 115200 > /tmp/knob_hw.log &
LinkServer run MIMXRT1176:MIMXRT1170-EVKB $WT/examples/display/synthui_knob_test/build/synthui_knob_test.elf &
sleep 20; kill %1 %2 2>/dev/null; cat /tmp/knob_hw.log
```

Expected: the same token stream as QEMU, and — this is the point — **the five `KNOB_SUM_*` values IDENTICAL to the QEMU goldens** (deterministic software render; the house premise since the LVGL v-series).

- [ ] **Step 3: Eyes on glass.** Confirm with the user (photo or their own look): 4×4 grid visible, rows reading endless/bounded/detents/arc, disabled column dimmed, then the hero knob spinning smoothly with the crescent brightening toward the top-left. **This confirmation is what makes the goldens glass-confirmed** — record the human confirmation in the transcript.

- [ ] **Step 4: Write `transcript_hw_evkb.txt`** — console capture + a dated note of the glass confirmation and who confirmed, house style. Commit (worktree): `synthui_knob_test: hardware transcript -- goldens confirmed on RK055 glass`.

### Task 10: Sweep + docs + wrap

**Files:**
- Modify: `$WT/CLAUDE.md` (sweep baseline), `$WT/examples/README.md` (display category list)

- [ ] **Step 1: Build check** — gates do not build; make sure nothing else regressed. Then run the full sweep from the worktree:

```bash
cd $WT && ./tools/run-all-qemu-gates.sh -l | tail -3   # count first
./tools/run-all-qemu-gates.sh
```

Expected: **previous-count+1 passed, 0 failed, 0 SKIP** — nominally 93 if the step-seq session's gate has merged into this line's history, 92 if not. DO NOT assume: read the runner's own count and gate names. The one tolerated red is `rt1176:dualcore/cm4_audio_test` (re-run idle before believing any dual-core red; do not weaken anything).

- [ ] **Step 2: Update `$WT/CLAUDE.md`** — extend the sweep-count paragraph exactly in its established style: new total, prepend `(N before the SynthUI knob pilot added display/synthui_knob_test; …)` to the history chain, and replace the "Measured" line with today's measured result. Numbers come from Step 1's actual output.

- [ ] **Step 3: Add `synthui_knob_test` to `$WT/examples/README.md`'s display section**, one line in the neighbors' voice (what it proves: first SynthUI widget, per-mode goldens, glass-confirmed).

- [ ] **Step 3b: Document the SKIP class in `$WT/docs/KNOWN-BROKEN-GATES.md`** — a
  ★ entry in the house style of the existing local-only entries, stating: on a
  fresh clone, `rt1176:display/synthui_knob_test` reports **SKIP, not RED** —
  `import_evkb_synthui()` FATAL_ERRORs without a local `$TEENSY_LIB_ROOT/SynthUI`
  checkout (SynthUI is unpushed), so the example cannot even configure and the
  runner reports `(not built)`. This is the tree's FIRST SKIP-class local-only
  dependency (the three existing local-only deps all build and surface as REDs);
  the distinction matters because 0-SKIP is the sweep's coverage signal. The
  example's CMakeLists comment cross-references this entry.

- [ ] **Step 3c: Correct `$WT/README.md`'s dedicated-macro paragraph** — the "Two
  libraries are too large for the Arduino-style importer" paragraph (~line 152)
  now undercounts: three libraries use dedicated import macros, and SynthUI's
  reason is LVGL's target-propagated includes, not size. One-line correction.

- [ ] **Step 4: Commit (worktree)** — `docs: sweep baseline +1 (synthui_knob_test), measured clean`.

- [ ] **Step 5: Final state report** — branch `synthui-knob` in the worktree, all commits listed, SynthUI HEAD sha, sweep numbers, audit PASS, hardware confirmation status. Then invoke **superpowers:finishing-a-development-branch** for the merge decision (merge target: evkb `master`; note the peer session's `step-seq` may merge before or after — both orders are conflict-free except CLAUDE.md's sweep counts, which whoever merges second reconciles by re-measuring).
