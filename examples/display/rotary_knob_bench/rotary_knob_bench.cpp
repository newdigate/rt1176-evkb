/* rotary_knob_bench - RotaryKnob render-strategy bench (12 cells).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec: docs/superpowers/specs/2026-08-27-rotary-knob-bench-design.md
 *
 * ONE ELF, BOTH ENGINES. LVGL is ALWAYS the software renderer here
 * (LV_USE_DRAW_VG_LITE stays 0 -- see CMakeLists for why that is what makes a
 * single binary safe); the GC355 is reached only by direct vg_lite_* calls
 * guarded by the chip-ID probe. QEMU has no GC355, so there every gpu cell
 * reports st=gpu-absent -- asserted by the gate, never a silent skip.
 */
#include <Arduino.h>
#include <string.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "rk_geometry.h"

extern "C" {
#include "vg_lite.h"
#include "vg_lite_platform.h"
}

/* Same pool siting and reasoning as vglite_probe: EXTMEM (SDRAM), not DMAMEM
 * -- a 2 MB pool overflows the 512K OCRAM at link time, and the GPU reaches
 * SDRAM as a bus master exactly as it reaches the framebuffer. */
#define VGLITE_POOL_BYTES (2u * 1024u * 1024u)
EXTMEM __attribute__((aligned(64))) static uint8_t vglite_pool[VGLITE_POOL_BYTES];
#define TESS_W 256
#define TESS_H 256

static bool s_gpu = false;
static vg_lite_buffer_t s_target;     /* the panel framebuffer, mapped */

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("rotary_knob_bench up");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) { Serial1.println("rkb_fatal=panel"); return; }
    Display.fillScreen(0x0000);

    /* ★ ASK BEFORE COMMITTING (vglite_probe): vg_lite_init() SPINS on absent
     * hardware, so the chip-ID read is what makes the absent case a clean
     * negative instead of a hang. */
    vg_lite_init_mem(VGLITE_RT1176_REGISTER_BASE, 0u, vglite_pool, VGLITE_POOL_BYTES);
    const uint32_t chip_id = vg_lite_hal_probe_chip_id();
    if (chip_id != 0u && vg_lite_init(TESS_W, TESS_H) == VG_LITE_SUCCESS) {
        memset(&s_target, 0, sizeof(s_target));
        s_target.width   = Display.width();
        s_target.height  = Display.height();
        s_target.stride  = Display.width() * PANEL_BYTES_PER_PIXEL;
        s_target.tiled   = VG_LITE_LINEAR;
        s_target.format  = VG_LITE_BGRA8888;   /* = panel XRGB8888 memory order */
        s_target.memory  = (void *)Display.framebuffer();
        s_target.address = (uint32_t)(uintptr_t)Display.framebuffer();
        /* ★ REGISTER the framebuffer with the driver or every draw "succeeds"
         * and changes nothing (vglite_probe, measured on silicon). */
        s_gpu = (vg_lite_map(&s_target, VG_LITE_MAP_USER_MEMORY, 0) == VG_LITE_SUCCESS);
    }
    Serial1.printf("gpu=%s chip_id=0x%08lX\n", s_gpu ? "present" : "absent",
                   (unsigned long)chip_id);

    lvgl_rt1176_begin();
    lvgl_mipi_panel_create(Display);
    /* State machine armed in Task 4; nothing more yet. */
}

void loop()
{
    lvgl_rt1176_loop();
}
