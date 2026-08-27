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

#ifdef __cplusplus
extern "C" {
#endif

#include "vg_lite.h"

#define RKB_KNOB_PX   150
#define RKB_KNOB_S    (RKB_KNOB_PX / 100.0f)
#define RKB_STRIP_N   64
#define RKB_STEP_DEG  (360.0f / RKB_STRIP_N)
#define RKB_CANON_DEG 45.0f

typedef enum { RKG_NOTCH = 0, RKG_FACET } rkg_variant_t;

/* LVGL-sw expression, screen coords. cx/cy = knob centre, S = px per viewBox
 * unit, th = rotor angle in degrees. */
void rkg_draw_well_sw(lv_layer_t *layer, float cx, float cy, float S);
void rkg_draw_rotor_sw(lv_layer_t *layer, rkg_variant_t v,
                       float cx, float cy, float S, float th);

/* Render the rotor alone (transparent background) into a side*side
 * ARGB8888 buffer via an LVGL canvas. */
void rkg_render_rotor_argb(rkg_variant_t v, uint32_t *buf, int side, float th);

/* Straight-alpha ARGB8888 -> premultiplied, in place (gpu blit sources). */
void rkg_premultiply(uint32_t *buf, size_t npx);

/* vg_lite expression: build the rotor's cached paths in CENTRED viewBox
 * units x16 (S32 coords, 1/16-unit precision; pair with a matrix scale of
 * S/16). Returns the path count; fills paths[] and colors_abgr[].
 * out_bytes gets the total path-data byte count. */
#define RKG_VG_MAX_PATHS 9
int rkg_build_vg_paths(rkg_variant_t v, vg_lite_path_t *paths,
                       uint32_t *colors_abgr, size_t *out_bytes);

#ifdef __cplusplus
}
#endif
#endif
