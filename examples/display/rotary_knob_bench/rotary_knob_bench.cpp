/* rotary_knob_bench - RotaryKnob render-strategy bench (12 cells).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec: docs/superpowers/specs/2026-08-27-rotary-knob-bench-design.md
 *
 * ONE ELF, BOTH ENGINES. LVGL is ALWAYS the software renderer here
 * (LV_USE_DRAW_VG_LITE stays 0 -- see CMakeLists for why that is what makes a
 * single binary safe); the GC355 is reached only by direct vg_lite_* calls
 * guarded by the chip-ID probe. QEMU has no GC355, so there every gpu cell
 * reports st=gpu-absent -- asserted by the gate, never a silent skip.
 */
#include <Arduino.h>
#include <string.h>
#include <math.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "rk_geometry.h"

extern "C" {
#include "vg_lite.h"
#include "vg_lite_platform.h"
}

/* Same pool siting and reasoning as vglite_probe: EXTMEM (SDRAM), not DMAMEM
 * -- a 2 MB pool overflows the 512K OCRAM at link time, and the GPU reaches
 * SDRAM as a bus master exactly as it reaches the framebuffer. */
#define VGLITE_POOL_BYTES (2u * 1024u * 1024u)
EXTMEM __attribute__((aligned(64))) static uint8_t vglite_pool[VGLITE_POOL_BYTES];
#define TESS_W 256
#define TESS_H 256

static bool s_gpu = false;
static vg_lite_buffer_t s_target;     /* the panel framebuffer, mapped */

/* ---- cell table ---------------------------------------------------------- */
typedef enum { RKB_VECTOR = 0, RKB_BITMAP, RKB_STRIP } rkb_strat_t;
typedef struct { rkb_strat_t strat; bool gpu; rkg_variant_t var; } rkb_cell_t;

static const rkb_cell_t CELLS[12] = {
    { RKB_VECTOR, false, RKG_NOTCH }, { RKB_VECTOR, false, RKG_FACET },
    { RKB_VECTOR, true,  RKG_NOTCH }, { RKB_VECTOR, true,  RKG_FACET },
    { RKB_BITMAP, false, RKG_NOTCH }, { RKB_BITMAP, false, RKG_FACET },
    { RKB_BITMAP, true,  RKG_NOTCH }, { RKB_BITMAP, true,  RKG_FACET },
    { RKB_STRIP,  false, RKG_NOTCH }, { RKB_STRIP,  false, RKG_FACET },
    { RKB_STRIP,  true,  RKG_NOTCH }, { RKB_STRIP,  true,  RKG_FACET },
};
static const char *STRAT_NAME[3]  = { "vector", "bitmap", "strip" };
static const char *VAR_NAME[2]    = { "notch", "facet" };
#define ENGINE_NAME(c) ((c)->gpu ? "gpu" : "sw")

/* ---- per-knob state and scene -------------------------------------------- */
typedef struct { lv_obj_t *obj; float angle; float cx, cy; } rkb_knob_t;
static rkb_knob_t g_knob[16];
static const rkb_cell_t *g_cell = NULL;      /* active cell, NULL = none */

/* ---- rotor/strip arenas (SDRAM; rebuilt at each cell's init) -------------
 * 150 ROWS of RKB_ROTOR_STRIDE_PX (160) pixels, not 150x150: the GC355 refuses
 * a blit source whose stride is not 64-B aligned (see rk_geometry.h). The
 * padded frame is 96,000 B, itself 64-B aligned, so every strip frame -- not
 * just the first -- starts on a 64-B boundary. At the old 90,000-B pitch 63 of
 * the 64 frames sat 16 bytes off. */
/* ALLOCATED words per frame (150 rows x 160), not the 22,500 visible pixels --
 * the name is the warning, since the two differ by the stride padding. */
#define ROTOR_WORDS   (RKB_KNOB_PX * RKB_ROTOR_STRIDE_PX)
#define ROTOR_B    RKB_ROTOR_BYTES
EXTMEM __attribute__((aligned(64))) static uint32_t g_rotor[ROTOR_WORDS];
EXTMEM __attribute__((aligned(64))) static uint32_t g_strip[RKB_STRIP_N][ROTOR_WORDS];

static lv_image_dsc_t g_rotor_dsc;
static lv_image_dsc_t g_strip_dsc[RKB_STRIP_N];
static vg_lite_buffer_t g_rotor_vgbuf;
static vg_lite_buffer_t g_strip_vgbuf[RKB_STRIP_N];

/* vector/gpu cached paths */
static vg_lite_path_t g_vg_paths[RKG_VG_MAX_PATHS];
static uint32_t       g_vg_colors[RKG_VG_MAX_PATHS];
static int            g_vg_npaths = 0;

static uint32_t g_init_us = 0, g_rotor_bytes = 0;

