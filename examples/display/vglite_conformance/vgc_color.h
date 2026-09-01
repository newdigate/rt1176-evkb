/* vgc_color.h - PURE colour predicates for the GC355 conformance probe. No
 * vg_lite, no Arduino, no target headers: this file compiles on the host and
 * is unit-tested there (tests/color_test.c).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec: docs/superpowers/specs/2026-09-01-gc355-conformance-phase2-design.md
 *
 * SEPARATE FROM vgc_predicates.h ON PURPOSE. That file's central predicate,
 * vgc_is_filled, is a THRESHOLD on the green channel valid ONLY under the
 * black-background/white-fill convention, and its own header says so: a
 * pure-red fill has green=0 and reads as UNFILLED for every pixel. Different
 * question, different validity condition, different file. */
#ifndef VGC_COLOR_H
#define VGC_COLOR_H

#include <stdint.h>

/* Byte `i` of a memory word, i in [0,3], 0 = least significant.
 *
 * ★ THE ORDER-AGNOSTIC ACCESSOR EVERYTHING ELSE IS BUILT FROM. It deliberately
 * says nothing about which byte is red: that is what color/solid-word-order
 * re-confirms per boot, and a predicate that assumed it would make every
 * Phase 2 answer depend on a fact the matrix is supposed to be checking.
 *
 * Returns -1 out of range rather than a plausible byte value -- the same rule
 * vgc_count_runs_col follows, so an instrument bug cannot be spelled as a
 * valid BYTE.
 *
 * ★ BUT THE SENTINEL ONLY SURVIVES IF THE CALLER LOOKS, and this header used
 * to overclaim that it always does. Same obligation vgc_predicates.h puts on
 * vgc_count_runs_col: a caller passing a COMPUTED index must test for < 0.
 * MEASURED composition hazard -- vgc_near(vgc_ch(px, 7), 0, 4) returns 1,
 * because -1 sits comfortably inside a tolerance window straddling zero, so
 * the error is accepted as a PASS. -1 is unmistakable as a byte and entirely
 * mistakable once fed to a near-zero test, which is exactly the shape a
 * Phase 3 "the backdrop channel stayed near 0" case would take. Check the
 * index, or read the channel through a named VGC_* constant below. */
static inline int vgc_ch(uint32_t px, int i)
{
    if (i < 0 || i > 3) return -1;
    return (int)((px >> (i * 8)) & 0xFFu);
}

/* How many of the four bytes are exactly 0xFF. 0xFE does not count: the
 * identity test must not accept "nearly".
 *
 * ★ IT COUNTS ALL FOUR BYTES, ALPHA INCLUDED -- so an OPAQUE fill saturates
 * TWO of them, not one. color/solid-word-order fills pure red into an opaque
 * scratch, so the answer that case must assert is `sat == 2 && zero == 2`:
 * red and alpha at 0xFF, green and blue at 0x00. Writing `sat == 1` because
 * the fill is "one colour" makes the case report pixel=broken on CORRECT
 * silicon -- the instrument fabricating a defect, in the one case every later
 * colour verdict is read against. (An earlier draft of this comment said
 * "exactly one channel is saturated"; tests/color_test.c asserted the true
 * shape and the prose was wrong, not the test.)
 *
 * ★ NECESSARY, NOT SUFFICIENT. Being order-agnostic is what makes these
 * counts usable before the word order is settled, and it is also what they
 * cost: `sat == 2 && zero == 2` is equally satisfied by two saturated COLOUR
 * channels with alpha 0x00 -- a fully transparent pixel of the wrong colour.
 * What actually closes the case is the NAMED half the counts cannot express,
 * vgc_ch(px, VGC_A) == 0xFF together with vgc_ch(px, VGC_R) == 0xFF. Assert
 * both halves. */
static inline int vgc_saturated_channels(uint32_t px)
{
    int n = 0;
    for (int i = 0; i < 4; i++) if (vgc_ch(px, i) == 0xFF) n++;
    return n;
}

/* How many of the four bytes are exactly 0x00. Counterpart to the above, and
 * the pair is what makes "neither saturated nor zero" a visible answer rather
 * than an absence: for 0x00FE0000 the two counters read 0 and 3, and the
 * missing fourth byte is the 0xFE. */
static inline int vgc_zero_channels(uint32_t px)
{
    int n = 0;
    for (int i = 0; i < 4; i++) if (vgc_ch(px, i) == 0x00) n++;
    return n;
}

/* |a - b| <= tol, symmetric. Every blend tolerance is spelled at its call site
 * rather than hidden in a helper, so the number a case depends on is visible
 * in the case.
 *
 * ★ A NEGATIVE `tol` RETURNS 0 FOR EVERY INPUT, INCLUDING a == b, AND IS
 * DELIBERATELY UNGUARDED. This function returns a boolean, so the -1 sentinel
 * idiom vgc_ch uses is unavailable, and the obvious repair is worse than the
 * gap: clamping tol to 0 would turn vgc_near(128, 128, -1) into a PASS,
 * hiding a caller's bug behind a plausible answer. Failing loudly on every
 * input sends a case built on a negative tolerance straight to broken instead
 * of quietly narrowing its window. tests/color_test.c pins this so a later
 * tidy-up cannot silently normalise it. */
static inline int vgc_near(int a, int b, int tol)
{
    const int d = a - b;
    return (d >= -tol && d <= tol);
}

/* ---- named channel indices ------------------------------------------------
 * ★ MEASURED ON SILICON BY vglite_probe -- not assumed here, and not waiting
 * on a Phase 2 case to become true.
 *
 * The scratch is VG_LITE_BGRA8888, the same format vglite_probe clears, and
 * that probe settled the memory order by experiment: clearing with the
 * vg_lite_color_t 0xFF204060 -- which the driver reads as ABGR, so B=0x20,
 * G=0x40, R=0x60 -- put 0xFF604020 in memory. Red came back in bits 23:16.
 * A BGRA8888 memory WORD is therefore ARGB, and red is byte 2.
 * vglite_probe.cpp:56-59 carries that reading, along with the reason it had
 * to be measured: getting the two orders backwards does not fail, it renders
 * the wrong colour while every status says success. (That is also why
 * vg_lite_color_t arguments go through VGC_ABGR rather than being spelled as
 * literals -- vg_lite_color_t is ABGR, these indices are into the ARGB
 * memory word, and they are not the same order.)
 *
 * color/solid-word-order RE-CONFIRMS the mapping on THIS scratch buffer in
 * THIS boot; it is not the origin of the claim. What it adds is a per-boot
 * check that a driver or SDK bump has not moved the order under us -- the
 * role path/single-contour-rect plays for geometry -- so read its result
 * before trusting any colour verdict below it. */
#define VGC_B 0
#define VGC_G 1
#define VGC_R 2
#define VGC_A 3

#endif /* VGC_COLOR_H */
