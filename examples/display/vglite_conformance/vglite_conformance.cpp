/* vglite_conformance - which vg_lite features behave correctly on this GC355?
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec: docs/superpowers/specs/2026-08-30-gc355-conformance-design.md
 * Reference doc this feeds: docs/gc355-vglite-quirks.md
 *
 * Sibling to display/vglite_probe, which answers the PRIOR question (does the
 * GPU initialise and render at all). This one asks, feature by feature,
 * whether what it renders is RIGHT -- because every GC355 defect this tree has
 * hit reported success while producing the wrong picture.
 *
 * ONE BINARY, TWO OUTCOMES, both reported, neither hangs:
 *   QEMU     has no GC355, so the chip-ID probe reads 0, the run prints
 *            vgc_engine=absent and every case reports pixel=skip. That is the
 *            gated path (run_qemu.sh), and its tripwire is that NO case may
 *            report ok with no GPU present.
 *   Silicon  the whole matrix in ONE boot -- see transcript_hw_evkb.txt and
 *            expected_silicon.txt.
 *
 * NO PANEL. The core brings up the SEMC SDRAM in startup before setup(), so
 * EXTMEM is live without Display.begin(); nothing here scans out, flips or
 * touches LVGL. Prints to Serial1 (LPUART1, the console every gate captures).
 */
#include <Arduino.h>
#include <stdio.h>    /* snprintf -- arrives transitively via Print.h, but the
                       * api= field's bound depends on it, so name it here */
#include <string.h>
#include "vgc_harness.h"
#include "vgc_predicates.h"

extern "C" {
#include "vg_lite_platform.h"
}

/* Contiguous pool for VGLite's command and tessellation buffers. EXTMEM, not
 * DMAMEM: OCRAM is 512K on this part and already spoken for -- a 2 MB pool
 * there overflows the region at link time. (Same reasoning as vglite_probe.) */
#define VGC_POOL_BYTES (2u * 1024u * 1024u)
EXTMEM __attribute__((aligned(64))) static uint8_t vgc_pool[VGC_POOL_BYTES];

/* The scratch render target. 64-byte aligned: Vivante wants 64-byte-aligned
 * buffers and a misaligned one does not fail, it hangs the front end while
 * every API call returns VG_LITE_SUCCESS. */
EXTMEM __attribute__((aligned(64)))
static uint8_t vgc_scratch_mem[VGC_W * VGC_H * 4];

vg_lite_buffer_t vgc_scratch;

static bool s_gpu = false;

/* ---- scratch access ------------------------------------------------------- */

/* The ONE path to rendered pixels. Deliberately the static array and not
 * vgc_scratch.memory -- see the header for why a second path is a hazard
 * rather than a convenience. */
const uint32_t *vgc_fb(void)
{
    return (const uint32_t *)(const void *)vgc_scratch_mem;
}

static uint32_t s_px_oob;

uint32_t vgc_px_oob(void)      { return s_px_oob; }
void     vgc_px_oob_reset(void) { s_px_oob = 0; }

uint32_t vgc_px(int x, int y)
{
    if (x < 0 || x >= VGC_W || y < 0 || y >= VGC_H) { s_px_oob++; return 0u; }
    return vgc_fb()[(size_t)y * VGC_W + (size_t)x];
}

uint32_t vgc_scratch_sum(void)
{
    return vgc_fnv(vgc_fb(), (size_t)VGC_W * VGC_H * 4);
}

/* ONE clear implementation: vgc_clear() is vgc_clear_to(VGC_BG_COLOR). Two
 * copies would be two chances to diverge on the finish, which is the half a
 * caller cannot see. */
vg_lite_error_t vgc_clear_to(uint32_t abgr)
{
    const vg_lite_error_t e = vg_lite_clear(&vgc_scratch, NULL, abgr);
    if (e != VG_LITE_SUCCESS) return e;
    return vg_lite_finish();
}

vg_lite_error_t vgc_clear(void) { return vgc_clear_to(VGC_BG_COLOR); }

