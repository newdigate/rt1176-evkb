/* Host-compiled unit test for the conformance probe's COLOUR predicates.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Companion to predicates_test.c, and separate for the same reason vgc_color.h
 * is separate from vgc_predicates.h: vgc_is_filled is a THRESHOLD valid only
 * under the black-background/white-fill convention, and says so. A pure-red
 * fill has green=0 and reads as UNFILLED for every pixel -- an instrument
 * fabricating a defect. These predicates answer a different question and carry
 * a different validity condition. */
#include "../vgc_color.h"
#include <stdio.h>

static int failed = 0, checks = 0;
#define CHECK(c) do { checks++; if (c) printf("PASS: %s\n", #c); \
    else { printf("FAIL: %s  (line %d)\n", #c, __LINE__); failed++; } } while (0)

int main(void)
{
    /* --- vgc_ch: the order-agnostic byte accessor ----------------------- */
    CHECK(vgc_ch(0x11223344u, 0) == 0x44);
    CHECK(vgc_ch(0x11223344u, 1) == 0x33);
    CHECK(vgc_ch(0x11223344u, 2) == 0x22);
    CHECK(vgc_ch(0x11223344u, 3) == 0x11);
    /* a ZERO byte reads as 0 and not as the sentinel -- pinned at the
     * accessor rather than two layers up in a counter, where a confusion
     * between "byte is 0x00" and "index was bad" would arrive pre-summed */
    CHECK(vgc_ch(0xFF0000FFu, 1) == 0x00);
    /* out of range must be impossible to mistake for a real byte, the way
     * vgc_count_runs_col returns -1 rather than a plausible 0 */
    CHECK(vgc_ch(0x11223344u, 4) == -1);
    CHECK(vgc_ch(0x11223344u, -1) == -1);

    /* --- saturation / zero counts: how case 1 asserts identity without
     * naming a channel -------------------------------------------------- */
    CHECK(vgc_saturated_channels(0x00FF0000u) == 1);
    CHECK(vgc_saturated_channels(0xFFFFFFFFu) == 4);
    CHECK(vgc_saturated_channels(0x00000000u) == 0);
    /* 0xFE is NOT saturated -- the identity test must not accept "nearly" */
    CHECK(vgc_saturated_channels(0x00FE0000u) == 0);
    CHECK(vgc_zero_channels(0x00FF0000u) == 3);
    CHECK(vgc_zero_channels(0x00000000u) == 4);
    CHECK(vgc_zero_channels(0x01010101u) == 0);
    /* the one word where the two counters TOGETHER show a byte is NEITHER
     * saturated nor zero: 0 + 3 leaves the 0xFE unaccounted for. Either
     * counter alone would report a plausible number here */
    CHECK(vgc_zero_channels(0x00FE0000u) == 3);
    /* the exact shape color/solid-word-order sees: an OPAQUE pure-red pixel is
     * TWO saturated (red + alpha) and TWO zero (green, blue) */
    CHECK(vgc_saturated_channels(0xFF0000FFu) == 2);
    CHECK(vgc_zero_channels(0xFF0000FFu) == 2);

    /* --- vgc_near ------------------------------------------------------- */
    CHECK(vgc_near(128, 128, 0) == 1);
    CHECK(vgc_near(124, 128, 4) == 1);
    CHECK(vgc_near(132, 128, 4) == 1);
    CHECK(vgc_near(123, 128, 4) == 0);
    CHECK(vgc_near(133, 128, 4) == 0);
    /* symmetric: a one-sided tolerance would let a whole class of undershoot
     * through */
    CHECK(vgc_near(128, 124, 4) == 1);
    CHECK(vgc_near(128, 133, 4) == 0);
    /* a negative tolerance fails even the identity, ON PURPOSE -- see the
     * header. Clamping tol to 0 would make this a PASS and hide the caller's
     * bug; this check exists so a tidy-up cannot make that change quietly */
    CHECK(vgc_near(128, 128, -1) == 0);

    printf("--\n");
    if (failed) {
        printf("color_test: FAILED (%d of %d checks)\n", failed, checks);
        return 1;
    }
    printf("color_test: OK (%d checks)\n", checks);
    return 0;
}
