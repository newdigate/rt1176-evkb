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

/* ---- harness services ----------------------------------------------------- */

uint32_t vgc_px(int x, int y)
{
    const uint32_t *row = (const uint32_t *)(const void *)
        ((const uint8_t *)vgc_scratch_mem + (size_t)y * VGC_W * 4);
    return row[x];
}

uint32_t vgc_scratch_sum(void)
{
    return vgc_fnv(vgc_scratch_mem, (size_t)VGC_W * VGC_H * 4);
}

vg_lite_error_t vgc_clear(void)
{
    const vg_lite_error_t e = vg_lite_clear(&vgc_scratch, NULL, VGC_BG_COLOR);
    if (e != VG_LITE_SUCCESS) return e;
    return vg_lite_finish();
}

/* ---- path arena ----------------------------------------------------------- */

static int32_t s_arena[VGC_ARENA_WORDS];
static size_t  s_used;
static size_t  s_start;
static bool    s_overflow;

void vgc_arena_reset(void) { s_used = 0; s_start = 0; s_overflow = false; }

void vgc_emit(int32_t w)
{
    if (s_used < VGC_ARENA_WORDS) s_arena[s_used++] = w;
    else s_overflow = true;
}

bool vgc_finish_path(vg_lite_path_t *p, float x0, float y0, float x1, float y1)
{
    vgc_emit(VLC_OP_END);
    if (s_overflow) { s_overflow = false; s_start = s_used; return false; }
    memset(p, 0, sizeof(*p));
    vg_lite_init_path(p, VG_LITE_S32, VG_LITE_HIGH,
                      (uint32_t)((s_used - s_start) * sizeof(int32_t)),
                      &s_arena[s_start],
                      x0 - 1.0f, y0 - 1.0f, x1 + 1.0f, y1 + 1.0f);
    s_start = s_used;
    return true;
}

void vgc_emit_rect_cw(float x, float y, float w, float h)
{
    vgc_emit(VLC_OP_MOVE); vgc_emit((int32_t)x);       vgc_emit((int32_t)y);
    vgc_emit(VLC_OP_LINE); vgc_emit((int32_t)(x + w)); vgc_emit((int32_t)y);
    vgc_emit(VLC_OP_LINE); vgc_emit((int32_t)(x + w)); vgc_emit((int32_t)(y + h));
    vgc_emit(VLC_OP_LINE); vgc_emit((int32_t)x);       vgc_emit((int32_t)(y + h));
    vgc_emit(VLC_OP_CLOSE);
}

void vgc_emit_rect_ccw(float x, float y, float w, float h)
{
    vgc_emit(VLC_OP_MOVE); vgc_emit((int32_t)x);       vgc_emit((int32_t)y);
    vgc_emit(VLC_OP_LINE); vgc_emit((int32_t)x);       vgc_emit((int32_t)(y + h));
    vgc_emit(VLC_OP_LINE); vgc_emit((int32_t)(x + w)); vgc_emit((int32_t)(y + h));
    vgc_emit(VLC_OP_LINE); vgc_emit((int32_t)(x + w)); vgc_emit((int32_t)y);
    vgc_emit(VLC_OP_CLOSE);
}

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

/* ★ THE CASE LINE IS EMITTED IN THREE printf CALLS, ONE OUTPUT LINE.
 *
 * This core's Print::printf formats into a 128-byte stack buffer and TRUNCATES
 * anything longer (cores/imxrt1176/Print.cpp:109 PRINTF_BUF_SIZE) -- silently,
 * with no error and no newline. A single-call case line can exceed that:
 * "vgc case=" (9) + a slug (~32) + " api=success" (12) + " pixel=broken" (13)
 * + " detail=" (8) + a detail up to VGC_DETAIL_MAX-1 (95) + " repeat=differs"
 * (15) + newline = ~185 bytes worst case. The truncation would eat repeat=
 * -- which the gate asserts -- AND the newline, welding two case lines
 * together, so the transcript would read as a firmware fault rather than an
 * instrument limit. Split, each call's worst case is ~103 bytes.
 *
 * Nothing else writes to Serial1 between them, so the three fragments are
 * contiguous on the wire and a reader still sees exactly one line. */
static void print_case_line(const char *id, const char *api,
                            const char *pixel, const char *detail,
                            const char *repeat)
{
    Serial1.printf("vgc case=%s api=%s pixel=%s", id, api, pixel);
    Serial1.printf(" detail=%s", detail);
    Serial1.printf(" repeat=%s\n", repeat);
}

static void run_case(const vgc_case_t *c)
{
    /* ★ PRINT THE ID BEFORE ISSUING ANY CALL (spec section 6). Two known
     * traps HANG the front end rather than failing, and a hang costs the whole
     * matrix and a bench cycle. A case_begin line with no matching case line
     * names the culprit in the transcript. */
    Serial1.printf("vgc case_begin=%s\n", c->id);

    s_cases++;
    if (!s_gpu) {
        s_skip++;
        print_case_line(c->id, "skip", "skip", "engine=absent", "skip");
        return;
    }

    char detail[VGC_DETAIL_MAX];
    detail[0] = '\0';

    vgc_arena_reset();
    vgc_clear();
    const vg_lite_error_t api = c->run();
    const uint32_t sum1 = c->sum ? c->sum() : vgc_scratch_sum();
    const vgc_verdict_t v = c->check(detail, sizeof(detail));

    /* Second identical run. Per-boot and per-run nondeterminism is a
     * first-class GC355 symptom (7 boots, 7 checksums on the fader), so a
     * case that renders differently on an identical re-run is a finding in
     * its own right -- independent of whether the first render was correct,
     * which is why repeat= is its own field and does not fold into pixel=. */
    vgc_arena_reset();
    vgc_clear();
    (void)c->run();
    const uint32_t sum2 = c->sum ? c->sum() : vgc_scratch_sum();
    const bool same = (sum1 == sum2);
    if (!same) s_differs++;

    if (v == VGC_OK)          s_ok++;
    else if (v == VGC_BROKEN) s_broken++;
    else                      s_skip++;

    char api_field[16];
    if (api == VG_LITE_SUCCESS) strcpy(api_field, "success");
    else snprintf(api_field, sizeof(api_field), "error:%d", (int)api);

    print_case_line(c->id, api_field, verdict_name(v), detail,
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
