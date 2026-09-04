/* loopstat_pct.h -- a ring of uint32 samples and a nearest-rank percentile
 * over it.  Pure C, header-only, no allocation: the caller owns the storage.
 * Used by acid_box's ACIDBOX_LOOPSTAT instrument (NEW-33) for the frame
 * interval and touch-to-frame latency distributions; host-tested in tests/.
 * Copyright (c) 2026 Nicholas Newdigate.  SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t *buf;      /* caller-owned storage, `cap` entries */
    uint32_t  cap;
    uint32_t  head;     /* next write index */
    uint32_t  count;    /* valid entries, <= cap */
} loopstat_ring_t;

static inline void loopstat_ring_init(loopstat_ring_t *r, uint32_t *buf, uint32_t cap)
{
    r->buf = buf; r->cap = cap; r->head = 0; r->count = 0;
}

static inline void loopstat_ring_reset(loopstat_ring_t *r) { r->head = 0; r->count = 0; }

/* Overwrites the oldest entry once full: the ring holds the `cap` most recent. */
static inline void loopstat_ring_push(loopstat_ring_t *r, uint32_t v)
{
    r->buf[r->head] = v;
    r->head = (r->head + 1 == r->cap) ? 0 : r->head + 1;
    if (r->count < r->cap) r->count++;
}

/* Copy the ring's valid entries into out[] (>= cap entries) sorted ascending
 * and return how many.  Every valid entry lives in buf[0..count): before the
 * first wrap head == count, and after it all cap entries are valid -- so the
 * write order is irrelevant to a sort.  Insertion sort: n <= 256, once/sec. */
static inline uint32_t loopstat_ring_sorted(const loopstat_ring_t *r, uint32_t *out)
{
    const uint32_t n = r->count;
    for (uint32_t i = 0; i < n; i++) out[i] = r->buf[i];
    for (uint32_t i = 1; i < n; i++) {
        const uint32_t v = out[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && out[j] > v) { out[j + 1] = out[j]; j--; }
        out[j + 1] = v;
    }
    return n;
}

/* Nearest-rank percentile of a sorted array: the value at 1-based rank
 * ceil(p * n / 100), clamped to [1, n].  n == 0 returns 0.
 * p50 of {1,2,3,4} is 2; p95 of 100 samples is the 95th smallest; p100 is
 * the max.  Integer ceil: (p*n + 99) / 100. */
static inline uint32_t loopstat_pct_sorted(const uint32_t *sorted, uint32_t n, uint32_t p)
{
    if (n == 0) return 0;
    uint32_t rank = (p * n + 99u) / 100u;
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return sorted[rank - 1];
}