/* ---- per-case helpers ------------------------------------------------------ */

vg_lite_matrix_t *vgc_ident(void)
{
    static vg_lite_matrix_t m;
    vg_lite_identity(&m);
    return &m;
}

void vgc_draw_path_blend(vg_lite_path_t *p, vg_lite_fill_t rule, uint32_t color,
                         vg_lite_blend_t blend, vg_lite_error_t *acc)
{
    const vg_lite_error_t e = vg_lite_draw(&vgc_scratch, p, rule, vgc_ident(),
                                           blend, color);
    if (e != VG_LITE_SUCCESS && *acc == VG_LITE_SUCCESS) *acc = e;
}

/* Signature UNCHANGED -- every path case draws through it and their answers
 * are measured on silicon (a count would go stale as cases are added; the
 * property that matters is that none of them had to change). It delegates so
 * the status-accumulation contract the case tables are specified in terms of
 * has ONE implementation. */
void vgc_draw_path(vg_lite_path_t *p, vg_lite_fill_t rule, uint32_t color,
                   vg_lite_error_t *acc)
{
    vgc_draw_path_blend(p, rule, color, VG_LITE_BLEND_NONE, acc);
}

void vgc_finish_into(vg_lite_error_t *acc)
{
    const vg_lite_error_t e = vg_lite_finish();
    if (e != VG_LITE_SUCCESS && *acc == VG_LITE_SUCCESS) *acc = e;
}

/* ---- gradient draws ---------------------------------------------------------
 * paint_color is 0, as in NXP's own linear-gradient example. The driver
 * swizzles it and pushes it to paint register 0x0A26 (vg_lite_path.c, the
 * draw_linear_grad body) whatever the paint type; if it MODULATED a linear
 * paint, NXP's demo would render black, so 0 is the documented-by-usage value
 * rather than a guess. VG_LITE_FILTER_LINEAR is what vg_lite_draw_grad
 * hard-codes for the legacy path (vg_lite_path.c:5739), used here for both so
 * the two APIs differ in nothing but the API. */
void vgc_draw_linear_grad(vg_lite_path_t *p, vg_lite_ext_linear_gradient_t *g,
                          vg_lite_matrix_t *path_matrix, vg_lite_error_t *acc)
{
    const vg_lite_error_t e = vg_lite_draw_linear_grad(&vgc_scratch, p,
                                  VG_LITE_FILL_NON_ZERO, path_matrix, g, 0,
                                  VG_LITE_BLEND_NONE, VG_LITE_FILTER_LINEAR);
    if (e != VG_LITE_SUCCESS && *acc == VG_LITE_SUCCESS) *acc = e;
}

void vgc_draw_grad(vg_lite_path_t *p, vg_lite_linear_gradient_t *g,
                   vg_lite_matrix_t *path_matrix, vg_lite_error_t *acc)
{
    const vg_lite_error_t e = vg_lite_draw_grad(&vgc_scratch, p,
                                  VG_LITE_FILL_NON_ZERO, path_matrix, g,
                                  VG_LITE_BLEND_NONE);
    if (e != VG_LITE_SUCCESS && *acc == VG_LITE_SUCCESS) *acc = e;
}

/* The path arena lives in vgc_arena.cpp -- its own TU so the host test
 * (tests/arena_test.cpp) can compile it against a stubbed vg_lite_init_path.
 * See that file's header for why it is the piece most worth testing. */

/* ---- the run loop --------------------------------------------------------- */

static uint32_t s_ok, s_broken, s_skip, s_differs, s_cases;

static const char *verdict_name(vgc_verdict_t v)
{
    switch (v) {
    case VGC_OK:     return "ok";
    case VGC_BROKEN: return "broken";
    default:         return "skip";
    }
}

/* "success" or "error:N", into a caller-owned buffer. */
static void api_name(vg_lite_error_t e, char *out, size_t n)
{
    if (e == VG_LITE_SUCCESS) snprintf(out, n, "success");
    else                      snprintf(out, n, "error:%d", (int)e);
}