/* ---- gpu error latch ----------------------------------------------------
 * ★ COUNT of failed vg_lite_* calls in the current cell (map/draw/blit/finish),
 * reset at cell start. A COUNT rather than a first-error code, because the
 * failure mode this exists to catch is uniform -- e.g. a stride the driver
 * rejects fails all 16 blits identically -- and a count says whether it was the
 * whole cell or one knob. Reported as gpu_err= on gpu lines ONLY.
 * Without it a rejected blit is INVISIBLE: vg_lite returns an error, nothing
 * reads it, the framebuffer keeps the software-drawn well, and the cell posts a
 * superb time for having drawn no rotors. That is the single most dangerous
 * outcome a render benchmark can have, so it must be a named number. */
static uint32_t g_gpu_err = 0;
#define GPU_TRY(call) do { if ((call) != VG_LITE_SUCCESS) g_gpu_err++; } while (0)

/* ---- widget draw: LV_EVENT_DRAW_MAIN on a plain lv_obj ------------------- */
static void knob_draw_cb(lv_event_t *e)
{
    rkb_knob_t *k = (rkb_knob_t *)lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    lv_area_t coords; lv_obj_get_coords(obj, &coords);
    const float cx = (float)coords.x1 + RKB_KNOB_PX * 0.5f;
    const float cy = (float)coords.y1 + RKB_KNOB_PX * 0.5f;

    /* The well is common SW code in EVERY cell -- the constant addend that
     * keeps the A/B about the rotor strategy alone (spec section 3). */
    rkg_draw_well_sw(layer, cx, cy, RKB_KNOB_S);
    if (g_cell == NULL || g_cell->gpu) return;   /* gpu rotor: post-refresh pass */

    switch (g_cell->strat) {
    case RKB_VECTOR:
        rkg_draw_rotor_sw(layer, g_cell->var, cx, cy, RKB_KNOB_S, k->angle);
        break;
    case RKB_BITMAP: {
        lv_draw_image_dsc_t d; lv_draw_image_dsc_init(&d);
        d.src = &g_rotor_dsc;
        /* ★ 0.1 deg units -- LVGL's sw rotation QUANTIZES here (the 5.625 deg
         * animation step becomes 5.6) while the gpu path's vg_lite_rotate takes
         * full float. So in Phase B the two engines legitimately draw slightly
         * different pictures, and no checksum may ever be compared ACROSS
         * engines at a Phase B angle. Phase A is unaffected: RKB_CANON_DEG is
         * 45.0, which quantizes exactly. */
        d.rotation = (int32_t)lroundf(k->angle * 10.0f);
        d.pivot.x = RKB_KNOB_PX / 2; d.pivot.y = RKB_KNOB_PX / 2;
        d.antialias = 1;
        lv_draw_image(layer, &d, &coords);
        break;
    }
    case RKB_STRIP: {
        const int idx = ((int)lroundf(k->angle / RKB_STEP_DEG))
                        & (RKB_STRIP_N - 1);
        lv_draw_image_dsc_t d; lv_draw_image_dsc_init(&d);
        d.src = &g_strip_dsc[idx];
        lv_draw_image(layer, &d, &coords);
        break;
    }
    }
}

