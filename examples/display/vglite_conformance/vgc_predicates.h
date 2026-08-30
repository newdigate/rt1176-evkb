/* vgc_predicates.h - PURE pixel-buffer predicates for the GC355 conformance
 * probe. No vg_lite, no Arduino, no target headers: this file compiles on the
 * host and is unit-tested there (tests/predicates_test.c).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec: docs/superpowers/specs/2026-08-30-gc355-conformance-design.md section 5.
 *
 * ★ STRUCTURAL, NEVER CHECKSUMS. A predicate here answers a question about
 * GEOMETRY -- how many separated runs are filled down this column, is the
 * centre of this ring background -- so it stays true across antialiasing
 * differences, driver revisions and the two engines. A golden checksum
 * answers "did this render change", which decays into a re-goldening ritual
 * the moment anything legitimately moves. The one checksum here (vgc_fnv) is
 * used ONLY to compare a render against an identical re-render in the same
 * boot, which is a question about determinism rather than correctness.
 *
 * SHARED PRECONDITION: `stride_words` is the row pitch in 32-bit WORDS and
 * must be >= w. It MAY exceed w (pixels past w on a row are ignored); it must
 * never be smaller. Nothing here range-checks it -- every caller is in-tree
 * and a runtime check would buy nothing for an instrument whose whole value
 * is that it is simple enough to be obviously right. */
#ifndef VGC_PREDICATES_H
#define VGC_PREDICATES_H

#include <stddef.h>
#include <stdint.h>

/* ★ Threshold the GREEN channel at the midpoint.
 *
 * Green occupies bits 15:8 in BOTH ARGB and ABGR, so this predicate cannot be
 * fooled by the word-order question (vg_lite_color_t is ABGR; the BGRA8888
 * target's memory words are ARGB) that Phase 2's colour cases exist to settle.
 * Reading red or blue here would silently make every Phase-1 answer depend on
 * an unsettled Phase-2 fact.
 *
 * The scratch buffer is cleared to opaque BLACK and every Phase-1 case fills
 * with opaque WHITE, so the midpoint is the ~50 % coverage contour whatever
 * the driver does about antialiasing -- interior pixels are fully white,
 * exterior fully black, and only the boundary lands in between. That is what
 * makes a filled-pixel COUNT comparable with an analytic area.
 *
 * ★ NOT VALID OUTSIDE THAT BLACK-BACKGROUND/WHITE-FILL CONVENTION. This is a
 * threshold, not a "was anything drawn here" test: a pure-red or pure-blue
 * fill has green=0 and reads as UNFILLED for every pixel, which a caller
 * would see as "nothing rendered" -- an instrument fabricating a defect.
 * Phase 2's colour cases must bring their own predicate rather than reusing
 * this one. */
static inline int vgc_is_filled(uint32_t px)
{
    return ((px >> 8) & 0xFFu) >= 0x80u;
}

/* Filled pixels in the w x h region. `stride_words` is the row pitch in
 * 32-bit words and may exceed `w`; pixels beyond `w` on a row are NOT
 * counted, and rows at or beyond `h` are NOT read. */
static inline int vgc_count_filled(const uint32_t *fb, int w, int h,
                                   int stride_words)
{
    int n = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (vgc_is_filled(fb[(size_t)y * stride_words + x])) n++;
    return n;
}

/* Number of SEPARATED filled runs down column `x`, top to bottom.
 * Returns -1 if `x` is outside [0, w).
 *
 * This is the multi-contour predicate: four disjoint bars emitted in one path
 * give 4 if every contour rendered and 1 if only the first did (or if they
 * merged). Adjacent runs with no background row between them count as ONE --
 * deliberately, because "how many distinct filled regions are visible" is the
 * question, not "how many rectangles were emitted".
 *
 * ★ OUT-OF-RANGE RETURNS -1, NOT 0, and the distinction is the whole point.
 * 0 is a legitimate answer meaning "this column has no filled runs", so
 * returning it for a bad index would make a typo'd column in a caller's
 * check() read as "the contour did not render" -- the instrument inventing a
 * GC355 defect out of its own bug, which is the exact risk this probe exists
 * to avoid. Same reasoning as vgc_filled_rows' untouched out-params below:
 * an error must never be spellable as a valid measurement. Callers that can
 * pass a computed column should test for < 0. */
static inline int vgc_count_runs_col(const uint32_t *fb, int w, int h,
                                     int stride_words, int x)
{
    if (x < 0 || x >= w) return -1;
    int runs = 0, in = 0;
    for (int y = 0; y < h; y++) {
        const int f = vgc_is_filled(fb[(size_t)y * stride_words + x]);
        if (f && !in) runs++;
        in = f;
    }
    return runs;
}

/* Filled-pixel count, and the first/last row containing any filled pixel.
 * `*ymin`/`*ymax` are left UNTOUCHED when nothing is filled -- an out-param
 * that reads 0 in that case would be indistinguishable from "row 0 is
 * filled", which is exactly the ambiguity the degenerate-geometry case has to
 * resolve.
 *
 * ★ THE CALLER IS OBLIGED TO SEED BOTH with a sentinel outside [0, h), and
 * -1 SPECIFICALLY WILL NOT DO: this function tracks its extent in internal
 * lo/hi initialised to -1, so a caller seeding -1 cannot distinguish
 * "untouched" from a defect that writes the internal state through
 * unconditionally. Measured -- with the `if (n)` guard below removed and -1
 * sentinels, the unit test stayed GREEN. tests/predicates_test.c uses -99 and
 * carries the ★ note. */
static inline int vgc_filled_rows(const uint32_t *fb, int w, int h,
                                  int stride_words, int *ymin, int *ymax)
{
    int n = 0, lo = -1, hi = -1;
    for (int y = 0; y < h; y++) {
        int row = 0;
        for (int x = 0; x < w; x++)
            if (vgc_is_filled(fb[(size_t)y * stride_words + x])) row++;
        if (row) { if (lo < 0) lo = y; hi = y; n += row; }
    }
    if (n) { *ymin = lo; *ymax = hi; }
    return n;
}

/* FNV-1a over raw bytes. Same arithmetic as every other checksum in this tree
 * so the numbers are comparable in kind. Used ONLY for the repeat= check.
 *
 * ★ IT HASHES `n` BYTES, NUL BYTES INCLUDED -- it is not a string function,
 * and the real input begins with a NUL. The cleared scratch buffer is opaque
 * black = the BGRA8888 word 0xFF000000, whose first byte little-endian is
 * 0x00. A strlen-flavoured `i < n && b[i]` would therefore return the offset
 * basis for EVERY render, making repeat= report "identical" unconditionally:
 * a determinism check vacuously green forever, with nothing else in the probe
 * able to see it. That is the guard class that caught NEW-20's winding-2
 * per-boot nondeterminism, so it must not be allowed to rot. */
static inline uint32_t vgc_fnv(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t sum = 2166136261u;
    for (size_t i = 0; i < n; i++) { sum ^= b[i]; sum *= 16777619u; }
    return sum;
}

#endif /* VGC_PREDICATES_H */
