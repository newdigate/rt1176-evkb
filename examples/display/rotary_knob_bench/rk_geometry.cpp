/* rk_geometry.cpp - see rk_geometry.h.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#include "rk_geometry.h"
#include <math.h>
#include <string.h>

#define RKG_DEG (3.14159265358979f / 180.0f)

/* ---- palette: RotaryKnob.dc.html THEME.light, state idle ---- */
#define RKG_WELL        0xdcdce6
#define RKG_WELL_STROKE 0xb6b8cc
#define RKG_BODY        0x282b60
#define RKG_INNER       0x333871
#define RKG_INDEX       0xfcfbf6
static const uint32_t RKG_TONES[8] = { 0x5b61a8, 0x4a4f92, 0x3a3f7d, 0x333871,
                                       0x3a3f7d, 0x4a4f92, 0x5b61a8, 0x6a70b8 };

/* P(r, th): 0 deg = 12 o'clock, clockwise -- the DC convention (and
 * synthui_knob's polar()). */
static void rkg_polar(float cx, float cy, float S, float r, float deg,
                      float *x, float *y)
{
    *x = cx + r * S * sinf(deg * RKG_DEG);
    *y = cy - r * S * cosf(deg * RKG_DEG);
}

static void draw_disc(lv_layer_t *l, float x, float y, float rpx, uint32_t hex)
{
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_hex(hex); d.bg_opa = LV_OPA_COVER;
    lv_area_t a = { (int32_t)lroundf(x - rpx), (int32_t)lroundf(y - rpx),
                    (int32_t)lroundf(x + rpx), (int32_t)lroundf(y + rpx) };
    lv_draw_rect(l, &d, &a);
}

/* ring(r0, r1, a1, a2) as an LVGL arc: radius names the OUTER edge, width
 * extends inward. The fold below is synthui_knob's draw_arc_seg lesson --
 * LVGL's sw arc clamps a negative start to 0 and renders a truncated wedge,
 * so fold the start into [0,360) and carry the span. */
static void draw_ring_sector(lv_layer_t *l, float cx, float cy, float S,
                             float r0, float r1, float a1, float a2,
                             uint32_t hex)
{
    lv_draw_arc_dsc_t a; lv_draw_arc_dsc_init(&a);
    a.center.x = (int32_t)lroundf(cx); a.center.y = (int32_t)lroundf(cy);
    a.radius = (uint16_t)lroundf(r1 * S);
    a.width  = (int32_t)lroundf((r1 - r0) * S); if (a.width < 1) a.width = 1;
    float span = a2 - a1;
    if (span <= 0.0f) return;
    if (span > 360.0f) span = 360.0f;
    float s0 = fmodf(a1 - 90.0f, 360.0f);   /* LVGL measures from 3 o'clock */
    if (s0 < 0.0f) s0 += 360.0f;
    /* lv_value_precise_t is int32 here (LV_USE_FLOAT 0), so these casts
     * TRUNCATE rather than round -- deliberately left alone: it is
     * deterministic, identical on QEMU and silicon, and the goldens are
     * recorded against it. Flipping LV_USE_FLOAT to 1 would make these
     * fractional and MOVE every checksum in this example. */
    a.start_angle = (lv_value_precise_t)s0;
    a.end_angle   = (lv_value_precise_t)(s0 + span);
    a.color = lv_color_hex(hex); a.opa = LV_OPA_COVER;
    lv_draw_arc(l, &a);
}

void rkg_draw_well_sw(lv_layer_t *l, float cx, float cy, float S)
{
    /* SVG: circle r39 fill + centred stroke w1.6. LVGL's border sits inside
     * the radius rather than straddling it -- a half-stroke-width difference
     * the per-cell goldens absorb (every cell draws the well identically). */
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_hex(RKG_WELL); d.bg_opa = LV_OPA_COVER;
    d.border_color = lv_color_hex(RKG_WELL_STROKE); d.border_opa = LV_OPA_COVER;
    d.border_width = (int32_t)lroundf(1.6f * S);
    if (d.border_width < 1) d.border_width = 1;
    const float r = 39.0f * S;
    lv_area_t a = { (int32_t)lroundf(cx - r), (int32_t)lroundf(cy - r),
                    (int32_t)lroundf(cx + r), (int32_t)lroundf(cy + r) };
    lv_draw_rect(l, &d, &a);
}