/* ---- gpu rotor pass (post-REFR_READY, inside the timed frame) ------------ */
static void gpu_rotor_pass(void)
{
    for (int k = 0; k < 16; k++) {
        vg_lite_matrix_t m; vg_lite_identity(&m);
        vg_lite_translate(g_knob[k].cx, g_knob[k].cy, &m);
        /* ★ THE ROTATE BELONGS TO THE STRATEGY, NOT TO THE LOOP. It used to sit
         * here, above the switch, which silently broke the strip cells in BOTH
         * ways that matter -- see the RKB_STRIP case. */
        switch (g_cell->strat) {
        case RKB_VECTOR:
            vg_lite_rotate(g_knob[k].angle, &m);
            vg_lite_scale(RKB_KNOB_S / 16.0f, RKB_KNOB_S / 16.0f, &m);
            for (int p = 0; p < g_vg_npaths; p++)
                GPU_TRY(vg_lite_draw(&s_target, &g_vg_paths[p],
                                     VG_LITE_FILL_NON_ZERO, &m,
                                     VG_LITE_BLEND_SRC_OVER, g_vg_colors[p]));
            break;
        case RKB_BITMAP:
            /* One frame, rotated live: this IS the strategy. BI_LINEAR because
             * a real rotation lands between source texels. */
            vg_lite_rotate(g_knob[k].angle, &m);
            vg_lite_translate(-(RKB_KNOB_PX * 0.5f), -(RKB_KNOB_PX * 0.5f), &m);
            GPU_TRY(vg_lite_blit(&s_target, &g_rotor_vgbuf, &m,
                                 VG_LITE_BLEND_SRC_OVER, 0,
                                 VG_LITE_FILTER_BI_LINEAR));
            break;
        case RKB_STRIP: {
            const int idx = ((int)lroundf(g_knob[k].angle / RKB_STEP_DEG))
                            & (RKB_STRIP_N - 1);
            /* ★ NO rotate: the frame IS the rotation (spec section 3,
             * "identity matrix"). Rotating a pre-rotated frame both doubles the
             * angle and reintroduces the resample the filmstrip exists to
             * avoid. The second error is the dangerous one -- it would have
             * made strip/gpu perform exactly bitmap/gpu's work, and the bench
             * would have concluded the filmstrip buys nothing on the GPU.
             * The sw side never had this bug (d.rotation stays 0 there), and
             * that asymmetry is the tell. */
            vg_lite_translate(-(RKB_KNOB_PX * 0.5f), -(RKB_KNOB_PX * 0.5f), &m);
            /* ★ FILTER_POINT, and it is now HONEST rather than merely cheaper:
             * with the rotate gone the transform is a pure translate by
             * integers (every knob centre is an integer and the 75 px offset is
             * exact), so source texels land on destination pixels 1:1 and there
             * is nothing to interpolate. This is what declares section 4's
             * "zero interpolation" in code instead of in prose. */
            GPU_TRY(vg_lite_blit(&s_target, &g_strip_vgbuf[idx], &m,
                                 VG_LITE_BLEND_SRC_OVER, 0,
                                 VG_LITE_FILTER_POINT));
            break;
        }
        }
    }
    /* Retire before anyone reads the framebuffer -- checksumming earlier
     * would race the hardware (vglite_probe).
     * ★ NO D-CACHE MAINTENANCE IS NEEDED between the GPU's writes and the CPU's
     * checksum, for the same reason lvgl_mipi_panel.cpp:64-78 gives for its
     * flush: the imxrt1176 core never writes SCB_CCR, so SDRAM is uncached on
     * this part and arm_dcache_* are no-ops. (The teensy4 core DOES enable the
     * D-cache -- if this example is ever ported to rt1062, this line and the
     * panel's flush_cb become one change, not two.) */
    GPU_TRY(vg_lite_finish());
}

/* ---- cell lifecycle ------------------------------------------------------ */
static void vg_wrap_argb(vg_lite_buffer_t *b, uint32_t *px)
{
    memset(b, 0, sizeof(*b));
    b->width = RKB_KNOB_PX; b->height = RKB_KNOB_PX;
    /* ★ 640, NOT 600 -- srcbuf_align_check rejects a BGRA8888 source whose
     * stride is not 16 px x 4 B aligned and the blit then draws nothing while
     * still timing fast (VGLite/vg_lite.c:1854-1861; see rk_geometry.h). */
    b->stride = RKB_ROTOR_STRIDE_B;
    b->tiled = VG_LITE_LINEAR;
    /* premultiplied == 0 with premultiplied DATA is deliberate and is exactly
     * what LVGL's own VG_LITE backend does on this part -- the long note above
     * rkg_premultiply() in rk_geometry.cpp has the evidence. */
    b->format = VG_LITE_BGRA8888;   /* premultiplied ARGB8888 words */
    b->memory = px;
    b->address = (uint32_t)(uintptr_t)px;
    /* ★ Re-mapping the same pages once per cell is idempotent ONLY because
     * vg_lite_init_mem() was given GPU base 0: with no MMU translation the
     * "mapping" records the buffer with the kernel and hands back the physical
     * address unchanged, so repeated maps neither leak a translation nor move
     * the buffer. Under a non-zero base this loop would need a matching
     * vg_lite_unmap per cell. */
    GPU_TRY(vg_lite_map(b, VG_LITE_MAP_USER_MEMORY, 0));
}

/* Only reached for gpu cells when s_gpu -- the BS_*_START guards
 * short-circuit the absent case before any vg_lite_* call.
 *
 * ★ ORDERING INVARIANT, and it is what makes the canvas work below benign:
 * NO REFRESH MAY RUN BETWEEN cell_build_assets() AND cell_build_scene().
 * Both are called back-to-back from bench_step(), which runs after
 * lvgl_rt1176_loop() has returned, so LVGL's refresh timer cannot fire in
 * between. It matters because lv_canvas_finish_layer() invalidates the canvas
 * -- i.e. an area of the OLD, about-to-be-deleted screen -- 1 or 64 times; a
 * refresh landing there would render the outgoing scene. cell_build_scene()
 * then calls lv_screen_load(), whose scr_load_internal invalidates the entire
 * new screen, superseding every one of those areas. Insert anything that
 * pumps LVGL between the two calls and this stops being true.
 *
 * Returns false if the cell cannot be built (path-arena overflow); the caller
 * must report and skip rather than render a truncated path set. */
