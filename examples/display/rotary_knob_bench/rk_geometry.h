/* rk_geometry.h - RotaryKnob design geometry (RotaryKnob.dc.html, light/idle).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Single source of truth for the design: the LVGL-sw expression renders the
 * screen for sw cells AND paints the rotor bitmaps/filmstrips the bitmap and
 * strip cells (both engines) consume; the vg_lite expression is the cached
 * path set the vector/gpu cell draws. Convention throughout is the DC file's:
 * 0 deg = 12 o'clock, clockwise positive, viewBox 0..100.
 */
#ifndef RK_GEOMETRY_H
#define RK_GEOMETRY_H

#include <lvgl.h>
#include <stddef.h>
#include <stdint.h>
/* Outside the extern "C" below on purpose: vg_lite.h carries its own
 * extern-"C" guard, so nesting it added nothing and hid that fact. */
#include "vg_lite.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RKB_KNOB_PX   150
#define RKB_KNOB_S    (RKB_KNOB_PX / 100.0f)
#define RKB_STRIP_N   64
#define RKB_STEP_DEG  (360.0f / RKB_STRIP_N)
#define RKB_CANON_DEG 45.0f

/* ★ ROW PITCH OF EVERY ROTOR BUFFER, IN PIXELS -- 150 rounded up to a 16-px
 * boundary. The GC355 rejects a BGRA8888 blit source whose stride is not
 * 16 px x 4 B = 64 B aligned (srcbuf_align_check, VGLite/vg_lite.c:1854-1861,
 * with gcFEATURE_VG_16PIXELS_ALIGNED=1 and ERROR_CHECK=1 on this part), and a
 * rejected blit draws NOTHING while the cell still times fast. The build sets
 * LV_DRAW_BUF_STRIDE_ALIGN=64 so LVGL's canvas derives the identical 640-byte
 * stride; two static_asserts in rk_geometry.cpp tie the two together at COMPILE
 * time, so a mismatch is a build failure rather than a run-time discovery.
 * Buffers are therefore RKB_KNOB_PX rows of RKB_ROTOR_STRIDE_PX pixels: the
 * last 10 px of every row are padding the GPU never samples. */
#define RKB_ROTOR_STRIDE_PX 160
#define RKB_ROTOR_STRIDE_B  (RKB_ROTOR_STRIDE_PX * 4)
#define RKB_ROTOR_BYTES     (RKB_KNOB_PX * RKB_ROTOR_STRIDE_B)

typedef enum { RKG_NOTCH = 0, RKG_FACET } rkg_variant_t;

/* LVGL-sw expression, screen coords. cx/cy = knob centre, S = px per viewBox
 * unit, th = rotor angle in degrees. */
void rkg_draw_well_sw(lv_layer_t *layer, float cx, float cy, float S);
void rkg_draw_rotor_sw(lv_layer_t *layer, rkg_variant_t v,
                       float cx, float cy, float S, float th);

/* Render the rotor alone (transparent background) into an ARGB8888 buffer of
 * side rows x RKB_ROTOR_STRIDE_PX pixels, via an LVGL canvas. */
void rkg_render_rotor_argb(rkg_variant_t v, uint32_t *buf, int side, float th);

/* Filmstrip form of the same: n frames at i*deg_step, frame i based at
 * base + i*stride_words. ONE canvas is created for the whole run and
 * re-pointed per frame -- object churn is not what init_us is meant to
 * measure, and at n=64 the create/delete pair dominated it. */
void rkg_render_strip_argb(rkg_variant_t v, uint32_t *base, size_t stride_words,
                           int n, int side, float deg_step);

/* Straight-alpha ARGB8888 -> premultiplied, in place (gpu blit sources).
 * REQUIRED by VG_LITE_BLEND_SRC_OVER, whose arithmetic is S + D*(1 - Sa) --
 * see the long note in the .cpp for why this is not the double-premultiply it
 * can look like. */
void rkg_premultiply(uint32_t *buf, size_t npx);

/* vg_lite expression: build the rotor's cached paths in CENTRED viewBox
 * units x16 (S32 coords, 1/16-unit precision; pair with a matrix scale of
 * S/16). Fills paths[] and colors_abgr[]; out_bytes gets the total path-data
 * byte count.
 * RETURNS the path count, or -1 if the path arena overflowed -- the caller
 * MUST treat negative as fatal for the cell rather than drawing a truncated
 * path set (a silently short path is a wrong picture that still benchmarks).
 * Cannot fire today (~110 of 4096 words); it is a guard for the additional
 * variants Phase 2 adds. */
#define RKG_VG_MAX_PATHS 9
int rkg_build_vg_paths(rkg_variant_t v, vg_lite_path_t *paths,
                       uint32_t *colors_abgr, size_t *out_bytes);

#ifdef __cplusplus
}
#endif
#endif
