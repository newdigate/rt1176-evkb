/* vglite_probe - does the GC355 initialise and render on this board?
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Phase 1 of the VGLite bring-up: prove the GPU works INDEPENDENT of LVGL.
 * Everything else in the phase is scaffolding for this question.
 *
 * ONE BINARY, TWO OUTCOMES -- both are reported, neither crashes:
 *   QEMU     has no GC355 model, so vg_lite_init() fails and this prints
 *            VGLITE_INIT=ABSENT. That is the gated path, and it also proves
 *            the bare-metal port's waits are BOUNDED: an unbounded wait would
 *            hang here instead of returning.
 *   Silicon  the GPU must initialise, report its feature bits, fill one path
 *            and leave a stable checksum. See transcript_hw_evkb.txt.
 *
 * NOTE: uses Serial1 (the LPUART console run_qemu.sh captures via
 * `-serial file:`), not Serial (native USB CDC), like every sibling gate.
 */
#include <Arduino.h>
#include <string.h>
#include "Display.h"
#include "vg_lite.h"
#include "vg_lite_kernel.h"   /* vg_lite_kernel_mem_t */
#include "vg_lite_hal.h"      /* vg_lite_hal_query_mem */
#include "vg_lite_platform.h"

/* Contiguous pool for VGLite's command and tessellation buffers.
 *
 * ★ EXTMEM, not DMAMEM. DMAMEM lands in `.bss.dma` -> OCRAM, which is only
 * 512K on this part (imxrt1176.ld:10) and already spoken for -- a 2 MB pool
 * there overflows the region by ~1.5 MB at link time. EXTMEM places it in the
 * board's 64 MB SDRAM on SEMC at 0x80000000 (imxrt1176.h:1687,
 * imxrt1176.ld:5), which the GPU reaches as a bus master exactly as it reaches
 * the panel framebuffer.
 *
 * Phase 2 note: SDRAM is slower than OCRAM, so if command-buffer fetch ever
 * shows up as a bottleneck, the split worth trying is a small OCRAM command
 * buffer with the tessellation buffer left here. Measure before moving it. */
#define VGLITE_POOL_BYTES (2u * 1024u * 1024u)
EXTMEM __attribute__((aligned(64))) static uint8_t vglite_pool[VGLITE_POOL_BYTES];

/* Tessellation buffer geometry. VGLite rasterises paths in tiles of this size,
 * so it is a memory/throughput tradeoff rather than a limit on path size. */
#define TESS_W 256
#define TESS_H 256

/* The square this probe fills, in panel pixels. Fixed, so the checksum is a
 * golden rather than a moving target. */
#define SQ_MIN 100.0f
#define SQ_MAX 400.0f

static void report_features(void)
{
    /* ALL NINE bits this driver header defines -- the complete enum, so the
     * transcript is a full capability record rather than a sample.
     *
     * ★ PHASE 2 GAP, discovered here: LVGL 9.4's VG_LITE backend queries
     * gcFEATURE_BIT_VG_SCISSOR (lv_draw_vg_lite.c:141, lv_draw_vg_lite_vector.c:300)
     * and gcFEATURE_BIT_VG_IM_REPEAT_REFLECT / VG_24BIT / VG_INDEX_ENDIAN
     * (lv_vg_lite_grad.c, lv_vg_lite_utils.c) -- and NONE of those exist in
     * this driver's enum (VGLITE_HEADER_VERSION 6). Wiring LVGL's backend to
     * this driver will not compile until that is reconciled, by a newer NXP
     * driver or by shims. Phase 2 must settle it FIRST. */
    static const struct { vg_lite_feature_t bit; const char *name; } feats[] = {
        { gcFEATURE_BIT_VG_IM_INDEX_FORMAT,     "IM_INDEX_FORMAT"     },
        { gcFEATURE_BIT_VG_PE_PREMULTIPLY,      "PE_PREMULTIPLY"      },
        { gcFEATURE_BIT_VG_BORDER_CULLING,      "BORDER_CULLING"      },
        { gcFEATURE_BIT_VG_RGBA2_FORMAT,        "RGBA2_FORMAT"        },
        { gcFEATURE_BIT_VG_QUALITY_8X,          "QUALITY_8X"          },
        { gcFEATURE_BIT_VG_RADIAL_GRADIENT,     "RADIAL_GRADIENT"     },
        { gcFEATURE_BIT_VG_LINEAR_GRADIENT_EXT, "LINEAR_GRADIENT_EXT" },
        { gcFEATURE_BIT_VG_COLOR_KEY,           "COLOR_KEY"           },
        { gcFEATURE_BIT_VG_DITHER,              "DITHER"              },
    };
    for (unsigned i = 0; i < sizeof(feats) / sizeof(feats[0]); i++) {
        Serial1.printf("VGLITE_FEATURE %s=%lu\n", feats[i].name,
                       (unsigned long)vg_lite_query_feature(feats[i].bit));
    }
}

