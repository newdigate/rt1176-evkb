/* tests/harness_mirror.h - THE TARGET HARNESS'S CASE LIFECYCLE, mirrored for
 * the host case-geometry suites.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * ★★ WHAT THIS IS. On the board, run_case() in vglite_conformance.cpp puts a
 * vgc_case_t through a fixed sequence -- reset the arena, reset the px-oob
 * counter, clear, run, sum, check -- and then, for the repeat= column, resets
 * the arena, clears, RUNS AGAIN and sums again. A host suite that ran a case
 * any other way would be measuring it under a lifecycle it never sees on
 * silicon. This file is that sequence, and the only one in the tree.
 *
 * ★★ WHY IT IS A HEADER RATHER THAN A COPY PER SUITE, which is where it
 * started (it lived in cases_path_geom_test.cpp through Phase 1). The Phase 2
 * colour suite needs the identical sequence, and the failure mode of a second
 * copy is SILENT: a copy that forgot the vgc_arena_reset() between the two
 * runs would still run both, still compare both sums, and still print
 * "repeat identical" -- against a second render that overflowed the arena and
 * drew nothing. The suite would go on reporting a repeat check it had stopped
 * performing. Exactly the argument that promoted the blend into model.h.
 *
 * ★ WHY NOT IN model.h. That file states its own scope in its header -- "it
 * knows about pixels and paths" -- and forbids growing anything that knows
 * about the harness's case protocol. The two are checked against DIFFERENT
 * sources: model.h against the driver and the measured framebuffer layout,
 * this file against run_case() in vglite_conformance.cpp. Keeping them apart
 * keeps each one's reference visible. It also costs nothing: everything here
 * is `static`, so unlike model.h -- whose external-linkage definitions make a
 * second includer a link error by design -- this header may be included by as
 * many TUs as ever need it.
 *
 * ★ IT SAYS NOTHING ABOUT THE SILICON, for the same reason model.h does not.
 * There is no GPU behind any of this. */
#ifndef VGC_TEST_HARNESS_MIRROR_H
#define VGC_TEST_HARNESS_MIRROR_H

#include "../vgc_harness.h"
#include <stdint.h>
#include <string.h>

typedef struct {
    vgc_verdict_t   verdict;
    vg_lite_error_t api;
    vg_lite_error_t api2;      /* the SECOND run's status -- the harness prints it */
    uint32_t        oob;
    int             repeat_same;
    int             hook_distinct;  /* a sum() hook must not be the live buffer */
    char            detail[VGC_DETAIL_MAX];
} case_result_t;

static void run_one(const vgc_case_t *c, case_result_t *r)
{
    memset(r, 0, sizeof(*r));
    vgc_arena_reset();
    vgc_px_oob_reset();
    vgc_clear();
    r->api = c->run();
    const uint32_t live1 = vgc_scratch_sum();
    const uint32_t sum1  = c->sum ? c->sum() : live1;
    /* ★ A HOOK THAT RETURNS THE LIVE BUFFER IS A HOOK THAT DOES NOTHING, and
     * the failure is invisible: repeat= keeps comparing something, just not
     * every sub-render. vgc_harness.h names a multi-render case without a
     * working hook as its top risk, and this is the only place that can see
     * it. Cases WITHOUT a hook are exempt by construction -- for them sum1 IS
     * live1. */
    r->hook_distinct = c->sum ? (sum1 != live1) : 1;
    r->verdict = c->check(r->detail, sizeof(r->detail));
    r->oob = vgc_px_oob();

    vgc_arena_reset();
    vgc_clear();
    r->api2 = c->run();
    const uint32_t sum2 = c->sum ? c->sum() : vgc_scratch_sum();
    r->repeat_same = (sum1 == sum2);
}

#endif /* VGC_TEST_HARNESS_MIRROR_H */
