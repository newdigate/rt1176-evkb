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
 * anything.
 *
 * ★ CHECK(), NOT assert(), AND IT KEEPS GOING. Every other suite in this tree
 * (tools/gate-vacuity.test.sh and siblings) prints PASS:/FAIL: per case and
 * runs to the end -- CLAUDE.md treats `grep -c "^PASS:"` on a live run as the
 * authoritative case count. assert() aborts on the first failure, so a broken
 * vgc_count_filled would leave you knowing NOTHING about vgc_fnv. For an
 * instrument, "which predicates are still trustworthy" is precisely the
 * question a red run has to answer. CHECK is a plain `if`, so unlike assert()
 * it cannot be compiled away by -DNDEBUG at all -- strictly stronger than the
 * `#undef NDEBUG` this file used to carry. */
#include "../vgc_predicates.h"
#include <stdio.h>

static int failed = 0;
static int checks = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        checks++;                                                        \
        if (cond) {                                                      \
            printf("PASS: %s\n", #cond);                                 \
        } else {                                                         \
            printf("FAIL: %s  (line %d)\n", #cond, __LINE__);            \
            failed++;                                                    \
        }                                                                \
    } while (0)

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
    printf("-- vgc_is_filled --\n");
    CHECK(vgc_is_filled(0xFFFFFFFFu) == 1);   /* white  */
    CHECK(vgc_is_filled(0xFF000000u) == 0);   /* black  */
    CHECK(vgc_is_filled(0xFF008000u) == 1);   /* g=0x80, exactly at the threshold */
    CHECK(vgc_is_filled(0xFF007F00u) == 0);   /* g=0x7F, just under */
    /* Immune to R/B word order: a pure-red pixel is UNFILLED whichever way
     * round the driver packs it, and a pure-green pixel is FILLED either way. */
    CHECK(vgc_is_filled(0xFFFF0000u) == 0);
    CHECK(vgc_is_filled(0xFF0000FFu) == 0);
    CHECK(vgc_is_filled(0xFF00FF00u) == 1);

    /* --- vgc_count_filled ----------------------------------------------- */
    printf("-- vgc_count_filled --\n");
    clear_bg();
    CHECK(vgc_count_filled(buf, W, H, W) == 0);
    set_fill(2, 3, 10, 7);                       /* 8 wide, 4 tall = 32 */
    CHECK(vgc_count_filled(buf, W, H, W) == 32);
    clear_bg();
    set_fill(0, 0, W, H);
    CHECK(vgc_count_filled(buf, W, H, W) == W * H);

    /* stride is in WORDS and may exceed the width: pixels past `w` on a row
     * must NOT be counted (the scratch buffer's stride is a byte stride the
     * caller converts, and a stride bug is exactly the kind of off-by-a-row
     * error that would make every case wrong in the same plausible way) */
    clear_bg();
    for (int y = 0; y < H; y++) buf[y * W + (W - 1)] = 0xFFFFFFFFu;  /* last column */
    CHECK(vgc_count_filled(buf, W - 1, H, W) == 0);
    CHECK(vgc_count_filled(buf, W,     H, W) == H);

    /* ★ `h` needs the same pinning `w` gets from the stride case above, and
     * for the same reason: every OTHER call here passes h == H, so a `y <= h`
     * one-row overrun would read a row past the region and no case could see
     * it. Fill row 8 and ask for h=8 -- rows 0..7 -- which must ignore it. */
    clear_bg();
    set_fill(0, 8, W, 9);                        /* row 8 only */
    CHECK(vgc_count_filled(buf, W, 8, W) == 0);  /* h=8 covers rows 0..7 */
    CHECK(vgc_count_filled(buf, W, 9, W) == W);  /* h=9 reaches it */

    /* --- vgc_count_runs_col: the multi-contour predicate ------------------ */
    printf("-- vgc_count_runs_col --\n");
    clear_bg();
    CHECK(vgc_count_runs_col(buf, W, H, W, 8) == 0);
    set_fill(0, 2, W, 4);                        /* one band */
    CHECK(vgc_count_runs_col(buf, W, H, W, 8) == 1);
    set_fill(0, 6, W, 8);
    set_fill(0, 10, W, 12);
    set_fill(0, 14, W, 16);                      /* four bands, last touching the edge */
    CHECK(vgc_count_runs_col(buf, W, H, W, 8) == 4);
    /* a band running to the bottom edge still closes its run */
    clear_bg();
    set_fill(0, 14, W, 16);
    CHECK(vgc_count_runs_col(buf, W, H, W, 8) == 1);
    /* adjacent bands with no gap are ONE run -- this is what makes the
     * predicate answer "how many contours rendered", not "how many were
     * emitted": four bars merged into one solid block must read as 1 */
    clear_bg();
    set_fill(0, 2, W, 6);
    set_fill(0, 6, W, 10);
    CHECK(vgc_count_runs_col(buf, W, H, W, 8) == 1);
    /* the column argument is honoured */
    clear_bg();
    set_fill(0, 2, 4, 6);                        /* only x in [0,4) */
    CHECK(vgc_count_runs_col(buf, W, H, W, 2) == 1);
    CHECK(vgc_count_runs_col(buf, W, H, W, 8) == 0);

    /* ★ THE BOUNDS GUARD NEEDS ITS OWN CASES AT THE EDGES. Sampling only
     * interior columns (2 and 8 of 16) leaves the guard unpinned three ways:
     * deleting it, `x >= w` -> `x >= w-1` (rejects the rightmost column) and
     * `x < 0` -> `x <= 0` (rejects column 0) all survive. On a 128-wide target
     * buffer a case sampling column 0 or 127 is entirely plausible, so the
     * columns most likely to be asked for are the ones least tested.
     * Out-of-range must read -1 and never 0 -- see the header: 0 is a real
     * answer, so returning it for a bad index makes the instrument fabricate
     * a GC355 defect out of a caller's typo. */
    clear_bg();
    set_fill(0, 2, W, 4);                        /* full-width band */
    CHECK(vgc_count_runs_col(buf, W, H, W, 0) == 1);        /* leftmost column */
    CHECK(vgc_count_runs_col(buf, W, H, W, W - 1) == 1);    /* rightmost column */
    CHECK(vgc_count_runs_col(buf, W, H, W, -1) == -1);      /* below range */
    CHECK(vgc_count_runs_col(buf, W, H, W, W) == -1);       /* at/above range */

    /* --- vgc_filled_rows: the degenerate-geometry extent ------------------ */
    printf("-- vgc_filled_rows --\n");
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
    CHECK(vgc_filled_rows(buf, W, H, W, &ymin, &ymax) == 0);
    CHECK(ymin == -99 && ymax == -99);            /* untouched when nothing is filled */
    set_fill(3, 7, 9, 9);                         /* rows 7..8 */
    CHECK(vgc_filled_rows(buf, W, H, W, &ymin, &ymax) == 12);
    CHECK(ymin == 7 && ymax == 8);

    /* A SINGLE filled row -- ymin == ymax. Per the header this is the very
     * case the function exists for (degenerate zero-area geometry that
     * renders as one row, or as nothing), and a multi-row case cannot stand
     * in for it: an off-by-one in the extent shows up as a 2-row answer where
     * a 1-row answer is correct. */
    int y1 = -99, y2 = -99;
    clear_bg();
    set_fill(3, 5, 9, 6);                         /* row 5 only, 6 px wide */
    CHECK(vgc_filled_rows(buf, W, H, W, &y1, &y2) == 6);
    CHECK(y1 == 5 && y2 == 5);

    /* --- vgc_fnv: pinned against the reference FNV-1a --------------------- */
    printf("-- vgc_fnv --\n");
    /* offset basis alone, for a zero-length input */
    CHECK(vgc_fnv("", 0) == 2166136261u);
    /* "a" -> 0xE40C292C, the published FNV-1a 32-bit test vector */
    CHECK(vgc_fnv("a", 1) == 0xE40C292Cu);
    /* "foobar" -> 0xBF9CF968, ditto */
    CHECK(vgc_fnv("foobar", 6) == 0xBF9CF968u);
    /* order-sensitive: a checksum that is not is useless as a repeat probe */
    CHECK(vgc_fnv("ab", 2) != vgc_fnv("ba", 2));

    /* ★ EMBEDDED NUL BYTES -- the case the real input actually is. Every
     * vector above is a NUL-free C string, so a strlen-flavoured
     * `i < n && b[i]` mutation survives all of them. It must not: the scratch
     * buffer is cleared to opaque black = the BGRA8888 word 0xFF000000, whose
     * first byte little-endian is 0x00, so a NUL-stopping vgc_fnv returns the
     * offset basis for EVERY render and repeat= reports "identical"
     * unconditionally -- the determinism check vacuously green forever, and
     * nothing else in the probe can see it.
     *
     * Both pins were computed independently in python3 from the FNV-1a
     * definition (verified against the three published vectors above), NOT
     * read off this implementation. */
    static const unsigned char z[4] = { 0x00, 0x00, 0x00, 0xFF };  /* one black pixel */
    CHECK(vgc_fnv(z, 4) == 0xDC954658u);
    CHECK(vgc_fnv(z, 1) == 0x050C5D1Fu);
    /* `n` is honoured rather than the content: a NUL-stopper collapses both
     * of these to the bare offset basis, so they stop differing */
    CHECK(vgc_fnv(z, 4) != vgc_fnv(z, 1));
    CHECK(vgc_fnv(z, 4) != 2166136261u);

    printf("--\n");
    if (failed) {
        printf("predicates_test: FAILED (%d of %d checks)\n", failed, checks);
        return 1;
    }
    printf("predicates_test: OK (%d checks)\n", checks);
    return 0;
}