/* ★ THE CASE LINE IS EMITTED IN THREE printf CALLS, ONE OUTPUT LINE.
 *
 * This core's Print::printf formats into a 128-byte stack buffer and TRUNCATES
 * anything longer (cores/imxrt1176/Print.cpp:109 PRINTF_BUF_SIZE) -- silently,
 * with no error and no newline. A single-call case line can exceed that:
 * "vgc case=" (9) + a slug (~32) + " api=success" (12) + " api2=success" (13)
 * + " pixel=broken" (13) + " detail=" (8) + a detail up to VGC_DETAIL_MAX-1
 * (95) + " repeat=differs" (15) + newline = ~198 bytes worst case. The
 * truncation would eat repeat= -- which the gate asserts -- AND the newline,
 * welding two case lines together, so the transcript would read as a firmware
 * fault rather than an instrument limit. The detail is the field that forces
 * the split: " detail=" plus a full detail is 103 bytes on its own, so it gets
 * a call to itself and the other two are ~99 and ~16.
 *
 * Nothing else writes to Serial1 between them, so the three fragments are
 * contiguous on the wire and a reader still sees exactly one line. */
static void print_case_line(const char *id, const char *api, const char *api2,
                            const char *pixel, const char *detail,
                            const char *repeat)
{
    Serial1.printf("vgc case=%s api=%s api2=%s pixel=%s", id, api, api2, pixel);
    Serial1.printf(" detail=%s", detail);
    Serial1.printf(" repeat=%s\n", repeat);
}

/* The detail field is written by case authors and parsed by field in Task 7's
 * checker, so a stray space would split one field into two and misalign every
 * field after it. The contract says "no spaces"; this ENFORCES it rather than
 * trusting it, and gives an empty detail a spelling of its own so a case line
 * can never read "detail= repeat=same". */
static void sanitise_detail(char *d, size_t n)
{
    for (size_t i = 0; i < n && d[i]; i++) if (d[i] == ' ') d[i] = '_';
    if (d[0] == '\0' && n >= 5) { d[0]='n'; d[1]='o'; d[2]='n'; d[3]='e'; d[4]='\0'; }
}