void rkg_draw_rotor_sw(lv_layer_t *l, rkg_variant_t v,
                       float cx, float cy, float S, float th)
{
    if (v == RKG_NOTCH) {
        draw_disc(l, cx, cy, 36.0f * S, RKG_BODY);
        draw_disc(l, cx, cy, 27.0f * S, RKG_INNER);
        draw_ring_sector(l, cx, cy, S, 16.0f, 36.0f, th - 8.0f, th + 8.0f,
                         RKG_INDEX);
    } else { /* RKG_FACET: 8 triangles + index sector, all rotated by th */
        for (int i = 0; i < 8; i++) {
            const float a1 = (float)i * 45.0f + 22.5f + th;
            lv_draw_triangle_dsc_t t; lv_draw_triangle_dsc_init(&t);
            t.color = lv_color_hex(RKG_TONES[i]); t.opa = LV_OPA_COVER;
            float x, y;
            t.p[0].x = (lv_value_precise_t)lroundf(cx);
            t.p[0].y = (lv_value_precise_t)lroundf(cy);
            rkg_polar(cx, cy, S, 36.0f, a1, &x, &y);
            t.p[1].x = (lv_value_precise_t)lroundf(x);
            t.p[1].y = (lv_value_precise_t)lroundf(y);
            rkg_polar(cx, cy, S, 36.0f, a1 + 45.0f, &x, &y);
            t.p[2].x = (lv_value_precise_t)lroundf(x);
            t.p[2].y = (lv_value_precise_t)lroundf(y);
            lv_draw_triangle(l, &t);
        }
        draw_ring_sector(l, cx, cy, S, 20.0f, 36.0f, th - 22.5f, th + 22.5f,
                         RKG_INDEX);
    }
}

/* One frame into an already-created canvas.
 *
 * ★ finish_layer DISPATCHES THE TASKS ITSELF, here, outside any refresh --
 * safe only because LV_DRAW_SW_DRAW_UNIT_CNT == 1 (lv_conf.h:252). With more
 * than one draw unit the tasks would be handed to units that only run from the
 * refresh loop, and this would return with nothing drawn. */
static void render_one(lv_obj_t *cv, rkg_variant_t v, uint32_t *buf, int side,
                       float th)
{
    /* The whole allocated extent, padding columns included: the padding is
     * never sampled but it IS hashed by nothing and blitted by the GPU as part
     * of a 64-B burst, so leaving it uninitialised would make the picture
     * depend on stale SDRAM. */
    memset(buf, 0, (size_t)side * (size_t)RKB_ROTOR_STRIDE_B);
    lv_canvas_set_buffer(cv, buf, side, side, LV_COLOR_FORMAT_ARGB8888);
    /* The build's LV_DRAW_BUF_STRIDE_ALIGN must be what the arenas were sized
     * for, or the canvas writes rows at a pitch the buffers do not have. */
    LV_ASSERT(lv_draw_buf_width_to_stride(side, LV_COLOR_FORMAT_ARGB8888)
              == (uint32_t)RKB_ROTOR_STRIDE_B);
    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);
    rkg_draw_rotor_sw(&layer, v, side * 0.5f, side * 0.5f,
                      (float)side / 100.0f, th);
    lv_canvas_finish_layer(cv, &layer);   /* dispatches synchronously */
}

void rkg_render_rotor_argb(rkg_variant_t v, uint32_t *buf, int side, float th)
{
    lv_obj_t *cv = lv_canvas_create(lv_screen_active());
    render_one(cv, v, buf, side, th);
    lv_obj_delete(cv);
}

void rkg_render_strip_argb(rkg_variant_t v, uint32_t *base, size_t stride_words,
                           int n, int side, float deg_step)
{
    lv_obj_t *cv = lv_canvas_create(lv_screen_active());
    for (int i = 0; i < n; i++)
        render_one(cv, v, base + (size_t)i * stride_words, side,
                   (float)i * deg_step);
    lv_obj_delete(cv);
}

