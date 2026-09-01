/* tests/stub/vg_lite.h - the SMALLEST vg_lite surface vgc_harness.h,
 * vgc_arena.cpp and vgc_cases_path.cpp need, so the arena and the path case
 * geometry can be compiled and tested on the host.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * This directory goes on the include path AHEAD of the real driver, and only
 * for tests/run.sh -- the target build never sees it (it has no -I pointing
 * here, and the real VGLite include dir is the only vg_lite.h on that path).
 *
 * ★ A STUB IS A CLAIM ABOUT THE REAL HEADER, so the things the tests ASSERT
 * on are kept faithful rather than convenient:
 *   - the VLC_OP_* values are the real ones (inc/vg_lite.h:62-67). arena_test
 *     pins emitted opcodes by value, so a wrong constant here would pin the
 *     wrong thing and agree with itself forever.
 *   - vg_lite_format_t is in the real order and starts at 0
 *     (inc/vg_lite.h:262-268), so VG_LITE_S8/S16/S32/FP32 are 0/1/2/3.
 *     cases_path_geom_test derives each format's ELEMENT WIDTH from that
 *     enumerator, and the four path arrays it renders are laid out by that
 *     width, so the identities have to be the real ones.
 *   - vg_lite_path_t carries the four fields the arena writes through
 *     vg_lite_init_path, with bounding_box FIRST as in the real struct.
 *   - vg_lite_fill_t is the real 0x1900/0x1901 pair (inc/vg_lite.h:514-518).
 *     Nothing here compares a fill rule to a literal -- the model rasteriser
 *     only tests it against the enumerator -- so the wrong values would never
 *     have failed. That is exactly the argument for fixing them: a stub is
 *     only useful while every value in it is the driver's.
 *   - VG_LITE_SUCCESS is 0 and VG_LITE_OUT_OF_RESOURCES is non-zero, which is
 *     what makes the overflow assertions mean anything.
 * It is NOT a general vg_lite mock and must not grow into one: anything the
 * host tests do not touch belongs in the target build, not here. In
 * particular nothing here models vg_lite_draw, vg_lite_clear or any of the
 * completion path -- each suite supplies its own stand-in and says what that
 * stand-in does and does not model. */
#ifndef VGC_TEST_STUB_VG_LITE_H
#define VGC_TEST_STUB_VG_LITE_H

#include <stdint.h>
#include <stddef.h>

/* Real values, inc/vg_lite.h:62-67. */
#define VLC_OP_END      0x00
#define VLC_OP_CLOSE    0x01
#define VLC_OP_MOVE     0x02
#define VLC_OP_LINE     0x04

typedef enum { VG_LITE_SUCCESS = 0, VG_LITE_OUT_OF_RESOURCES = 5 } vg_lite_error_t;
typedef enum { VG_LITE_S8 = 0, VG_LITE_S16 = 1, VG_LITE_S32 = 2,
               VG_LITE_FP32 = 3 } vg_lite_format_t;
/* Real order and real values (inc/vg_lite.h:253-259) -- HIGH is 0 and LOW is
 * 3, i.e. the REVERSE of the intuitive reading, and VG_LITE_UPPER exists
 * between them. arena_test asserts on VG_LITE_HIGH, so a convenient ordering
 * here would be a stub vouching for a value the driver does not use. */
typedef enum { VG_LITE_HIGH = 0, VG_LITE_UPPER = 1, VG_LITE_MEDIUM = 2,
               VG_LITE_LOW = 3 } vg_lite_quality_t;
/* Real values (inc/vg_lite.h:514-518) -- EVEN_ODD is 0x1900 and NON_ZERO is
 * 0x1901, NOT 0 and 1. They were 0/1 here until Phase 2, which was this stub
 * vouching for values the driver does not use: nothing compares a fill rule to
 * a literal, so it was latent, but "latent" is how the quality_t ordering got
 * in too and it was corrected for the same reason earlier in this phase. A
 * stub is a claim about the real header; a claim that costs nothing to make
 * true has no excuse for being false. */
typedef enum { VG_LITE_FILL_EVEN_ODD = 0x1900,
               VG_LITE_FILL_NON_ZERO = 0x1901 } vg_lite_fill_t;
/* Only the two modes the harness names, at their real values
 * (inc/vg_lite.h:459-461). No suite renders through a blend mode -- each
 * supplies its own vgc_draw_path -- so this exists purely so
 * vgc_harness.h's vgc_draw_path_blend declaration parses on the host. */
typedef enum { VG_LITE_BLEND_NONE = 0, VG_LITE_BLEND_SRC_OVER = 1 } vg_lite_blend_t;

typedef struct { float m[3][3]; }            vg_lite_matrix_t;
typedef struct { void *memory; uint32_t address; int32_t width, height, stride; } vg_lite_buffer_t;

typedef struct vg_lite_path {
    float             bounding_box[4];
    vg_lite_quality_t quality;
    vg_lite_format_t  format;
    uint32_t          path_length;   /* bytes */
    void             *path;
} vg_lite_path_t;

vg_lite_error_t vg_lite_init_path(vg_lite_path_t *path,
                                  vg_lite_format_t format,
                                  vg_lite_quality_t quality,
                                  uint32_t length,
                                  void *data,
                                  float min_x, float min_y,
                                  float max_x, float max_y);

#endif /* VGC_TEST_STUB_VG_LITE_H */
