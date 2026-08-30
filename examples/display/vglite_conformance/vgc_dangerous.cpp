/* vgc_dangerous.cpp - cases that can HANG the Vivante front end, compiled
 * ONLY under -DVGC_DANGEROUS=1.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec section 6. The default build's matrix must never depend on the port's
 * bounded waits surviving a hang -- a hang there costs the whole matrix and a
 * bench cycle. Each case prints its case_begin line before issuing the call
 * (the harness does that for every case), so a hang is attributable from the
 * transcript, and vgc_summary's dangerous=on/off records which build produced
 * a given matrix.
 *
 * Build the opt-in matrix in its own directory, never in build/:
 *   cmake -B build-danger -DVGC_DANGEROUS=ON \
 *         -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
 *   cmake --build build-danger
 * The gate only ever runs build/, so it always sees dangerous=off. */
#include "vgc_harness.h"

#ifdef VGC_DANGEROUS

#include "vgc_predicates.h"
#include <stdio.h>
#include <string.h>

/* path/unterminated -- path data with NO VLC_OP_END.
 *
 * SUSPECTED HANG (spec section 6). Both compositors' comments record that
 * unterminated path data is what hangs the front end while every vg_lite_*
 * call keeps returning VG_LITE_SUCCESS, but that has never been PROBED: it was
 * inferred from an arena-overflow near-miss. This case is the probe.
 *
 * The arena is written directly rather than through vgc_finish_path(), whose
 * whole job is to append the END this case must omit. */
static int32_t s_unterm[] = {
    VLC_OP_MOVE, 24, 24,
    VLC_OP_LINE, 104, 24,
    VLC_OP_LINE, 104, 104,
    VLC_OP_LINE, 24, 104,
    VLC_OP_CLOSE,
    /* deliberately no VLC_OP_END */
};

static vg_lite_error_t run_unterminated(void)
{
    vg_lite_path_t p;
    memset(&p, 0, sizeof(p));
    vg_lite_init_path(&p, VG_LITE_S32, VG_LITE_HIGH,
                      (uint32_t)sizeof(s_unterm), s_unterm,
                      23.0f, 23.0f, 105.0f, 105.0f);
    vg_lite_matrix_t m;
    vg_lite_identity(&m);
    const vg_lite_error_t e = vg_lite_draw(&vgc_scratch, &p, VG_LITE_FILL_NON_ZERO,
                                           &m, VG_LITE_BLEND_NONE, VGC_FILL_COLOR);
    const vg_lite_error_t f = vg_lite_finish();
    return e != VG_LITE_SUCCESS ? e : f;
}

static vgc_verdict_t check_unterminated(char *d, size_t n)
{
    /* Reaching here at all is the finding: the call RETURNED. Whether it drew
     * the square is secondary and is recorded rather than judged. */
    const int centre = vgc_is_filled(vgc_px(64, 64));
    snprintf(d, n, "returned=1,centre=%d", centre);
    return centre ? VGC_OK : VGC_BROKEN;
}

const vgc_case_t vgc_dangerous_cases[] = {
    { "path/unterminated", run_unterminated, check_unterminated, NULL },
};
const size_t vgc_dangerous_case_count =
    sizeof(vgc_dangerous_cases) / sizeof(vgc_dangerous_cases[0]);

#else

/* A zero-length array is not standard C++, and a NULL table with a zero count
 * would make the run loop's bounds depend on a pointer nobody dereferences.
 * One never-iterated entry keeps both honest. */
const vgc_case_t vgc_dangerous_cases[] = { { NULL, NULL, NULL, NULL } };
const size_t vgc_dangerous_case_count = 0;

#endif /* VGC_DANGEROUS */
