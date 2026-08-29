# SynthUI Fader GPU Compositor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Transplant the rotary's deferred pre-flip GPU compositor to the fader so the 16-bank clears `mfps_med ≥ 30000` on silicon, with every sw/QEMU golden unchanged.

**Architecture:** Spec: `docs/superpowers/specs/2026-08-29-synthui-fader-gpu-design.md`. Two-TU split (private header + registry + `gpu_pending`; GPU TU in `src/vglite/`), per-frame path arena + composite-minus machinery copied from `synthui_rotary_knob_gpu.cpp` (the authoritative reference — read it first, every ★ comment in it is a paid-for lesson), cap gradient via cached `vg_lite_linear_gradient_t` ramps.

**Tech Stack:** SynthUI + VGLite (vendored, `~/Development/VGLite`), LVGL 9.4 port, evkb QEMU gates, LinkServer + SW4 bench procedure (coordinator handles all SW4 presses via the user).

**Standing rules:** `./run_qemu.sh`, never `sh`. sw goldens must never move — the fader gate and knob gate are the tripwires, run after every task. The bench costs a human button-press per boot: batch everything measurable into single boots. `git -C ~/Development/SynthUI status` clean after every task.

---

### Task 1: core refactor — private header, registry, `gpu_pending` (SynthUI)

**Files:**
- Create: `~/Development/SynthUI/src/synthui_fader_private.h`
- Modify: `~/Development/SynthUI/src/synthui_fader.cpp`

- [ ] **Step 1: Write the private header**

Create `~/Development/SynthUI/src/synthui_fader_private.h`:

```c
/* synthui_fader_private.h - shared between the widget core and the
 * optional GPU compositor TU (src/vglite/). NOT part of the public API.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_FADER_PRIVATE_H
#define SYNTHUI_FADER_PRIVATE_H

#include "synthui_fader.h"
/* lv_obj_t by value needs the complete private type (the rotary's note). */
#include <lvgl_private.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct synthui_fader_t {
    lv_obj_t obj;
    float value;          /* 0..1; 1 = cap at the top */
    float press_anchor;   /* value at PRESSED (anchor-total drag) */
    float press_y;        /* screen y at PRESSED */
    uint32_t panel;
    uint8_t ticks;
    bool center;
    /* Set by DRAW_MAIN when the GPU hook is enabled: "my ground was painted
     * this frame, my content still needs compositing". Cleared by the
     * compositor. Never set for hidden/other-screen objects, because
     * DRAW_MAIN does not run for them (the rotary's contract). */
    bool gpu_pending;
    struct synthui_fader_t *prev, *next;   /* instance registry */
} synthui_fader_t;

/* Registry of live instances (constructor links, destructor unlinks) and the
 * one flag the GPU TU flips on successful synthui_fader_gpu_begin_deferred().
 * Both are defined in the core so the core never references GPU symbols; a
 * build without src/vglite/ simply leaves the flag false forever. */
extern synthui_fader_t *synthui_fader_list;
extern bool synthui_fader_gpu_enabled;

/* Value-independent geometry in viewBox units (width 100, vh = 100*H/W,
 * u = px per unit). false = degenerate size; nothing may be drawn. */
typedef struct {
    float u, vh, cap_h, top, travel;
} synthui_fader_geom_t;
bool  synthui_fader_geom(const synthui_fader_t *f, synthui_fader_geom_t *g,
                         lv_area_t *coords);
float synthui_fader_cap_y(const synthui_fader_geom_t *g, float value);

/* Resolve the palette for one instance from its LVGL state (shared by the
 * sw draw and the compositor so the two engines cannot disagree on color). */
typedef struct {
    uint32_t cap_top, cap_mid, cap_low, ticks, center;
    lv_opa_t gloss_opa;
} synthui_fader_palette_t;
void synthui_fader_palette(const synthui_fader_t *f,
                           synthui_fader_palette_t *p);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Refactor the core TU**

In `~/Development/SynthUI/src/synthui_fader.cpp`, apply exactly these changes (nothing else — the sw pixels must not move):

a) Replace the `#include <lvgl_private.h>` line and the local `synthui_fader_t`, `fd_geom_t`, and `synthui_fader_palette_t` struct definitions with `#include "synthui_fader_private.h"` (keep the `synthui_fader_math.h` and `<math.h>` includes). Delete the local `typedef struct { ... } synthui_fader_t;`, the local `fd_geom_t`, and the local palette typedef — they now come from the header.

b) Add the registry/flag definitions after the includes (mirroring the rotary's lines 22-23):

```c
synthui_fader_t *synthui_fader_list = NULL;
bool synthui_fader_gpu_enabled = false;
```

c) Rename and de-static the three shared helpers, updating every call site:
   - `static bool fd_geom(...)` → `bool synthui_fader_geom(const synthui_fader_t *f, synthui_fader_geom_t *g, lv_area_t *coords)` (body unchanged; the struct type renames from `fd_geom_t` to `synthui_fader_geom_t`).
   - `static float fd_cap_y(const fd_geom_t *g, float value)` → `float synthui_fader_cap_y(const synthui_fader_geom_t *g, float value)`.
   - `static void fd_palette(bool disabled, bool pressed, ...)` → keep it as the static worker, and ADD the exported resolver below it (the rotary's split):

```c
void synthui_fader_palette(const synthui_fader_t *f,
                           synthui_fader_palette_t *p)
{
    const lv_state_t st = lv_obj_get_state((const lv_obj_t *)&f->obj);
    fd_palette((st & LV_STATE_DISABLED) != 0,
               (st & LV_STATE_PRESSED) != 0, p);
}
```

   In `fd_draw`, replace the inline `lv_obj_get_state` + `fd_palette` pair with one `synthui_fader_palette(f, &pal);` call. Update the palette-seam comment above `fd_palette` to say the export now exists (its "it would move to a header then" promise is being kept — reword to "exported via synthui_fader_private.h for the GPU TU; fd_palette stays the pure state->colors core").

d) Constructor: initialise `f->gpu_pending = false;` and head-insert into the registry (copy the rotary's constructor lines 64-68, s/knob/fader/). Add a destructor (the rotary's lines 81-89 adapted) and register it in the class struct:

```c
static void fd_destructor(const lv_obj_class_t *cls, lv_obj_t *obj);
...
const lv_obj_class_t synthui_fader_class = {
    .base_class     = &lv_obj_class,
    .constructor_cb = fd_constructor,
    .destructor_cb  = fd_destructor,
    .event_cb       = fd_event,
    ...
```

```c
static void fd_destructor(const lv_obj_class_t *cls, lv_obj_t *obj)
{
    LV_UNUSED(cls);
    synthui_fader_t *f = (synthui_fader_t *)obj;
    if (f->prev) f->prev->next = f->next;
    else synthui_fader_list = f->next;
    if (f->next) f->next->prev = f->prev;
    f->prev = f->next = NULL;
}
```

e) The GPU gate at the top of `fd_draw` (the rotary's line 273 pattern):

```c
static void fd_draw(synthui_fader_t *f, lv_layer_t *layer)
{
    /* GPU mode: the compositor draws EVERYTHING (panel..cap) per rendered
     * area -- removing the per-widget draw-task churn is the entire point
     * (the sw floor diagnosis, gpu spec section 1). DRAW_MAIN only marks
     * the instance; LVGL paints the screen ground beneath. sw mode is
     * untouched, so every QEMU golden and the sw delta guards stand. */
    if (synthui_fader_gpu_enabled) { f->gpu_pending = true; return; }
    ...
```

- [ ] **Step 3: Prove the sw path did not move**

Run, in order (all must pass):
1. `~/Development/SynthUI/tests/run.sh` → three PASS lines.
2. `cd ~/Development/rt1170/evkb/examples/display/synthui_knob_test && cmake --build build && ./run_qemu.sh` → `PASS: SynthUI knob render verified`.
3. `cd ~/Development/rt1170/evkb/examples/display/synthui_fader_test && cmake --build build && ./run_qemu.sh` → `PASS: SynthUI fader render verified` with `fd_crc=0xAB66DE0D` — the refactor is pixel-neutral or this task FAILED.

- [ ] **Step 4: Commit (SynthUI)**

```bash
cd ~/Development/SynthUI && git add src/synthui_fader_private.h src/synthui_fader.cpp && git commit -m "fader: private header + registry + gpu_pending -- the rotary's two-TU seam, sw pixels unchanged (NEW-23 gpu)"
```

---

### Task 2: the GPU TU (SynthUI)

**Files:**
- Create: `~/Development/SynthUI/src/vglite/synthui_fader_gpu.h`
- Create: `~/Development/SynthUI/src/vglite/synthui_fader_gpu.cpp`

Read `src/vglite/synthui_rotary_knob_gpu.cpp` COMPLETELY first — the arena, `composite_minus`, deferred-target, and scissor machinery below are copied from it, and its ★ comments explain why each exists.

- [ ] **Step 1: Write the GPU header**

Create `~/Development/SynthUI/src/vglite/synthui_fader_gpu.h`:

```c
/* synthui_fader_gpu.h - opt-in GC355 compositor for synthui_fader.
 * Compiled only by import_evkb_synthui(VGLITE); without it the widget is
 * fully software and this header must not be included.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_FADER_GPU_H
#define SYNTHUI_FADER_GPU_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DEFERRED (double-buffered) mode ONLY -- the fader never had a live-scanout
 * compositor and never will (the rotary's scanout-flash finding; gpu spec
 * section 4). Call AFTER vg_lite_init() succeeded (app owns the chip-ID
 * probe -- vg_lite_init SPINS on absent hardware) and AFTER the LVGL display
 * exists. Arms the widgets (all drawing moves to the GPU); the app wires
 * compose_into() as the panel binding's pre-flip callback. Returns false --
 * and changes nothing -- on any failure. */
bool synthui_fader_gpu_begin_deferred(int32_t w, int32_t h,
                                      int32_t stride_bytes);
void synthui_fader_gpu_compose_into(uint8_t *framebuffer);

/* Cumulative count of vg_lite_* calls that did not return VG_LITE_SUCCESS.
 * A rejected draw paints nothing while everything else looks healthy, so
 * examples must print this and hardware transcripts must show 0. */
uint32_t synthui_fader_gpu_errors(void);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Write the GPU TU**

Create `~/Development/SynthUI/src/vglite/synthui_fader_gpu.cpp`. This is the complete intended content; where a vg_lite call signature differs from the vendored `~/Development/VGLite/inc/vg_lite.h`, fix minimally against the real header and report the deviation:

```c
/* synthui_fader_gpu.cpp - see synthui_fader_gpu.h.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Structure is synthui_rotary_knob_gpu.cpp's, simplified: every shape is
 * emitted per frame into the bump arena in viewBox units x16 (S32) with the
 * fader's CURRENT cap_y baked in, and ONE matrix per fader
 * (translate(coords) * scale(u/16)) maps them to the screen -- a fader has
 * no rotation, so the rotary's two-matrix AA lesson does not arise. Ticks
 * are batched into two DISJOINT multi-rect paths (bright / dim); disjoint
 * subpaths are winding-1 everywhere, unlike the OVERLAPPING subpaths that
 * were the rotary's one source of per-boot nondeterminism (its emit_track
 * comment) -- overlap stays banned here.
 * The cap gradient uses cached vg_lite_linear_gradient_t ramps (one per
 * band per palette state, built lazily) with a per-frame gradient matrix;
 * ramps are never rebuilt per frame (the NEW-12 per-frame-construction
 * lesson). No blits, so the 64-byte source-stride rule does not apply.
 * No D-cache maintenance, deliberately: the imxrt1176 core never enables
 * the D-cache. */
#include "synthui_fader_gpu.h"
#include "../synthui_fader_private.h"
#include <math.h>
#include <string.h>

extern "C" {
#include "vg_lite.h"
}

#define FD_FIX 16.0f

static vg_lite_buffer_t *s_cur_target = NULL;
static bool     s_begun = false;
static uint32_t s_err = 0;
#define GPU_TRY(call) do { if ((call) != VG_LITE_SUCCESS) s_err++; } while (0)

/* ---- per-frame path arena (the rotary's, sized for 16 faders) -----------
 * Worst case per fader: panel(17) + ticks up to 33 rects in two paths
 * (~540) + rod(45) + center(17) + shadow(45) + base(45) + 2 grad bands(34)
 * + groove(17) + 2 gloss(34) ~= 800 words; x16 ~= 12.8K. 16384 leaves
 * headroom; overflow is COUNTED and draws nothing (a truncated path set is
 * a wrong picture that still draws -- the bench rule). Reset only AFTER
 * vg_lite_finish, in case the driver references path data until submit. */
#define FD_ARENA_WORDS 16384
static int32_t s_arena[FD_ARENA_WORDS];
static size_t  s_used;
static bool    s_overflow;

static void emit(int32_t w)
{
    if (s_used < FD_ARENA_WORDS) s_arena[s_used++] = w;
    else s_overflow = true;
}
static int32_t fx(float f) { return (int32_t)lroundf(f * FD_FIX); }

/* axis-aligned rect contour, unit coords */
static void emit_rect(float x, float y, float w, float h)
{
    emit(VLC_OP_MOVE); emit(fx(x));     emit(fx(y));
    emit(VLC_OP_LINE); emit(fx(x + w)); emit(fx(y));
    emit(VLC_OP_LINE); emit(fx(x + w)); emit(fx(y + h));
    emit(VLC_OP_LINE); emit(fx(x));     emit(fx(y + h));
    emit(VLC_OP_CLOSE);
}
/* rounded rect: four quarter-circle corners as single cubics (r is small --
 * <= 2 units -- so one cubic per 90 degrees is visually exact). k = c*r,
 * c = 0.5523 (4/3*(sqrt(2)-1)). Clockwise from the top-left arc end. */
static void emit_round_rect(float x, float y, float w, float h, float r)
{
    const float k = 0.5523f * r;
    emit(VLC_OP_MOVE);  emit(fx(x + r));         emit(fx(y));
    emit(VLC_OP_LINE);  emit(fx(x + w - r));     emit(fx(y));
    emit(VLC_OP_CUBIC); emit(fx(x + w - r + k)); emit(fx(y));
                        emit(fx(x + w));         emit(fx(y + r - k));
                        emit(fx(x + w));         emit(fx(y + r));
    emit(VLC_OP_LINE);  emit(fx(x + w));         emit(fx(y + h - r));
    emit(VLC_OP_CUBIC); emit(fx(x + w));         emit(fx(y + h - r + k));
                        emit(fx(x + w - r + k)); emit(fx(y + h));
                        emit(fx(x + w - r));     emit(fx(y + h));
    emit(VLC_OP_LINE);  emit(fx(x + r));         emit(fx(y + h));
    emit(VLC_OP_CUBIC); emit(fx(x + r - k));     emit(fx(y + h));
                        emit(fx(x));             emit(fx(y + h - r + k));
                        emit(fx(x));             emit(fx(y + h - r));
    emit(VLC_OP_LINE);  emit(fx(x));             emit(fx(y + r));
    emit(VLC_OP_CUBIC); emit(fx(x));             emit(fx(y + r - k));
                        emit(fx(x + r - k));     emit(fx(y));
                        emit(fx(x + r));         emit(fx(y));
    emit(VLC_OP_CLOSE);
}
/* border ring: outer rounded contour + reversed inner sharp contour in one
 * path -- non-zero winding makes the donut, exactly like the rotary's
 * emit_ring. The two contours never overlap, so winding is 1 everywhere. */
static void emit_border_ring(float x, float y, float w, float h, float r,
                             float bw)
{
    emit_round_rect(x, y, w, h, r);
    /* inner, counter-clockwise (reversed) */
    const float ix = x + bw, iy = y + bw, iw = w - 2.0f * bw,
                ih = h - 2.0f * bw;
    emit(VLC_OP_MOVE); emit(fx(ix));      emit(fx(iy));
    emit(VLC_OP_LINE); emit(fx(ix));      emit(fx(iy + ih));
    emit(VLC_OP_LINE); emit(fx(ix + iw)); emit(fx(iy + ih));
    emit(VLC_OP_LINE); emit(fx(ix + iw)); emit(fx(iy));
    emit(VLC_OP_CLOSE);
}

static void finish_path(vg_lite_path_t *p, size_t start, float x0, float y0,
                        float x1, float y1)
{
    emit(VLC_OP_END);
    memset(p, 0, sizeof(*p));
    vg_lite_init_path(p, VG_LITE_S32, VG_LITE_HIGH,
                      (uint32_t)((s_used - start) * sizeof(int32_t)),
                      &s_arena[start],
                      x0 * FD_FIX, y0 * FD_FIX, x1 * FD_FIX, y1 * FD_FIX);
}

/* vg_lite_color_t is ABGR -- red in the LOW byte (vglite_probe, measured);
 * `a` carries the sw path's opa values so the two looks stay close. */
static uint32_t abgr_a(uint32_t hex, uint32_t a)
{
    return (a << 24) | ((hex & 0xFFu) << 16) | (hex & 0xFF00u)
           | ((hex >> 16) & 0xFFu);
}

/* ---- cached gradient ramps: one per band per palette state --------------
 * vg_lite_init_grad allocates the 256x1 ramp image from the vg pool ONCE;
 * only the grad MATRIX changes per frame (the ramp must not be rebuilt per
 * frame -- the NEW-12 lesson). Key: band 0 = capTop->capMid, band 1 =
 * capMid->capLow; state index 0 idle / 1 active / 2 disabled. */
static vg_lite_linear_gradient_t s_grads[2][3];
static bool s_grad_ready[2][3];

static vg_lite_linear_gradient_t *grad_get(int band, int stidx,
                                           uint32_t c_top, uint32_t c_bot)
{
    if (!s_grad_ready[band][stidx]) {
        vg_lite_linear_gradient_t *g = &s_grads[band][stidx];
        memset(g, 0, sizeof(*g));
        if (vg_lite_init_grad(g) != VG_LITE_SUCCESS) { s_err++; return NULL; }
        uint32_t cols[2] = { abgr_a(c_top, 0xFFu), abgr_a(c_bot, 0xFFu) };
        uint32_t stops[2] = { 0, 255 };
        GPU_TRY(vg_lite_set_grad(g, 2, cols, stops));
        GPU_TRY(vg_lite_update_grad(g));
        s_grad_ready[band][stidx] = true;
    }
    return &s_grads[band][stidx];
}

/* ---- one-composite-per-pixel machinery (the rotary's, verbatim shape) ---
 * LVGL only guarantees a damaged pixel is rendered AT LEAST once; two
 * surviving inv areas may overlap, and an SRC_OVER composite of antialiased
 * paths is not idempotent (found by the knob's equality guard on its FIRST
 * silicon run). Each area is composited MINUS everything already composited
 * for this fader. */
typedef struct {
    const synthui_fader_t *f;
    const synthui_fader_geom_t *g;
    const synthui_fader_palette_t *pal;
    const vg_lite_matrix_t *m;      /* unit*16 -> screen */
    float cap_y;                    /* units, this frame */
    int stidx;                      /* palette state index for grad cache */
} fd_gpu_ctx_t;

static void draw_fader_clipped(const fd_gpu_ctx_t *c, const lv_area_t *clip);

static void composite_minus(const fd_gpu_ctx_t *ctx, lv_area_t area,
                            const lv_area_t *done, int ndone)
{
    for (int i = 0; i < ndone; i++) {
        lv_area_t ix;
        if (!lv_area_intersect(&ix, &area, &done[i])) continue;
        lv_area_t piece;
        if (area.y1 < ix.y1) {
            piece.x1 = area.x1; piece.y1 = area.y1;
            piece.x2 = area.x2; piece.y2 = ix.y1 - 1;
            composite_minus(ctx, piece, done + i + 1, ndone - i - 1);
        }
        if (ix.y2 < area.y2) {
            piece.x1 = area.x1; piece.y1 = ix.y2 + 1;
            piece.x2 = area.x2; piece.y2 = area.y2;
            composite_minus(ctx, piece, done + i + 1, ndone - i - 1);
        }
        if (area.x1 < ix.x1) {
            piece.x1 = area.x1; piece.y1 = ix.y1;
            piece.x2 = ix.x1 - 1; piece.y2 = ix.y2;
            composite_minus(ctx, piece, done + i + 1, ndone - i - 1);
        }
        if (ix.x2 < area.x2) {
            piece.x1 = ix.x2 + 1; piece.y1 = ix.y1;
            piece.x2 = area.x2; piece.y2 = ix.y2;
            composite_minus(ctx, piece, done + i + 1, ndone - i - 1);
        }
        return;
    }
    /* lv_area x2/y2 inclusive; the driver's right/bottom exclusive. */
    GPU_TRY(vg_lite_set_scissor(area.x1, area.y1, area.x2 + 1, area.y2 + 1));
    draw_fader_clipped(ctx, &area);
}

/* Draw the fader's FULL content, clipped by the scissor already set.
 * Geometry mirrors the sw path's fd_draw (spec 2026-08-29 base, section 4);
 * pixel parity is NOT required (two golden sets, never reconciled) but the
 * shapes and draw order are the same so the looks stay close. */
static void draw_fader_clipped(const fd_gpu_ctx_t *c, const lv_area_t *clip)
{
    (void)clip;
    const synthui_fader_geom_t *g = c->g;
    const synthui_fader_palette_t *pal = c->pal;
    const float ch = g->cap_h, cy = c->cap_y, bw = 1.6f;
    vg_lite_path_t p;
    size_t start;

    /* panel */
    start = s_used; emit_rect(0.0f, 0.0f, 100.0f, g->vh);
    finish_path(&p, start, 0.0f, 0.0f, 100.0f, g->vh);
    GPU_TRY(vg_lite_draw(s_cur_target, &p, VG_LITE_FILL_NON_ZERO,
                         (vg_lite_matrix_t *)c->m, VG_LITE_BLEND_SRC_OVER,
                         abgr_a(c->f->panel, 0xFFu)));

    /* ticks: two disjoint multi-rect paths (bright i%4==0 at 158, dim 87);
     * tick_w in units, drawn as thin rects centred on the tick y. */
    const float tick_w = fmaxf(1.4f, 0.012f * g->vh);
    const int n = c->f->ticks;
    for (int pass = 0; pass < 2; pass++) {
        start = s_used;
        int emitted = 0;
        for (int i = 0; i < n; i++) {
            const bool bright = (i % 4 == 0);
            if (bright != (pass == 0)) continue;
            const float ty = g->top + ch * 0.5f
                             + (float)i * g->travel / (float)(n - 1);
            emit_rect(8.0f, ty - tick_w * 0.5f, 84.0f, tick_w);
            emitted++;
        }
        if (!emitted) { s_used = start; continue; }
        finish_path(&p, start, 8.0f, 0.0f, 92.0f, g->vh);
        GPU_TRY(vg_lite_draw(s_cur_target, &p, VG_LITE_FILL_NON_ZERO,
                             (vg_lite_matrix_t *)c->m, VG_LITE_BLEND_SRC_OVER,
                             abgr_a(pal->ticks, pass == 0 ? 158u : 87u)));
    }

    /* rod */
    start = s_used;
    emit_round_rect(46.5f, g->top + ch * 0.5f - 2.0f, 7.0f,
                    g->travel + 4.0f, 1.5f);
    finish_path(&p, start, 46.5f, 0.0f, 53.5f, g->vh);
    GPU_TRY(vg_lite_draw(s_cur_target, &p, VG_LITE_FILL_NON_ZERO,
                         (vg_lite_matrix_t *)c->m, VG_LITE_BLEND_SRC_OVER,
                         abgr_a(0x14181Bu, 0xFFu)));

    /* center-detent line */
    if (c->f->center) {
        const float cyl = g->top + ch * 0.5f + g->travel * 0.5f;
        start = s_used; emit_rect(4.0f, cyl - 1.2f, 92.0f, 2.4f);
        finish_path(&p, start, 4.0f, cyl - 1.2f, 96.0f, cyl + 1.2f);
        GPU_TRY(vg_lite_draw(s_cur_target, &p, VG_LITE_FILL_NON_ZERO,
                             (vg_lite_matrix_t *)c->m, VG_LITE_BLEND_SRC_OVER,
                             abgr_a(pal->center, 0xFFu)));
    }

    /* cap: shadow, base, grad x2, groove, gloss x2, border ring */
    start = s_used; emit_round_rect(6.0f, cy + 2.5f, 88.0f, ch, 2.0f);
    finish_path(&p, start, 6.0f, cy, 94.0f, cy + ch + 3.0f);
    GPU_TRY(vg_lite_draw(s_cur_target, &p, VG_LITE_FILL_NON_ZERO,
                         (vg_lite_matrix_t *)c->m, VG_LITE_BLEND_SRC_OVER,
                         abgr_a(0x1B1F22u, 115u)));

    start = s_used; emit_round_rect(4.0f, cy, 88.0f, ch, 2.0f);
    finish_path(&p, start, 4.0f, cy, 92.0f, cy + ch);
    GPU_TRY(vg_lite_draw(s_cur_target, &p, VG_LITE_FILL_NON_ZERO,
                         (vg_lite_matrix_t *)c->m, VG_LITE_BLEND_SRC_OVER,
                         abgr_a(pal->cap_mid, 0xFFu)));

    /* gradient bands, inset inside the border; the grad matrix maps the
     * 256x1 ramp along +x, so: fader matrix, then translate to the band
     * origin (x16 fixed units), rotate 90 (ramp runs down), scale the 256
     * ramp length onto the band height. VERIFY against vg_lite.h and the
     * first silicon eyeball -- orientation is the one blind spot (gpu spec
     * section 10); the fallback is N solid interpolated strips. */
    const struct { float y0, h; int band; uint32_t top, bot; } bands[2] = {
        { cy + bw,          0.46f * ch - bw, 0, pal->cap_top, pal->cap_mid },
        { cy + 0.46f * ch,  0.54f * ch - bw, 1, pal->cap_mid, pal->cap_low },
    };
    for (int b = 0; b < 2; b++) {
        vg_lite_linear_gradient_t *gr = grad_get(bands[b].band, c->stidx,
                                                 bands[b].top, bands[b].bot);
        if (gr == NULL) continue;
        start = s_used;
        emit_rect(4.0f + bw, bands[b].y0, 88.0f - 2.0f * bw, bands[b].h);
        finish_path(&p, start, 4.0f + bw, bands[b].y0,
                    92.0f - bw, bands[b].y0 + bands[b].h);
        vg_lite_matrix_t *gm = vg_lite_get_grad_matrix(gr);
        *gm = *(vg_lite_matrix_t *)c->m;
        vg_lite_translate((4.0f + bw) * FD_FIX, bands[b].y0 * FD_FIX, gm);
        vg_lite_rotate(90.0f, gm);
        vg_lite_scale(bands[b].h * FD_FIX / 256.0f, 1.0f, gm);
        GPU_TRY(vg_lite_draw_grad(s_cur_target, &p, VG_LITE_FILL_NON_ZERO,
                                  (vg_lite_matrix_t *)c->m, gr,
                                  VG_LITE_BLEND_SRC_OVER));
    }

    start = s_used; emit_rect(4.0f, cy + 0.43f * ch, 88.0f, 0.14f * ch);
    finish_path(&p, start, 4.0f, cy, 92.0f, cy + ch);
    GPU_TRY(vg_lite_draw(s_cur_target, &p, VG_LITE_FILL_NON_ZERO,
                         (vg_lite_matrix_t *)c->m, VG_LITE_BLEND_SRC_OVER,
                         abgr_a(0x20262Au, 0xFFu)));

    const float gh = fmaxf(1.5f, 0.12f * ch);
    for (int s = 0; s < 2; s++) {
        const float gy = cy + (s ? 0.68f : 0.16f) * ch;
        start = s_used; emit_rect(9.0f, gy, 78.0f, gh);
        finish_path(&p, start, 9.0f, gy, 87.0f, gy + gh);
        GPU_TRY(vg_lite_draw(s_cur_target, &p, VG_LITE_FILL_NON_ZERO,
                             (vg_lite_matrix_t *)c->m, VG_LITE_BLEND_SRC_OVER,
                             abgr_a(0xFFFFFFu, pal->gloss_opa)));
    }

    start = s_used; emit_border_ring(4.0f, cy, 88.0f, ch, 2.0f, bw);
    finish_path(&p, start, 4.0f, cy, 92.0f, cy + ch);
    GPU_TRY(vg_lite_draw(s_cur_target, &p, VG_LITE_FILL_NON_ZERO,
                         (vg_lite_matrix_t *)c->m, VG_LITE_BLEND_SRC_OVER,
                         abgr_a(0x20262Au, 0xFFu)));
}

/* One composite pass over every pending instance, into *s_cur_target. */
static void compose_pass(void)
{
    lv_display_t *disp = lv_display_get_default();
    bool drew = false;
    for (synthui_fader_t *f = synthui_fader_list; f; f = f->next) {
        if (!f->gpu_pending) continue;
        f->gpu_pending = false;
        lv_area_t coords; synthui_fader_geom_t g;
        if (!synthui_fader_geom(f, &g, &coords)) continue;
        synthui_fader_palette_t pal;
        synthui_fader_palette(f, &pal);
        const lv_state_t st = lv_obj_get_state(&f->obj);
        const int stidx = (st & LV_STATE_DISABLED) ? 2
                        : (st & LV_STATE_PRESSED)  ? 1 : 0;
        vg_lite_matrix_t m; vg_lite_identity(&m);
        vg_lite_translate((float)coords.x1, (float)coords.y1, &m);
        vg_lite_scale(g.u / FD_FIX, g.u / FD_FIX, &m);
        const fd_gpu_ctx_t ctx = { f, &g, &pal, &m,
                                   synthui_fader_cap_y(&g, f->value), stidx };
        lv_area_t done[LV_INV_BUF_SIZE];
        int ndone = 0;
        for (uint32_t i = 0; i < disp->inv_p; i++) {
            if (disp->inv_area_joined[i]) continue;    /* merged, not drawn */
            lv_area_t clip;
            if (!lv_area_intersect(&clip, &disp->inv_areas[i], &coords))
                continue;                              /* not this fader */
            composite_minus(&ctx, clip, done, ndone);
            done[ndone++] = clip;
            drew = true;
        }
    }
    if (drew) {
        GPU_TRY(vg_lite_set_scissor(-1, -1, -1, -1));  /* disable */
        /* Retire before anyone (checksums, scanout) touches the buffer. */
        GPU_TRY(vg_lite_finish());
    }
    /* Reclaim the per-frame paths -- only AFTER finish, in case the driver
     * references (rather than copied) the path data until submit. */
    s_used = 0;
    s_overflow = false;
}

uint32_t synthui_fader_gpu_errors(void) { return s_err; }

/* ---- deferred (double-buffered) mode ---- */
static int32_t s_def_w, s_def_h, s_def_stride;
static vg_lite_buffer_t s_def_targets[2];
static void *s_def_ptrs[2];
static int s_def_n = 0;

bool synthui_fader_gpu_begin_deferred(int32_t w, int32_t h,
                                      int32_t stride_bytes)
{
    if (s_begun) return true;
    s_def_w = w; s_def_h = h; s_def_stride = stride_bytes;
    s_def_n = 0;
    s_used = 0; s_overflow = false;
    memset(s_grad_ready, 0, sizeof(s_grad_ready));
    synthui_fader_gpu_enabled = true;
    s_begun = true;
    return true;
}

void synthui_fader_gpu_compose_into(uint8_t *framebuffer)
{
    if (!synthui_fader_gpu_enabled || framebuffer == NULL) return;
    /* lazy wrap+map: a flip display alternates between exactly two buffers */
    int i;
    for (i = 0; i < s_def_n; i++)
        if (s_def_ptrs[i] == framebuffer) break;
    if (i == s_def_n) {
        if (s_def_n >= 2) { s_err++; return; }   /* a third buffer is a bug */
        vg_lite_buffer_t *t = &s_def_targets[s_def_n];
        memset(t, 0, sizeof(*t));
        t->width   = s_def_w;
        t->height  = s_def_h;
        t->stride  = s_def_stride;
        t->tiled   = VG_LITE_LINEAR;
        t->format  = VG_LITE_BGRA8888;   /* = panel XRGB8888 memory order */
        t->memory  = framebuffer;
        t->address = (uint32_t)(uintptr_t)framebuffer;
        /* REGISTER the target with the driver or every draw "succeeds" and
         * changes nothing (vglite_probe, measured on silicon). */
        if (vg_lite_map(t, VG_LITE_MAP_USER_MEMORY, 0) != VG_LITE_SUCCESS) {
            s_err++;
            return;
        }
        s_def_ptrs[s_def_n++] = framebuffer;
    }
    s_cur_target = &s_def_targets[i];
    compose_pass();
    s_cur_target = NULL;
}
```

NOTE on the arena/overflow contract: unlike the rotary (which pre-builds
static paths), every fader path is per-frame, so `s_used` resets to 0 in
`compose_pass` after finish. If `s_overflow` is ever true at the end of
`draw_fader_clipped`, count it: add at the end of `compose_pass`, before the
reset: `if (s_overflow) s_err++;`.

- [ ] **Step 3: Compile via the knob test's VGLITE build + prove inertness**

Run: `cd ~/Development/rt1170/evkb/examples/display/synthui_knob_test && cmake --build build 2>&1 | tail -3` — the VGLITE-flavored glob picks up the new TU. Fix compile errors minimally against the real `vg_lite.h`; report every deviation.
Then `./run_qemu.sh` → knob gate PASS (fader GPU TU linked but inert).
Then `cd ../synthui_fader_test && ./run_qemu.sh` → fader gate PASS (its build has no VGLITE yet; the core's `synthui_fader_gpu_enabled` stays false).

- [ ] **Step 4: Commit (SynthUI)**

```bash
cd ~/Development/SynthUI && git add src/vglite/synthui_fader_gpu.h src/vglite/synthui_fader_gpu.cpp && git commit -m "fader: GC355 deferred compositor -- per-frame unit-space paths, cached gradient ramps, composite-minus (NEW-23 gpu)"
```

---

### Task 3: example wiring + gate tripwires (evkb)

**Files:**
- Modify: `examples/display/synthui_fader_test/CMakeLists.txt`
- Modify: `examples/display/synthui_fader_test/synthui_fader_test.cpp`
- Modify: `examples/display/synthui_fader_test/run_qemu.sh`
- Recapture: `examples/display/synthui_fader_test/transcript_qemu.txt`

- [ ] **Step 1: CMakeLists — switch to the VGLITE flavor**

Change `import_evkb_synthui()` to `import_evkb_synthui(VGLITE)` (update its comment: the fader now has the opt-in compositor; LVGL stays the SOFTWARE renderer — one ELF, both engines, the knob test's pattern) and add `VGLite` to the `.elf` link line: `target_link_libraries(synthui_fader_test.elf SynthUI LVGL VGLite stdc++)`.

- [ ] **Step 2: Sketch wiring (mirror the knob test's setup block)**

In `synthui_fader_test.cpp`:

a) Add after the existing includes:

```c
#include "synthui_fader_gpu.h"

extern "C" {
#include "vg_lite.h"
#include "vg_lite_platform.h"
uint32_t vg_lite_os_irq_count(void);
uint32_t vg_lite_os_wait_timeouts(void);
}

/* Same pool siting and reasoning as the knob test: EXTMEM (SDRAM), not
 * DMAMEM -- a 2 MB pool overflows OCRAM at link time, and the GPU reaches
 * SDRAM as a bus master exactly as it reaches the framebuffer. */
#define VGLITE_POOL_BYTES (2u * 1024u * 1024u)
EXTMEM __attribute__((aligned(64))) static uint8_t vglite_pool[VGLITE_POOL_BYTES];
#define TESS_W 256
#define TESS_H 256

static bool s_gpu = false;
```

b) In `setup()`, after `Display.fillScreen(0x0000);` and BEFORE `lvgl_rt1176_begin();`, insert the probe (knob test lines 361-373, fader tokens):

```c
    /* ASK BEFORE COMMITTING (vglite_probe): vg_lite_init() SPINS on absent
     * hardware, so the chip-ID read is what makes QEMU a clean negative.
     * Zero the GPU working pool: EXTMEM is never zeroed by startup. */
    memset(vglite_pool, 0, VGLITE_POOL_BYTES);
    vg_lite_init_mem(VGLITE_RT1176_REGISTER_BASE, 0u, vglite_pool,
                     VGLITE_POOL_BYTES);
    const uint32_t chip_id = vg_lite_hal_probe_chip_id();
    Serial1.printf("FD_CHIP_ID=0x%08lX\n", (unsigned long)chip_id);
    const bool vg_up =
        (chip_id != 0u) && (vg_lite_init(TESS_W, TESS_H) == VG_LITE_SUCCESS);
```

c) After `lvgl_mipi_panel_create_db(Display);`, insert:

```c
    /* Attach the compositor only when the GPU is genuinely up; a false here
     * leaves the widget fully software -- the honest negative the gate pins. */
    if (vg_up) {
        s_gpu = synthui_fader_gpu_begin_deferred(
                    Display.width(), Display.height(),
                    Display.width() * PANEL_BYTES_PER_PIXEL);
        if (s_gpu)
            lvgl_mipi_panel_set_preflip_cb(synthui_fader_gpu_compose_into);
    }
    Serial1.printf("fd_engine=%s\n", s_gpu ? "gpu" : "sw");
```

d) Just before `Serial1.println("crc_done");`, insert (knob pattern — gpu lines NEVER appear in a sw run):

```c
    if (s_gpu) {
        Serial1.printf("fd_gpu_err=%lu\n",
                       (unsigned long)synthui_fader_gpu_errors());
        Serial1.printf("fd_gpu_diag irqs=%lu wait_timeouts=%lu\n",
                       (unsigned long)vg_lite_os_irq_count(),
                       (unsigned long)vg_lite_os_wait_timeouts());
    }
```

e) The fps summary already prints engine-agnostic numbers; append the engine to both fps lines' format: add ` engine=%s` and pass `s_gpu ? "gpu" : "sw"` (matches `rk_fps`).

- [ ] **Step 3: Build + QEMU run**

```bash
cd examples/display/synthui_fader_test && cmake --build build
```
Then one QEMU boot to `crc_done` via `timeout 40 ../../../tools/rt1170-qemu.sh build/synthui_fader_test.elf`: expect `FD_CHIP_ID=0x00000000`, `fd_engine=sw`, and EVERY checksum unchanged (`fd_crc=0xAB66DE0D`, delta pair equal, `fd_damage max=3234`). If any golden moved, STOP — the sw path was touched, which Task 1/2 forbade.

- [ ] **Step 4: Gate additions**

In `run_qemu.sh`, after the `LVGL_BYTES` check, add:

```sh
# Engine honesty: QEMU has no GC355, so the run must SAY software. A GPU
# claim with no GPU present -- or the gpu error counter appearing at all --
# must fail by name (the knob gate's tripwire discipline).
grep -qE "fd_engine=sw\r?$" "$OUT" || { echo "FAIL: engine line missing or not sw"; exit 1; }
grep -q  "fd_engine=gpu" "$OUT" && { echo "FAIL: TRIPWIRE gpu engine claimed in QEMU"; exit 1; }
grep -q  "fd_gpu_err="   "$OUT" && { echo "FAIL: TRIPWIRE gpu error counter in QEMU"; exit 1; }
```

- [ ] **Step 5: Demonstrate the tripwires RED**

Run the gate green first, then demonstrate each tripwire against a REAL gate run (the knob gate's method — a fabricated line in the firmware's own output, not a hand-edited capture):

1. In `synthui_fader_test.cpp`, temporarily add `Serial1.println("fd_engine=gpu");` immediately after the genuine `fd_engine=` print. Rebuild, `./run_qemu.sh`.
   Expected: `FAIL: TRIPWIRE gpu engine claimed in QEMU`, exit 1. Remove the line, rebuild, re-run → green.
2. Same again with `Serial1.println("fd_gpu_err=0");` in the same place. Rebuild, run.
   Expected: `FAIL: TRIPWIRE gpu error counter in QEMU`, exit 1. Remove, rebuild, re-run → green.
3. Temporarily change the genuine print to `s_gpu ? "gpu" : "swx"`, rebuild, run.
   Expected: `FAIL: engine line missing or not sw`, exit 1. Revert, rebuild, re-run → green.

Record all three quotes in the gate header comment above the tripwire block, dated, in the form the knob gate uses ("Demonstrated RED 2026-08-29: ... -> \"FAIL: ...\"").

- [ ] **Step 6: Recapture fixture + vacuity**

```bash
./run_qemu.sh && cp build/synthui_fader.uart transcript_qemu.txt
cd ../../.. && ./tools/gate-vacuity.test.sh
```
The fixture MUST be recaptured (the gate now requires `fd_engine=sw`, which the old fixture lacks — green-replay would fail on the stale fixture, which is the freshness lesson working). Vacuity: all cases PASS including the three fader ones against the new fixture.

- [ ] **Step 7: Commit (evkb)**

```bash
git add examples/display/synthui_fader_test && git commit -m "synthui_fader_test: two-engine wiring -- chip probe, deferred compositor, engine tripwires demonstrated RED (NEW-23 gpu)"
```

---

### Task 4: silicon — gpu goldens, equality guard, fps acceptance

The coordinator drives the bench (flash + console); the user presses SW4. This task is executed by the COORDINATOR, not a subagent — each boot costs a human press.

- [ ] **Step 1: Flash (VCOM free), attach console, first boot.** Procedure as before (pkill daemons; `LinkServer flash ... load` + `verify`; attach `rt1170-console.py`; user presses SW4).

- [ ] **Step 2: First-boot checks, in order:**
1. `fd_engine=gpu`, `FD_CHIP_ID` non-zero.
2. **Eyeball the glass NOW, before trusting numbers**: bank renders correctly — panels, ticks, rod, center lines, caps with gradient (top light → mid → low dark), groove, gloss, border. The gradient ORIENTATION is the known blind spot: if bands run sideways or are solid, stop and fix the grad matrix (fallback: N interpolated solid strips per band). Compare against the sw render from memory/photo — same design, minor AA differences expected.
3. `fd_delta_eq=PASS` — the equality guard on the gpu path. If FAIL: this is the guard doing its job (it caught all three knob silicon defects); debug the compositor (scissor, composite-minus, AA idempotency), do NOT weaken anything.
4. `fd_gpu_err=0`, `fd_gpu_diag` sane, `fd_vsync ... timeouts=0`, `fd_damage max=3234` (damage is engine-shared).
5. `fd_fps ... engine=gpu`: **`mfps_med >= 30000`** is the acceptance. Record `fps_avg`, `fd_probe_delta` (fence health), `fd_fps_fullinv`, and the Phase C probes.

- [ ] **Step 3: Repeated-boot stability.** Two more SW4 presses; all gpu checksums (`fd_crc`, `fd_delta_crc`, `fd_fresh_crc`) bit-identical across the three boots. One knob defect hid behind exactly one boot.

- [ ] **Step 4: Eyes/camera pass** during the endless animation: no flashes, no torn caps, no stale slivers (deferred compose is tear-free by construction — verify anyway; checksums cannot see scanout).

- [ ] **Step 5: Commit the evidence.** Replace `transcript_hw_evkb.txt` with the gpu-era transcript (three boots' token blocks + fps lines; keep the sw-era 11 fps transcript content beneath a separator as the before/after record). Commit with the numbers in the message.

STOP-rule: `mfps_med < 30000` → record everything, commit the honest transcript, stop (the next lever is the platform issue, not more compositor work).

---

### Task 5: close-out

- [ ] **Step 1: Push SynthUI + LVGL, bump BOTH pins.**
```bash
cd ~/Development/SynthUI && git push origin master && git rev-parse HEAD
cd ~/Development/LVGL && git push origin master && git rev-parse HEAD
```
In `evkb.cmake`: SynthUI pin → new SHA, comment prepend `Bumped 2026-08-29 (fader GC355 compositor, NEW-23; earlier: ...)`. LVGL pin → new SHA (the flip_sync wait counter), comment note `flip_sync wait_us counter (NEW-23 diagnosis)`.

- [ ] **Step 2: Fresh-fetch gate-run verify** (both libraries fetched):
```bash
cd examples/display/synthui_fader_test
cmake -B build-fetch -DEVKB_FORCE_FETCH=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build-fetch
mv build build-local && ln -s build-fetch build; ./run_qemu.sh; RC=$?; rm build && mv build-local build; rm -rf build-fetch
[ $RC -eq 0 ] && echo FETCH-VERIFY-PASS
```

- [ ] **Step 3: Final trio.** Sweep via a verified symlink (`readlink /tmp/fd23` first) → `gates: 123 passed` modulo the two documented standing reds (re-disposition if present); vacuity suite all-PASS; license audit PASS (`LICENSE_AUDIT_EVKB=$(pwd)`, ~10 min).

- [ ] **Step 4: CLAUDE.md.** In the NEW-23 paragraph add the outcome sentence (gpu compositor landed, mfps_med number, sw floor diagnosis pointer). New measured-sweep entry only if the sweep result differs from 2026-08-29's.

- [ ] **Step 5: Commit + push evkb**; update the base + gpu specs' Status lines if needed.

- [ ] **Step 6: Linear.** NEW-23: comment with the gpu numbers + three-boot stability + camera pass; move to Done if `mfps_med ≥ 30000` and the tear-free/double-buffered criteria hold. CREATE the platform issue: "LVGL sw floor: uncached draw-task churn (~90 µs/task); pool relocation faults usb_init" — body carries the Phase C probe table, the pool→DTCM BusFault repro (lv_conf edits + the fault signature CFSR=0x0400 in main→usb_init), and the three candidate fixes (safe pool relocation, D-cache + DMA coherence, LVGL task batching).

- [ ] **Step 7: Memory.** Update `new23-synthui-fader.md` with the outcome; add the platform-issue pointer.

---

## Plan self-review notes

- The GPU TU's gradient-matrix construction is the one section flagged VERIFY-on-silicon (Task 4 Step 2.2) with a named fallback — everything else transplants proven rotary machinery.
- Token names cross-checked: `fd_engine`, `fd_gpu_err`, `fd_gpu_diag`, `FD_CHIP_ID` between sketch (Task 3) and gate (Task 3 Step 4).
- sw-neutrality is asserted three times (Task 1 Step 3, Task 2 Step 3, Task 3 Step 3) because three different changes could each break it independently.
- Fixture recapture ordering (Task 3 Step 6) is load-bearing: the gate gains an assertion the old fixture cannot satisfy.