/* ★ THIS IS NOT A DOUBLE PREMULTIPLY, and the question is worth settling in
 * writing because it looks like one.
 *
 * The gpu cells blit with VG_LITE_BLEND_SRC_OVER, whose documented arithmetic
 * is "RGB: S + D*(1 - Sa)" (inc/vg_lite.h:461) -- the PREMULTIPLIED Porter-Duff
 * over. Straight alpha would need S*Sa + D*(1 - Sa), which is a different
 * enumerator (VG_LITE_BLEND_NORMAL_LVGL, :481) and is unavailable here:
 * gcFEATURE_VG_LVGL_SUPPORT is 0 on this part (gc355/0x0_1216).
 *
 * That is exactly what LVGL 9.4's own VG_LITE backend does against this same
 * driver: lv_draw_vg_lite_img.c:66 premultiplies in software precisely when
 * !lv_vg_lite_support_blend_normal(), and lv_vg_lite_utils.c:958 then selects
 * SRC_OVER. LVGL never sets vg_lite_buffer_t.premultiplied at all (no
 * assignment anywhere in lvgl/src/draw/vg_lite), so the shipped, tested
 * combination on this silicon is: software-premultiplied pixels,
 * premultiplied == 0, SRC_OVER. vg_wrap_argb reproduces it deliberately.
 *
 * The driver's premultiply selection (vg_lite.c:4742-4760) does NOT contradict
 * this: with SRC_OVER (=1) premul_flag is 0 -- SRC_OVER is outside both the
 * OPENVG_BLEND_* (0x2000..0x2009) and the *_LVGL (11..14) ranges the flag tests
 * -- so the FIRST branch applies, and that branch deliberately treats
 * source->premultiplied 0 and 1 IDENTICALLY. It therefore cannot be read as
 * "the hardware premultiplies because you declared 0". Consistent with
 * gcFEATURE_BIT_VG_HW_PREMULTIPLY's own header gloss: "HW multiplier can accept
 * either premultiplied or not" (inc/vg_lite.h:209). */