static void run_case(const vgc_case_t *c)
{
    /* ★ PRINT THE ID BEFORE ISSUING ANY CALL (spec section 6). Two known
     * traps HANG the front end rather than failing, and a hang costs the whole
     * matrix and a bench cycle. A case_begin line with no matching case line
     * names the culprit in the transcript. */
    const char *const id = c->id ? c->id : "<null>";
    Serial1.printf("vgc case_begin=%s\n", id);
    s_cases++;

    /* ★ A MALFORMED TABLE ENTRY MUST BE A TRANSCRIPT LINE, NOT A HARD FAULT.
     * Both case files ship a {NULL,NULL,NULL,NULL} sentinel whose safety rests
     * on a SEPARATELY declared count -- and vgc_dangerous.cpp's OFF branch
     * mismatches them deliberately (one entry, count 0). If a count ever
     * drifts past its table, calling c->run() through NULL faults the board and
     * the transcript ends mid-matrix with nothing naming the cause. Two lines
     * turn that into a diagnosable skip. */
    if (!c->id || !c->run || !c->check) {
        s_skip++;
        print_case_line(id, "skip", "skip", "skip", "null_table_entry", "skip");
        return;
    }

    if (!s_gpu) {
        s_skip++;
        print_case_line(id, "skip", "skip", "skip", "engine=absent", "skip");
        return;
    }

    char detail[VGC_DETAIL_MAX];
    detail[0] = '\0';

    /* ★ A FAILED CLEAR INVALIDATES THE CASE, so it is checked rather than
     * discarded. Rendering over a buffer still holding the previous case's
     * pixels does not fail -- it produces a pixel= verdict computed from
     * contamination and prints it beside api=success, which is the single most
     * misleading line this instrument could emit. */
    vgc_arena_reset();
    vgc_px_oob_reset();
    vg_lite_error_t cerr = vgc_clear();
    if (cerr != VG_LITE_SUCCESS) {
        s_skip++;
        snprintf(detail, sizeof(detail), "clear_failed:%d", (int)cerr);
        print_case_line(id, "skip", "skip", "skip", detail, "skip");
        return;
    }

    const vg_lite_error_t api = c->run();
    const uint32_t sum1 = c->sum ? c->sum() : vgc_scratch_sum();
    const vgc_verdict_t v = c->check(detail, sizeof(detail));

    /* ★ A check() THAT READ OUT OF RANGE HAS NO VERDICT TO GIVE. vgc_px()
     * counts rather than answers (see the header), and this is where the count
     * is spent: an instrument bug is reported as a named skip, never as an ok
     * or a broken. Same contract as vgc_count_runs_col's -1. */
    const uint32_t oob = vgc_px_oob();
    if (oob != 0u) {
        s_skip++;
        snprintf(detail, sizeof(detail), "px_oob:%lu", (unsigned long)oob);
        char af[16]; api_name(api, af, sizeof(af));
        print_case_line(id, af, "skip", "skip", detail, "skip");
        return;
    }

    /* Second identical run. Per-boot and per-run nondeterminism is a
     * first-class GC355 symptom (7 boots, 7 checksums on the fader), so a
     * case that renders differently on an identical re-run is a finding in
     * its own right -- independent of whether the first render was correct,
     * which is why repeat= is its own field and does not fold into pixel=.
     *
     * ★ THE SECOND RUN'S STATUS IS COMPARED TOO, in its own api2= field. A
     * status that FLIPS between two identical runs is exactly the finding this
     * repeat exists to catch, and comparing only pixels would miss it for one
     * comparison's worth of work. api2= is its own field rather than folded
     * into repeat= so that each field keeps a single meaning: repeat= is about
     * PIXELS, and it is what the summary's repeat_differs counts. */
    vgc_arena_reset();
    cerr = vgc_clear();
    if (cerr != VG_LITE_SUCCESS) {
        s_skip++;
        snprintf(detail, sizeof(detail), "clear2_failed:%d", (int)cerr);
        char af[16]; api_name(api, af, sizeof(af));
        print_case_line(id, af, "skip", "skip", detail, "skip");
        return;
    }
    const vg_lite_error_t api2 = c->run();
    const uint32_t sum2 = c->sum ? c->sum() : vgc_scratch_sum();
    const bool same = (sum1 == sum2);
    if (!same) s_differs++;

    if (v == VGC_OK)          s_ok++;
    else if (v == VGC_BROKEN) s_broken++;
    else                      s_skip++;

    char af[16];  api_name(api, af, sizeof(af));
    char af2[16];
    if (api2 == api) snprintf(af2, sizeof(af2), "same");
    else             api_name(api2, af2, sizeof(af2));

    sanitise_detail(detail, sizeof(detail));
    print_case_line(id, af, af2, verdict_name(v), detail,
                    same ? "same" : "differs");
}

/* Which build produced this matrix. Hoisted out of the vgc_summary argument
 * list: a preprocessor conditional spanning a call's arguments is legal but
 * unreadable, and this is read from transcripts. */