static bool cell_build_assets(const rkb_cell_t *c)
{
    const uint32_t t0 = micros();
    g_rotor_bytes = 0;
    switch (c->strat) {
    case RKB_VECTOR:
        if (c->gpu) {
            size_t bytes = 0;
            const int np = rkg_build_vg_paths(c->var, g_vg_paths, g_vg_colors,
                                              &bytes);
            /* g_init_us too: leaving the previous cell's value would attach a
             * plausible init time to a cell that never built. */
            if (np < 0) { g_vg_npaths = 0; g_init_us = 0; return false; }
            g_vg_npaths = np;
            g_rotor_bytes = (uint32_t)bytes;
        }
        break;
    case RKB_BITMAP:
        rkg_render_rotor_argb(c->var, g_rotor, RKB_KNOB_PX, 0.0f);
        if (c->gpu) {
            rkg_premultiply(g_rotor, ROTOR_WORDS);
            vg_wrap_argb(&g_rotor_vgbuf, g_rotor);
        } else {
            memset(&g_rotor_dsc, 0, sizeof(g_rotor_dsc));
            g_rotor_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            g_rotor_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
            g_rotor_dsc.header.w = RKB_KNOB_PX;
            g_rotor_dsc.header.h = RKB_KNOB_PX;
            g_rotor_dsc.header.stride = RKB_ROTOR_STRIDE_B;
            g_rotor_dsc.data_size = ROTOR_B;
            g_rotor_dsc.data = (const uint8_t *)g_rotor;
        }
        g_rotor_bytes = ROTOR_B;
        break;
    case RKB_STRIP:
        /* ONE canvas for all 64 frames: init_us should measure rendering, not
         * lv_obj create/delete churn. */
        rkg_render_strip_argb(c->var, g_strip[0], ROTOR_WORDS, RKB_STRIP_N,
                              RKB_KNOB_PX, RKB_STEP_DEG);
        for (int i = 0; i < RKB_STRIP_N; i++) {
            if (c->gpu) {
                rkg_premultiply(g_strip[i], ROTOR_WORDS);
                vg_wrap_argb(&g_strip_vgbuf[i], g_strip[i]);
            } else {
                memset(&g_strip_dsc[i], 0, sizeof(g_strip_dsc[i]));
                g_strip_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
                g_strip_dsc[i].header.cf = LV_COLOR_FORMAT_ARGB8888;
                g_strip_dsc[i].header.w = RKB_KNOB_PX;
                g_strip_dsc[i].header.h = RKB_KNOB_PX;
                g_strip_dsc[i].header.stride = RKB_ROTOR_STRIDE_B;
                g_strip_dsc[i].data_size = ROTOR_B;
                g_strip_dsc[i].data = (const uint8_t *)g_strip[i];
            }
        }
        g_rotor_bytes = (uint32_t)ROTOR_B * RKB_STRIP_N;
        break;
    }
    g_init_us = micros() - t0;
    return true;
}

