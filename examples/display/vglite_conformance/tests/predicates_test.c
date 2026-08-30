/* Host-compiled unit test for the conformance probe's PIXEL PREDICATES --
 * no vg_lite, no Arduino, no target.  Build: tests/run.sh
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * These predicates are the instrument. Spec section 11 names "a predicate is
 * wrong rather than the hardware" as a risk of the whole exercise, and a
 * predicate that miscounts on silicon is indistinguishable from silicon that
 * misrenders. Synthetic buffers with KNOWN answers are the only place that
 * confusion can be settled, so it is settled here, before any GPU is asked
 * anything. */
#undef NDEBUG          /* assertions must survive -DNDEBUG; BEFORE every include */
#include "../vgc_predicates.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define W 16
#define H 16
static uint32_t buf[W * H];

static void clear_bg(void)
{
    for (int i = 0; i < W * H; i++) buf[i] = 0xFF000000u;   /* opaque black */
}
static void set_fill(int x0, int y0, int x1, int y1)        /* half-open */
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) buf[y * W + x] = 0xFFFFFFFFu;
}

int main(void)
{
    /* --- vgc_is_filled: the midpoint threshold, on the GREEN channel ---- */
    assert(vgc_is_filled(0xFFFFFFFFu) == 1);   /* white  */
    assert(vgc_is_filled(0xFF000000u) == 0);   /* black  */
    assert(vgc_is_filled(0xFF008000u) == 1);   /* g=0x80, exactly at the threshold */
    assert(vgc_is_filled(0xFF007F00u) == 0);   /* g=0x7F, just under */
    /* Immune to R/B word order: a pure-red pixel is UNFILLED whichever way
     * round the driver packs it, and a pure-green pixel is FILLED either way. */
    assert(vgc_is_filled(0xFFFF0000u) == 0);
    assert(vgc_is_filled(0xFF0000FFu) == 0);
    assert(vgc_is_filled(0xFF00FF00u) == 1);

    /* --- vgc_count_filled ----------------------------------------------- */
    clear_bg();
    assert(vgc_count_filled(buf, W, H, W) == 0);
    set_fill(2, 3, 10, 7);                       /* 8 wide, 4 tall = 32 */
    assert(vgc_count_filled(buf, W, H, W) == 32);
    clear_bg();
    set_fill(0, 0, W, H);
    assert(vgc_count_filled(buf, W, H, W) == W * H);

    /* stride is in WORDS and may exceed the width: pixels past `w` on a row
     * must NOT be counted (the scratch buffer's stride is a byte stride the
     * caller converts, and a stride bug is exactly the kind of off-by-a-row
     * error that would make every case wrong in the same plausible way) */
    clear_bg();
    for (int y = 0; y < H; y++) buf[y * W + (W - 1)] = 0xFFFFFFFFu;  /* last column */
    assert(vgc_count_filled(buf, W - 1, H, W) == 0);
    assert(vgc_count_filled(buf, W,     H, W) == H);

    /* --- vgc_count_runs_col: the multi-contour predicate ------------------ */
    clear_bg();
    assert(vgc_count_runs_col(buf, W, H, W, 8) == 0);
    set_fill(0, 2, W, 4);                        /* one band */
    assert(vgc_count_runs_col(buf, W, H, W, 8) == 1);
    set_fill(0, 6, W, 8);
    set_fill(0, 10, W, 12);
    set_fill(0, 14, W, 16);                      /* four bands, last touching the edge */
    assert(vgc_count_runs_col(buf, W, H, W, 8) == 4);
    /* a band running to the bottom edge still closes its run */
    clear_bg();
    set_fill(0, 14, W, 16);
    assert(vgc_count_runs_col(buf, W, H, W, 8) == 1);
    /* adjacent bands with no gap are ONE run -- this is what makes the
     * predicate answer "how many contours rendered", not "how many were
     * emitted": four bars merged into one solid block must read as 1 */
    clear_bg();
    set_fill(0, 2, W, 6);
    set_fill(0, 6, W, 10);
    assert(vgc_count_runs_col(buf, W, H, W, 8) == 1);
    /* the column argument is honoured */
    clear_bg();
    set_fill(0, 2, 4, 6);                        /* only x in [0,4) */
    assert(vgc_count_runs_col(buf, W, H, W, 2) == 1);
    assert(vgc_count_runs_col(buf, W, H, W, 8) == 0);

    /* --- vgc_filled_rows: the degenerate-geometry extent ------------------ */
    /* ★ THE SENTINEL MUST BE A VALUE THE IMPLEMENTATION CANNOT PRODUCE.
     * vgc_filled_rows tracks its extent in internal lo/hi initialised to -1,
     * so a test that primed ymin/ymax to -1 could not see the single most
     * likely way to break the untouched contract: dropping the `if (n)` guard
     * writes -1 over -1 and the assertion below passes unchanged. Measured --
     * with the guard removed and -1 sentinels, this suite stayed GREEN. -99 is
     * outside the implementation's whole value space (a real answer is a row
     * index in [0,h), an untouched answer is whatever we put here), so the
     * assertion now has power in exactly the case it exists for. */
    int ymin = -99, ymax = -99;
    clear_bg();
    assert(vgc_filled_rows(buf, W, H, W, &ymin, &ymax) == 0);
    assert(ymin == -99 && ymax == -99);           /* untouched when nothing is filled */
    set_fill(3, 7, 9, 9);                         /* rows 7..8 */
    assert(vgc_filled_rows(buf, W, H, W, &ymin, &ymax) == 12);
    assert(ymin == 7 && ymax == 8);

    /* --- vgc_fnv: pinned against the reference FNV-1a --------------------- */
    /* offset basis alone, for a zero-length input */
    assert(vgc_fnv("", 0) == 2166136261u);
    /* "a" -> 0xE40C292C, the published FNV-1a 32-bit test vector */
    assert(vgc_fnv("a", 1) == 0xE40C292Cu);
    /* "foobar" -> 0xBF9CF968, ditto */
    assert(vgc_fnv("foobar", 6) == 0xBF9CF968u);
    /* order-sensitive: a checksum that is not is useless as a repeat probe */
    assert(vgc_fnv("ab", 2) != vgc_fnv("ba", 2));

    printf("predicates_test: OK\n");
    return 0;
}