#ifdef VGC_DANGEROUS
static const char *const s_dangerous = "on";
#else
static const char *const s_dangerous = "off";
#endif

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("VGC_BEGIN");

    vg_lite_init_mem(VGLITE_RT1176_REGISTER_BASE, 0u, vgc_pool, VGC_POOL_BYTES);

    /* ★ ASK BEFORE COMMITTING. vg_lite_init() assumes the GPU exists and
     * SPINS when it does not (measured on QEMU). The chip-ID read is what
     * makes the absent case deterministic instead of a hang, and it is why one
     * binary serves both paths. */
    const uint32_t chip_id = vg_lite_hal_probe_chip_id();
    Serial1.printf("vgc_chip_id=0x%08lX\n", (unsigned long)chip_id);

    const char *absent_reason = NULL;
    if (chip_id == 0u) {
        absent_reason = "no_chip_id";
    } else {
        const vg_lite_error_t err = vg_lite_init(VGC_TESS_W, VGC_TESS_H);
        if (err != VG_LITE_SUCCESS) {
            Serial1.printf("vgc_init_err=%d\n", (int)err);
            absent_reason = "init_failed";
        } else {
            memset(&vgc_scratch, 0, sizeof(vgc_scratch));
            vgc_scratch.width   = VGC_W;
            vgc_scratch.height  = VGC_H;
            vgc_scratch.stride  = VGC_W * 4;
            vgc_scratch.tiled   = VG_LITE_LINEAR;
            vgc_scratch.format  = VG_LITE_BGRA8888;
            vgc_scratch.memory  = (void *)vgc_scratch_mem;
            vgc_scratch.address = (uint32_t)(uintptr_t)vgc_scratch_mem;
            /* ★ The GPU will not touch memory the kernel does not know about.
             * Without the map every draw returns SUCCESS and not one pixel
             * changes (vglite_probe, measured on silicon). */
            const vg_lite_error_t merr =
                vg_lite_map(&vgc_scratch, VG_LITE_MAP_USER_MEMORY, 0);
            if (merr != VG_LITE_SUCCESS) {
                Serial1.printf("vgc_map_err=%d\n", (int)merr);
                absent_reason = "map_failed";
            } else {
                s_gpu = true;
            }
        }
    }

    if (s_gpu) {
        Serial1.printf("vgc_engine=gpu target=%dx%d fmt=%d tess=%dx%d\n",
                       VGC_W, VGC_H, (int)VG_LITE_BGRA8888,
                       VGC_TESS_W, VGC_TESS_H);
    } else {
        Serial1.printf("vgc_engine=absent reason=%s\n", absent_reason);
    }

    for (size_t i = 0; i < vgc_path_case_count; i++)      run_case(&vgc_path_cases[i]);
    for (size_t i = 0; i < vgc_color_case_count; i++)     run_case(&vgc_color_cases[i]);
    for (size_t i = 0; i < vgc_grad_case_count; i++)      run_case(&vgc_grad_cases[i]);
    for (size_t i = 0; i < vgc_dangerous_case_count; i++) run_case(&vgc_dangerous_cases[i]);

    /* The spec's summary line, with repeat_differs appended (the spec's field
     * list is an exact PREFIX of this one -- an additive extension, because
     * the repeat count has to be assertable by the gate).
     *
     * Worst realistic length is ~91 bytes (counts are bounded by the case
     * table, which is tens of entries), so this one fits the 128-byte printf
     * buffer in a single call. */
    Serial1.printf("vgc_summary engine=%s cases=%lu ok=%lu broken=%lu skip=%lu "
                   "dangerous=%s repeat_differs=%lu\n",
                   s_gpu ? "gpu" : "absent",
                   (unsigned long)s_cases, (unsigned long)s_ok,
                   (unsigned long)s_broken, (unsigned long)s_skip,
                   s_dangerous,
                   (unsigned long)s_differs);

    /* A non-zero count means a bounded wait gave up, so the completion path
     * is wrong even where the pixels look right. */
    Serial1.printf("vgc_timeouts=%lu vgc_irqs=%lu\n",
                   (unsigned long)vg_lite_os_wait_timeouts(),
                   (unsigned long)vg_lite_os_irq_count());
    Serial1.println("VGC_DONE");
}

/* Heartbeat so a bench operator can tell a finished matrix from a hung one.
 * Touches no GPU state. */
void loop()
{
    static uint32_t last = 0;
    if (millis() - last >= 5000u) {
        last = millis();
        Serial1.printf("vgc_hb t=%lu\n", (unsigned long)last);
    }
}