void rkg_premultiply(uint32_t *buf, size_t npx)
{
    for (size_t i = 0; i < npx; i++) {
        const uint32_t p = buf[i], a = p >> 24;
        const uint32_t r = (((p >> 16) & 0xFFu) * a) / 255u;
        const uint32_t g = (((p >> 8)  & 0xFFu) * a) / 255u;
        const uint32_t b = ((p & 0xFFu) * a) / 255u;
        buf[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

/* ---- vg_lite path build -------------------------------------------------- */
/* ★ vg_lite_color_t IS ABGR (vglite_probe's measured lesson): red in the LOW
 * byte. Convert once here so nobody upstream can get it backwards. */
static uint32_t abgr(uint32_t hex)
{
    return 0xFF000000u | ((hex & 0xFFu) << 16) | (hex & 0xFF00u)
           | ((hex >> 16) & 0xFFu);
}

/* S32 path data in centred viewBox units x16: integer coords keep the format
 * vglite_probe proved on this driver, x16 keeps ~0.1 px precision at S=1.5.
 * The drawing matrix carries scale(S/16). */
#define RKG_FIX 16.0f
#define RKG_VG_ARENA_WORDS 4096
static int32_t s_vg_arena[RKG_VG_ARENA_WORDS];
static size_t  s_vg_used;
/* Sticky: a truncated path is a WRONG picture that still draws and still
 * benchmarks, so the overflow has to leave a trace the caller must look at
 * rather than being absorbed by the bounds check. */
static bool    s_vg_overflow;

static void emit(int32_t w)
{
    if (s_vg_used < RKG_VG_ARENA_WORDS) s_vg_arena[s_vg_used++] = w;
    else s_vg_overflow = true;
}
static int32_t fx(float f) { return (int32_t)lroundf(f * RKG_FIX); }
/* centred polar, viewBox units (no scale -- the matrix scales) */
static void cpol(float r, float deg, float *x, float *y)
{
    *x = r * sinf(deg * RKG_DEG);
    *y = -r * cosf(deg * RKG_DEG);
}

/* Emit cubics approximating the arc r, a1 -> a2 (current point must already
 * be at (r, a1)). Standard k = (4/3)tan(delta/4); a negative span flips the
 * tangent sign via tan, so the inner (reversed) arc of a ring needs no
 * special case. */
static void emit_arc(float r, float a1, float a2)
{
    const float span = a2 - a1;
    int nseg = (int)ceilf(fabsf(span) / 90.0f);
    if (nseg < 1) nseg = 1;
    const float step = span / (float)nseg;
    const float d = (4.0f / 3.0f) * tanf(step * RKG_DEG / 4.0f) * r;
    for (int i = 0; i < nseg; i++) {
        const float b1 = a1 + (float)i * step, b2 = b1 + step;
        float x1, y1, x2, y2;
        cpol(r, b1, &x1, &y1);
        cpol(r, b2, &x2, &y2);
        /* unit tangent of p(t)=(r sin t, -r cos t) is (cos t, sin t) */
        emit(VLC_OP_CUBIC);
        emit(fx(x1 + d * cosf(b1 * RKG_DEG))); emit(fx(y1 + d * sinf(b1 * RKG_DEG)));
        emit(fx(x2 - d * cosf(b2 * RKG_DEG))); emit(fx(y2 - d * sinf(b2 * RKG_DEG)));
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
    emit_arc(r0, a2, a1);               /* reversed inner edge */
    emit(VLC_OP_CLOSE);
}

static void emit_tri(float r, float a1, float a2)
{
    float x, y;
    emit(VLC_OP_MOVE); emit(0); emit(0);
    cpol(r, a1, &x, &y);
    emit(VLC_OP_LINE); emit(fx(x)); emit(fx(y));
    cpol(r, a2, &x, &y);
    emit(VLC_OP_LINE); emit(fx(x)); emit(fx(y));
    emit(VLC_OP_CLOSE);
}

/* Close one path object over the arena words emitted since 'start'. */
static void finish_path(vg_lite_path_t *p, size_t start)
{
    emit(VLC_OP_END);
    memset(p, 0, sizeof(*p));
    vg_lite_init_path(p, VG_LITE_S32, VG_LITE_HIGH,
                      (uint32_t)((s_vg_used - start) * sizeof(int32_t)),
                      &s_vg_arena[start],
                      -41.0f * RKG_FIX, -41.0f * RKG_FIX,
                      41.0f * RKG_FIX, 41.0f * RKG_FIX);
}

int rkg_build_vg_paths(rkg_variant_t v, vg_lite_path_t *paths,
                       uint32_t *colors_abgr, size_t *out_bytes)
{
    s_vg_used = 0;
    s_vg_overflow = false;
    int n = 0;
    size_t start;
    if (v == RKG_NOTCH) {
        start = s_vg_used; emit_circle(36.0f);
        finish_path(&paths[n], start); colors_abgr[n++] = abgr(RKG_BODY);
        start = s_vg_used; emit_circle(27.0f);
        finish_path(&paths[n], start); colors_abgr[n++] = abgr(RKG_INNER);
        start = s_vg_used; emit_ring(16.0f, 36.0f, -8.0f, 8.0f);
        finish_path(&paths[n], start); colors_abgr[n++] = abgr(RKG_INDEX);
    } else {
        for (int i = 0; i < 8; i++) {
            const float a1 = (float)i * 45.0f + 22.5f;
            start = s_vg_used; emit_tri(36.0f, a1, a1 + 45.0f);
            finish_path(&paths[n], start); colors_abgr[n++] = abgr(RKG_TONES[i]);
        }
        start = s_vg_used; emit_ring(20.0f, 36.0f, -22.5f, 22.5f);
        finish_path(&paths[n], start); colors_abgr[n++] = abgr(RKG_INDEX);
    }
    *out_bytes = s_vg_used * sizeof(int32_t);
    return s_vg_overflow ? -1 : n;
}
