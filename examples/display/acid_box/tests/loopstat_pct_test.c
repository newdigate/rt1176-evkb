/* loopstat_pct_test.c -- host test for loopstat_pct.h (NEW-33).
 * PASS:/FAIL: per check, count at the end (the tree's convention:
 * `grep -c "^PASS:"` on a live run is the case count). */
#include <stdio.h>
#include <stdint.h>
#include "../loopstat_pct.h"

static int pass = 0, fail = 0;
static void check(const char *name, uint32_t got, uint32_t want)
{
    if (got == want) { printf("PASS: %s (%lu)\n", name, (unsigned long)got); pass++; }
    else { printf("FAIL: %s got %lu want %lu\n", name, (unsigned long)got, (unsigned long)want); fail++; }
}

int main(void)
{
    uint32_t buf[4], sorted[256];
    loopstat_ring_t r;

    /* empty ring: every percentile is 0, count 0 */
    loopstat_ring_init(&r, buf, 4);
    check("empty sorted count", loopstat_ring_sorted(&r, sorted), 0);
    check("empty p50", loopstat_pct_sorted(sorted, 0, 50), 0);

    /* one sample: every percentile is that sample */
    loopstat_ring_push(&r, 7);
    uint32_t n = loopstat_ring_sorted(&r, sorted);
    check("n=1 count", n, 1);
    check("n=1 p50", loopstat_pct_sorted(sorted, n, 50), 7);
    check("n=1 p95", loopstat_pct_sorted(sorted, n, 95), 7);
    check("n=1 p100", loopstat_pct_sorted(sorted, n, 100), 7);

    /* {4,1,3,2} -> sorted {1,2,3,4}: p50 = rank ceil(2) = 2nd = 2, p95 = rank 4 = 4 */
    loopstat_ring_reset(&r);
    loopstat_ring_push(&r, 4); loopstat_ring_push(&r, 1);
    loopstat_ring_push(&r, 3); loopstat_ring_push(&r, 2);
    n = loopstat_ring_sorted(&r, sorted);
    check("n=4 count", n, 4);
    check("n=4 sorted[0]", sorted[0], 1);
    check("n=4 sorted[3]", sorted[3], 4);
    check("n=4 p50", loopstat_pct_sorted(sorted, n, 50), 2);
    check("n=4 p95", loopstat_pct_sorted(sorted, n, 95), 4);
    check("n=4 p0 clamps to rank 1", loopstat_pct_sorted(sorted, n, 0), 1);

    /* wrap: cap 4, push 1..6 -> the ring keeps {3,4,5,6} */
    loopstat_ring_push(&r, 5); loopstat_ring_push(&r, 6);
    loopstat_ring_reset(&r);
    for (uint32_t v = 1; v <= 6; v++) loopstat_ring_push(&r, v);
    n = loopstat_ring_sorted(&r, sorted);
    check("wrap count", n, 4);
    check("wrap min", sorted[0], 3);
    check("wrap max", sorted[3], 6);

    /* 10 samples 10,9,...,1 -> p95 is the 10th smallest (rank ceil(9.5)=10), p50 the 5th */
    uint32_t big[16]; loopstat_ring_t rb; loopstat_ring_init(&rb, big, 16);
    for (uint32_t v = 10; v >= 1; v--) loopstat_ring_push(&rb, v);
    n = loopstat_ring_sorted(&rb, sorted);
    check("n=10 count", n, 10);
    check("n=10 p50", loopstat_pct_sorted(sorted, n, 50), 5);
    check("n=10 p95", loopstat_pct_sorted(sorted, n, 95), 10);
    check("n=10 p90", loopstat_pct_sorted(sorted, n, 90), 9);

    /* 100 samples 100..1 -> p50 = 50, p95 = 95, p100 = 100 */
    uint32_t hb[256]; loopstat_ring_t rh; loopstat_ring_init(&rh, hb, 256);
    for (uint32_t v = 100; v >= 1; v--) loopstat_ring_push(&rh, v);
    n = loopstat_ring_sorted(&rh, sorted);
    check("n=100 p50", loopstat_pct_sorted(sorted, n, 50), 50);
    check("n=100 p95", loopstat_pct_sorted(sorted, n, 95), 95);
    check("n=100 p100", loopstat_pct_sorted(sorted, n, 100), 100);

    printf("loopstat_pct_test: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