static lv_obj_t *cell_build_scene(const rkb_cell_t *c, float angle)
{
    g_cell = c;
    lv_obj_t *scr = lv_obj_create(NULL);
    /* Opaque ground: every pixel defined, so the checksum means something
     * (vglite_lvgl_test). No labels -- no font dependence in the goldens. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            const int k = r * 4 + col;
            lv_obj_t *o = lv_obj_create(scr);
            lv_obj_remove_style_all(o);
            lv_obj_set_size(o, RKB_KNOB_PX, RKB_KNOB_PX);
            lv_obj_set_pos(o, 15 + col * 175, 120 + r * 175);
            lv_obj_add_event_cb(o, knob_draw_cb, LV_EVENT_DRAW_MAIN, &g_knob[k]);
            g_knob[k].obj = o;
            g_knob[k].angle = angle;
            g_knob[k].cx = 15.0f + col * 175.0f + RKB_KNOB_PX * 0.5f;
            g_knob[k].cy = 120.0f + r * 175.0f + RKB_KNOB_PX * 0.5f;
        }
    }
    lv_obj_t *old = lv_screen_active();
    lv_screen_load(scr);
    if (old) lv_obj_delete(old);
    return scr;
}

/* ---- bench state machine (Phase A checksums, then Phase B timings) ------- */
typedef enum { BS_IDLE = 0, BS_A_START, BS_A_WAIT, BS_B_START, BS_B_RUN,
               BS_DONE } rkb_state_t;
static rkb_state_t g_state = BS_IDLE;
static int g_ci = 0;                    /* cell index 0..11 */
static uint32_t g_ok = 0, g_absent = 0, g_failed = 0;
static volatile uint32_t g_refr_count = 0;
static uint32_t g_refr_at_start = 0;

/* ---- Phase B timing ------------------------------------------------------
 * ★ NONE OF THE volatile HERE IS REQUIRED TODAY, and the disarm-before-delete
 * ordering in BS_B_RUN is not either: with LV_USE_OS == LV_OS_NONE every writer
 * (refr_cb, anim_cb) and every reader (bench_step) runs from the same thread
 * context under lv_timer_handler(), and this binding's flush_cb is synchronous,
 * so there is no concurrent writer to guard against. Both are defensive, kept
 * because they cost nothing and become LOAD-BEARING the moment a vsync-ISR
 * flush path is adopted -- the same forward hazard, and the same reasoning,
 * that lvgl_mipi_panel.cpp:54-60 records for its own non-volatile flags. */
#define RKB_TIMED_N 64
/* >5x the slowest plausible cell (64 frames at the recorded 2.83 fps ~= 23 s),
 * so a healthy cell can never trip it. */
#define RKB_CELL_TIMEOUT_MS 120000u
static uint32_t g_us[RKB_TIMED_N];
static volatile uint32_t g_nsamp = 0;
static volatile bool g_timing = false;
static volatile bool g_skip_first = false;
static volatile uint32_t g_t0 = 0;      /* micros() at LV_EVENT_REFR_START */
static uint32_t g_frame = 0;            /* animation step, drives every angle */
static uint32_t g_cell_t0 = 0;          /* millis() when the cell armed */
static lv_timer_t *g_anim = NULL;
static uint32_t g_timed = 0, g_absent_b = 0, g_failed_b = 0;

/* ★ LV_EVENT_REFR_READY FIRES ON EMPTY REFRESHES TOO -- lv_refr.c:415 jumps
 * straight to refr_finish when there is nothing invalid, and :439 sends the
 * event from there. Phase A tolerates that: it arms g_refr_at_start
 * immediately after lv_screen_load(), which invalidates the whole screen, so
 * the first event after arming is always a real full render.
 * ★ PHASE B CANNOT INHERIT THAT ASSUMPTION -- it free-runs, so it WILL meet
 * empty refreshes, and both sampling a frame time from one (a near-zero
 * outlier dragging us_med down for exactly the cell being ranked) and
 * re-running gpu_rotor_pass() on one (rotors blitted onto a frame LVGL never
 * redrew the well for) would be wrong. So damage, not REFR_READY, gates both:
 *   LV_EVENT_RENDER_READY is sent from refr_invalid_areas (lv_refr.c:823),
 *   which returns at its FIRST LINE (:751) when inv_p == 0. It therefore
 *   cannot fire on an empty refresh, which is precisely the discrimination
 *   REFR_READY lacks. g_frame_rendered latches it; REFR_READY consumes it.
 * ★ THE GPU PASS STAYS INSIDE THE TIMED WINDOW. RENDER_READY sits between
 * REFR_START (:387) and REFR_READY (:439), so moving the pass there does not
 * move it out of the measurement -- required, since the bench times the whole
 * frame including vg_lite_finish(). It also composites onto a COMPLETE
 * software frame: draw_buf_flush() runs per area inside the loop above :823,
 * and this binding's flush_cb is synchronous. */
static volatile bool g_frame_rendered = false;

static void refr_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:
        g_t0 = micros();
        break;
    case LV_EVENT_RENDER_READY:
        /* gpu rotor pass INSIDE the frame, before anyone counts it done */
        if (g_cell && g_cell->gpu && s_gpu) gpu_rotor_pass();
        g_frame_rendered = true;
        break;
    case LV_EVENT_REFR_READY:
        if (g_frame_rendered) {
            g_refr_count++;
            if (g_timing) {
                /* ★ DROP THE FIRST GATED FRAME OF EVERY CELL. It is the
                 * lv_screen_load() render -- the whole 720x1280 panel,
                 * 921,600 px -- while samples 1..63 are the 16 knob areas LVGL
                 * joins from anim_cb, 16 x 150x150 = 360,000 px. That is a 2.5x
                 * different workload wearing the same units, and it lands in
                 * the mean of the cell it opens. frames= stays 64 because 64
                 * STEADY-STATE samples are still taken; the skipped frame is
                 * not one of them. */
                if (g_skip_first) g_skip_first = false;
                else if (g_nsamp < RKB_TIMED_N)
                    g_us[g_nsamp++] = micros() - g_t0;
            }
        }
        g_frame_rendered = false;
        break;
    default:
        break;
    }
}

/* One animation step for all 16 knobs, then damage them. The per-knob 22.5 deg
 * offset keeps the grid from being 16 copies of one angle -- with the bitmap
 * and strip cells that would let a single rotated blit or a single filmstrip
 * frame stay warm for the entire grid and flatter those strategies. */
static void anim_cb(lv_timer_t *t)
{
    (void)t;
    g_frame++;
    for (int k = 0; k < 16; k++) {
        g_knob[k].angle = fmodf((float)g_frame * RKB_STEP_DEG
                                + (float)k * 22.5f, 360.0f);
        lv_obj_invalidate(g_knob[k].obj);
    }
}