/* FNV-1a over the whole framebuffer -- the same arithmetic the LVGL port's
 * oracle uses, so the numbers are comparable in kind. */
static uint32_t fb_sum(void)
{
    uint32_t sum = 2166136261u;
    const uint8_t *p = (const uint8_t *)Display.framebuffer();
    const size_t n = (size_t)Display.width() * (size_t)Display.height()
                     * (size_t)PANEL_BYTES_PER_PIXEL;
    for (size_t i = 0; i < n; i++) { sum ^= p[i]; sum *= 16777619u; }
    return sum;
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("VGLITE_PROBE_BEGIN");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) {
        /* Safe-but-only-just: nothing below has run, so loop() -- which is
         * empty here anyway -- touches no GPU state. */
        Serial1.println("VGLITE_PROBE_DONE");
        return;
    }
    Display.fillScreen(0x0000);

    /* Hand the driver its register window and pool. The base is passed in
     * rather than assumed by the HAL: upstream's own defaults are an RT500
     * address and an FPGA address, neither of which is this part. */
    vg_lite_init_mem(VGLITE_RT1176_REGISTER_BASE, 0u,
                     vglite_pool, VGLITE_POOL_BYTES);

    /* ★ ASK BEFORE COMMITTING. vg_lite_init() assumes the GPU exists and
     * SPINS when it does not -- measured on QEMU, which has no GC355 model:
     * the probe reached PANEL_OK and then went silent, no fault logged. The
     * chip-ID read is what makes the absent case deterministic instead of a
     * hang, and it is why one binary can serve both paths. */
    const uint32_t chip_id = vg_lite_hal_probe_chip_id();
    Serial1.printf("VGLITE_CHIP_ID=0x%08lX\n", (unsigned long)chip_id);
    if (chip_id == 0u) {
        Serial1.println("VGLITE_INIT=ABSENT err=0 reason=no_chip_id");
        Serial1.printf("VGLITE_TIMEOUTS=%lu\n",
                       (unsigned long)vg_lite_os_wait_timeouts());
        Serial1.println("VGLITE_PROBE_DONE");
        return;
    }

    const vg_lite_error_t err = vg_lite_init(TESS_W, TESS_H);
    if (err != VG_LITE_SUCCESS) {
        Serial1.printf("VGLITE_INIT=ABSENT err=%d\n", (int)err);
        Serial1.printf("VGLITE_TIMEOUTS=%lu\n",
                       (unsigned long)vg_lite_os_wait_timeouts());
        Serial1.println("VGLITE_PROBE_DONE");
        return;
    }
    Serial1.println("VGLITE_INIT=OK");
    report_features();

    /* Target the panel's live framebuffer directly -- no intermediate buffer,
     * so what the GPU writes is what the LCDIFv2 scans out. */
    vg_lite_buffer_t target;
    memset(&target, 0, sizeof(target));
    target.width   = Display.width();
    target.height  = Display.height();
    target.stride  = Display.width() * PANEL_BYTES_PER_PIXEL;
    target.tiled   = VG_LITE_LINEAR;
    target.format  = VG_LITE_BGRA8888;
    target.memory  = (void *)Display.framebuffer();
    target.address = (uint32_t)(uintptr_t)Display.framebuffer();

    /* ★ REGISTER the framebuffer with the driver before drawing into it.
     *
     * The GPU will not touch memory the kernel does not know about. Without
     * this, vg_lite_draw() and vg_lite_finish() both return VG_LITE_SUCCESS,
     * the completion interrupt fires (TIMEOUTS=0) -- and NOT ONE PIXEL
     * changes: measured on silicon, where the framebuffer checksum came back
     * as exactly the all-zeros FNV (0x9BC99DC5 for 720*1280*4). Every status
     * the firmware could see said success.
     *
     * NXP's own examples sidestep this by allocating their render target with
     * vg_lite_allocate(); a target the application already owns -- like a
     * panel framebuffer -- has to be mapped instead. */
    const vg_lite_error_t merr = vg_lite_map(&target);
    Serial1.printf("VGLITE_MAP=%s err=%d\n",
                   merr == VG_LITE_SUCCESS ? "OK" : "FAIL", (int)merr);

    /* Diagnostics: what the driver actually believes about the target. If the
     * GPU address or stride is not what the panel scans out, the GPU can write
     * perfectly and change nothing visible. */
    Serial1.printf("VGLITE_TGT fb=0x%08lX addr=0x%08lX w=%ld h=%ld stride=%ld fmt=%d\n",
                   (unsigned long)(uintptr_t)Display.framebuffer(),
                   (unsigned long)target.address,
                   (long)target.width, (long)target.height, (long)target.stride,
                   (int)target.format);

    /* ★ BISECT: clear is the simplest write the GPU can perform -- no path, no
     * matrix, no tessellation. If this changes the framebuffer, the target is
     * reachable and any later failure is in the path/draw. If it does NOT, the
     * target itself is wrong and the path was never the question. */
    const vg_lite_error_t cerr = vg_lite_clear(&target, NULL, 0xFF204060u);
    vg_lite_finish();
    Serial1.printf("VGLITE_CLEAR=%s err=%d sum=0x%08lX irq=%lu\n",
                   cerr == VG_LITE_SUCCESS ? "OK" : "FAIL", (int)cerr,
                   (unsigned long)fb_sum(),
                   (unsigned long)vg_lite_os_irq_count());

    /* ★ SECOND BISECT: can the GPU write ANYWHERE?
     *
     * The clear above proved the panel framebuffer does not change. This one
     * targets a buffer the DRIVER allocates from its own pool -- the path
     * NXP's examples use. If this one changes and the framebuffer does not,
     * the fault is specific to externally-owned memory. If neither changes,
     * the GPU cannot write at all and the fault is lower: clocks, the AXI
     * master path, or bus-master permissions (RDC). */
    vg_lite_buffer_t scratch;
    memset(&scratch, 0, sizeof(scratch));
    scratch.width  = 64;
    scratch.height = 64;
    scratch.format = VG_LITE_BGRA8888;
    const vg_lite_error_t aerr = vg_lite_allocate(&scratch);
    Serial1.printf("VGLITE_ALLOC=%s err=%d mem=0x%08lX addr=0x%08lX stride=%ld\n",
                   aerr == VG_LITE_SUCCESS ? "OK" : "FAIL", (int)aerr,
                   (unsigned long)(uintptr_t)scratch.memory,
                   (unsigned long)scratch.address, (long)scratch.stride);
    if (aerr == VG_LITE_SUCCESS) {
        volatile uint32_t *px = (volatile uint32_t *)scratch.memory;
        px[0] = 0xDEADBEEFu;            /* poison, so "unchanged" is visible */
        const vg_lite_error_t serr = vg_lite_clear(&scratch, NULL, 0xFF204060u);
        vg_lite_finish();
        Serial1.printf("VGLITE_SCRATCH=%s err=%d px0=0x%08lX px1=0x%08lX irq=%lu\n",
                       serr == VG_LITE_SUCCESS ? "OK" : "FAIL", (int)serr,
                       (unsigned long)px[0], (unsigned long)px[1],
                       (unsigned long)vg_lite_os_irq_count());
    }

    /* One closed square. Opcodes are VGLite's path VLC encoding:
     * 2 = MOVE_TO, 4 = LINE_TO, 0 = END. */
    static int32_t path_data[] = {
        2, (int32_t)SQ_MIN, (int32_t)SQ_MIN,
        4, (int32_t)SQ_MAX, (int32_t)SQ_MIN,
        4, (int32_t)SQ_MAX, (int32_t)SQ_MAX,
        4, (int32_t)SQ_MIN, (int32_t)SQ_MAX,
        0,
    };
    vg_lite_path_t path;
    memset(&path, 0, sizeof(path));
    vg_lite_init_path(&path, VG_LITE_S32, VG_LITE_HIGH,
                      (uint32_t)sizeof(path_data), path_data,
                      SQ_MIN, SQ_MIN, SQ_MAX, SQ_MAX);

    vg_lite_matrix_t matrix;
    vg_lite_identity(&matrix);

    const vg_lite_error_t derr =
        vg_lite_draw(&target, &path, VG_LITE_FILL_EVEN_ODD, &matrix,
                     VG_LITE_BLEND_NONE, 0xFF3399FFu);
    Serial1.printf("VGLITE_DRAW=%s err=%d\n",
                   derr == VG_LITE_SUCCESS ? "OK" : "FAIL", (int)derr);

    /* Block until the GPU has actually retired the work -- checksumming before
     * this would race the hardware and produce a value that is stable only by
     * luck. */
    const vg_lite_error_t ferr = vg_lite_finish();
    Serial1.printf("VGLITE_FINISH=%s err=%d\n",
                   ferr == VG_LITE_SUCCESS ? "OK" : "FAIL", (int)ferr);

    /* ★ Load-bearing: a non-zero count means a bounded wait gave up, so the
     * completion path is wrong even if pixels appeared. */
    Serial1.printf("VGLITE_TIMEOUTS=%lu VGLITE_IRQS=%lu\n",
                   (unsigned long)vg_lite_os_wait_timeouts(),
                   (unsigned long)vg_lite_os_irq_count());

    vg_lite_kernel_mem_t mem;
    memset(&mem, 0, sizeof(mem));
    if (vg_lite_hal_query_mem(&mem) == VG_LITE_SUCCESS) {
        /* Phase 2 sizes the pool and tessellation buffer from this. */
        Serial1.printf("VGLITE_POOL_FREE=%lu\n", (unsigned long)mem.bytes);
    }

    Serial1.printf("VGLITE_SUM=0x%08lX\n", (unsigned long)fb_sum());
    Serial1.println("VGLITE_PROBE_DONE");
}

void loop() { }