/* gpu_err is appended for GPU cells ONLY. Deliberate: the QEMU gate greps the
 * sw lines, and those lines stay byte-identical to what they were before the
 * counter existed. A sw cell makes no vg_lite call, so a gpu_err token on one
 * could only ever be noise. */
static void cell_print_a(const rkb_cell_t *c, uint32_t crc)
{
    Serial1.printf("cell=%s/%s/%s st=ok crc=0x%08lX init_us=%lu rotor_bytes=%lu",
                   STRAT_NAME[c->strat], ENGINE_NAME(c), VAR_NAME[c->var],
                   (unsigned long)crc, (unsigned long)g_init_us,
                   (unsigned long)g_rotor_bytes);
    if (c->gpu) Serial1.printf(" gpu_err=%lu", (unsigned long)g_gpu_err);
    Serial1.println();
}

/* Same gpu_err rule as cell_print_a, and for the same reason: a cell that
 * blitted nothing posts a superb time, so the gpu lines must carry the number
 * that says whether they drew. Integer-only -- no %f anywhere in this build's
 * printf, so millifps is the unit rather than a formatting choice.
 * ★ us_min/us_max EXIST BECAUSE us_mean IS OUTLIER-DOMINATED, measured rather
 * than feared: two cells whose medians agreed to within 0.3% reported means
 * 45% apart, because ONE ~3 s frame landed in one of them. The median is what
 * ranks the cells; us_max is what says whether the mean beside it is describing
 * the workload or describing a single stall. Reporting the mean without the
 * spread invites reading §13's table off the wrong column. */
static void cell_print_b(const rkb_cell_t *c, uint32_t us_med, uint32_t us_min,
                         uint32_t us_max, uint32_t us_mean)
{
    Serial1.printf("time=%s/%s/%s frames=%d mfps_med=%lu us_med=%lu"
                   " us_min=%lu us_max=%lu us_mean=%lu",
                   STRAT_NAME[c->strat], ENGINE_NAME(c), VAR_NAME[c->var],
                   RKB_TIMED_N,
                   (unsigned long)(1000000000ull / (us_med ? us_med : 1)),
                   (unsigned long)us_med, (unsigned long)us_min,
                   (unsigned long)us_max, (unsigned long)us_mean);
    if (c->gpu) Serial1.printf(" gpu_err=%lu", (unsigned long)g_gpu_err);
    Serial1.println();
}

static void bench_step(void)
{
    switch (g_state) {
    case BS_IDLE:
        break;
    case BS_A_START: {
        if (g_ci >= 12) {
            /* ok + gpu_absent + failed == cells is the vacuity check: it is
             * what says every cell was actually accounted for rather than
             * quietly skipped. failed= is appended last so the three original
             * tokens stay verbatim and in order. */
            Serial1.printf("crc_done cells=12 ok=%lu gpu_absent=%lu failed=%lu\n",
                           (unsigned long)g_ok, (unsigned long)g_absent,
                           (unsigned long)g_failed);
            g_ci = 0;                    /* Phase B re-walks the same 12 cells */
            g_state = BS_B_START;
            break;
        }
        const rkb_cell_t *c = &CELLS[g_ci];
        if (c->gpu && !s_gpu) {
            Serial1.printf("cell=%s/gpu/%s st=gpu-absent\n",
                           STRAT_NAME[c->strat], VAR_NAME[c->var]);
            g_absent++; g_ci++;
            break;                        /* stay in BS_A_START, next cell */
        }
        g_gpu_err = 0;                    /* per-cell, see the latch above */
        if (!cell_build_assets(c)) {
            /* Path arena overflowed: greppable, counted apart from ok, and
             * NOT rendered -- a truncated path set would still draw and still
             * produce a plausible time. */
            Serial1.printf("cell=%s/%s/%s st=vg-overflow\n",
                           STRAT_NAME[c->strat], ENGINE_NAME(c),
                           VAR_NAME[c->var]);
            g_failed++; g_ci++;
            break;
        }
        cell_build_scene(c, RKB_CANON_DEG);
        g_refr_at_start = g_refr_count;
        g_state = BS_A_WAIT;
        break;
    }
    case BS_A_WAIT:
        /* One full refresh has retired, gpu pass included (refr_cb runs it
         * before bumping the count).
         * ★ REFR_READY IS SUFFICIENT ON ITS OWN. It is sent from refr_finish
         * AFTER refr_invalid_areas has rendered and flushed (lv_refr.c:439),
         * and this binding's flush_cb is synchronous -- direct mode has nothing
         * to transfer. The lvgl_mipi_panel_frame_done() conjunct that used to
         * be here was vacuous: that flag is LATCHING and one-shot (set by the
         * first full refresh ever, cleared only by another create()), so from
         * cell 1 onward it was a constant true. */
        if (g_refr_count > g_refr_at_start) {
            lvgl_sum_reset();
            lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);
            cell_print_a(&CELLS[g_ci], lvgl_sum_value());
            g_ok++; g_ci++;
            g_cell = NULL;
            g_state = BS_A_START;
        }
        break;

    case BS_B_START: {
        if (g_ci >= 12) {
            /* Same vacuity arithmetic as crc_done: timed + gpu_absent + failed
             * == cells is what says no cell was quietly dropped. */
            Serial1.printf("bench_done cells=12 timed=%lu gpu_absent=%lu failed=%lu\n",
                           (unsigned long)g_timed, (unsigned long)g_absent_b,
                           (unsigned long)g_failed_b);
            g_state = BS_DONE;
            break;
        }
        const rkb_cell_t *c = &CELLS[g_ci];
        if (c->gpu && !s_gpu) {
            Serial1.printf("time=%s/gpu/%s st=gpu-absent\n",
                           STRAT_NAME[c->strat], VAR_NAME[c->var]);
            g_absent_b++; g_ci++;
            break;                        /* stay in BS_B_START, next cell */
        }
        g_gpu_err = 0;                    /* per-cell, as in Phase A */
        if (!cell_build_assets(c)) {
            Serial1.printf("time=%s/%s/%s st=vg-overflow\n",
                           STRAT_NAME[c->strat], ENGINE_NAME(c),
                           VAR_NAME[c->var]);
            g_failed_b++; g_ci++;
            break;
        }
        /* Angle 0 here, not RKB_CANON_DEG: the canonical angle exists to make
         * Phase A's checksums comparable, and anim_cb overwrites it on its
         * first tick anyway. The build_assets/build_scene ordering invariant
         * documented above cell_build_assets() holds here for the same reason
         * it does in Phase A -- nothing between the two calls pumps LVGL. */
        cell_build_scene(c, 0.0f);
        g_frame = 0;
        g_nsamp = 0;
        g_skip_first = true;              /* the screen-load render, see refr_cb */
        g_cell_t0 = millis();
        g_timing = true;
        /* ★ 15 ms against LVGL's 33 ms refresh period, and created LAST on
         * purpose: lv_timer_create inserts at the list HEAD (lv_timer.c:169),
         * so anim_cb runs BEFORE the display refresh timer in every
         * lv_timer_handler pass. Every refresh therefore has fresh damage, and
         * the RENDER_READY gate above turns a violation of that into a stall
         * (no samples) rather than into a fast, meaningless number. */
        g_anim = lv_timer_create(anim_cb, 15, NULL);
        g_state = BS_B_RUN;
        break;
    }

    case BS_B_RUN: {
        const rkb_cell_t *c = &CELLS[g_ci];
        if (g_nsamp >= RKB_TIMED_N) {
            g_timing = false;             /* before the delete: no late sample */
            if (g_anim) { lv_timer_delete(g_anim); g_anim = NULL; }
            /* Sort a COPY -- g_us stays in arrival order, which is what makes
             * a first-frame outlier visible to anyone who ever dumps it. */
            uint32_t s[RKB_TIMED_N];
            memcpy(s, g_us, sizeof(s));
            for (int i = 1; i < RKB_TIMED_N; i++) {
                const uint32_t v = s[i];
                int j = i - 1;
                while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
                s[j + 1] = v;
            }
            /* 64-bit accumulator: at QEMU's seconds-per-frame the uint32 sum
             * of 64 samples is within a small factor of overflowing, and a
             * wrapped mean beside a sane median reads as a real measurement. */
            uint64_t sum = 0;
            for (int i = 0; i < RKB_TIMED_N; i++) sum += s[i];
            cell_print_b(c, s[RKB_TIMED_N / 2], s[0], s[RKB_TIMED_N - 1],
                         (uint32_t)(sum / RKB_TIMED_N));
            g_timed++; g_ci++;
            g_cell = NULL;
            g_state = BS_B_START;
            break;
        }
        /* ★ BOUNDED WAIT, and the RENDER_READY gate is exactly why it is
         * needed. That gate turns "nothing was damaged" into "no samples
         * arrive" -- correct, and SILENT: a wedged cell would sit here forever
         * printing nothing, and take the remaining cells with it, so a run that
         * lost eleven cells would look identical to one still working on cell
         * one. This names the stall and salvages the rest of the sweep.
         * gpu_err rides along here (unlike st=vg-overflow, which reports a
         * BUILD failure before any frame): a cell that starved mid-render is
         * precisely where "was the GPU erroring?" is the first question. */
        if (millis() - g_cell_t0 > RKB_CELL_TIMEOUT_MS) {
            g_timing = false;
            if (g_anim) { lv_timer_delete(g_anim); g_anim = NULL; }
            Serial1.printf("time=%s/%s/%s st=timeout nsamp=%lu",
                           STRAT_NAME[c->strat], ENGINE_NAME(c),
                           VAR_NAME[c->var], (unsigned long)g_nsamp);
            if (c->gpu) Serial1.printf(" gpu_err=%lu", (unsigned long)g_gpu_err);
            Serial1.println();
            g_failed_b++; g_ci++;
            g_cell = NULL;
            g_state = BS_B_START;
        }
        break;
    }

    case BS_DONE:
        break;
    }
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("rotary_knob_bench up");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) { Serial1.println("rkb_fatal=panel"); return; }
    Display.fillScreen(0x0000);

    /* ★ ASK BEFORE COMMITTING (vglite_probe): vg_lite_init() SPINS on absent
     * hardware, so the chip-ID read is what makes the absent case a clean
     * negative instead of a hang. */
    vg_lite_init_mem(VGLITE_RT1176_REGISTER_BASE, 0u, vglite_pool, VGLITE_POOL_BYTES);
    const uint32_t chip_id = vg_lite_hal_probe_chip_id();
    /* ★ ONE BOOL CANNOT SAY WHY. s_gpu collapses "no GC355 here" (the QEMU
     * case) and "the GPU is present but init or map FAILED" (a silicon case,
     * and the one that would otherwise present as a board that mysteriously
     * reports every gpu cell absent). These two codes separate them. They are
     * printed only on the attempt path -- when chip_id is 0 nothing was
     * attempted and there is no code to report. */
    vg_lite_error_t vg_init_err = VG_LITE_SUCCESS;
    vg_lite_error_t vg_map_err  = VG_LITE_SUCCESS;
    if (chip_id != 0u && (vg_init_err = vg_lite_init(TESS_W, TESS_H)) == VG_LITE_SUCCESS) {
        memset(&s_target, 0, sizeof(s_target));
        s_target.width   = Display.width();
        s_target.height  = Display.height();
        s_target.stride  = Display.width() * PANEL_BYTES_PER_PIXEL;
        s_target.tiled   = VG_LITE_LINEAR;
        s_target.format  = VG_LITE_BGRA8888;   /* = panel XRGB8888 memory order */
        s_target.memory  = (void *)Display.framebuffer();
        s_target.address = (uint32_t)(uintptr_t)Display.framebuffer();
        /* ★ REGISTER the framebuffer with the driver or every draw "succeeds"
         * and changes nothing (vglite_probe, measured on silicon). */
        vg_map_err = vg_lite_map(&s_target, VG_LITE_MAP_USER_MEMORY, 0);
        s_gpu = (vg_map_err == VG_LITE_SUCCESS);
    }
    /* The `gpu=present`/`gpu=absent` PREFIX is load-bearing -- run_qemu.sh
     * prefix-greps it -- so the codes are appended, never interpolated. The
     * token is deliberately NOT spelled `gpu_err=`: that name belongs to the
     * per-cell counter and the gate has a tripwire on it. */
    if (chip_id != 0u)
        Serial1.printf("gpu=%s chip_id=0x%08lX vg_init=%d vg_map=%d\n",
                       s_gpu ? "present" : "absent", (unsigned long)chip_id,
                       (int)vg_init_err, (int)vg_map_err);
    else
        Serial1.printf("gpu=%s chip_id=0x%08lX\n", s_gpu ? "present" : "absent",
                       (unsigned long)chip_id);

    lvgl_rt1176_begin();
    lvgl_mipi_panel_create(Display);
    /* All three legs of the frame go to one callback: REFR_START opens the
     * timed window, RENDER_READY proves the frame had damage (and runs the gpu
     * pass), REFR_READY closes it. Registering them separately is LVGL's
     * per-code API, not three behaviours. */
    lv_display_t *disp = lv_display_get_default();
    lv_display_add_event_cb(disp, refr_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, refr_cb, LV_EVENT_RENDER_READY, NULL);
    lv_display_add_event_cb(disp, refr_cb, LV_EVENT_REFR_READY, NULL);
    g_state = BS_A_START;
}

void loop()
{
    lvgl_rt1176_loop();
    bench_step();

    /* Liveness after bench_done, FOR THE SILICON TRANSCRIPT -- no gate greps
     * hb. Without it "the bench finished and the image is still running" and
     * "the image died the instant it printed its last line" are the same
     * capture, and on the bench that distinction is the difference between a
     * completed run and one to repeat. The QEMU gate stops at bench_done. */
    static uint32_t hb_last = 0, hb_n = 0;
    if (g_state == BS_DONE && millis() - hb_last >= 2000) {
        hb_last = millis();
        Serial1.printf("hb n=%lu\n", (unsigned long)hb_n++);
    }
}
