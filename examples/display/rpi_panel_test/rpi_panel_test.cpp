/* rpi_panel_test - RT1176 -> RPi 7" MIPI-DSI display v1 gate
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * As of Task 10: ATTINY_OK + PLL_OK + LCDIFV2_OK + DSI_OK (the ATtiny88 driver,
 * the VIDEO_PLL / LCDIFv2+MIPI clock-root bring-up, the LCDIFv2 driver and the
 * MIPI-DSI host driver are all live), TC358762_FAIL and FB_SUM never appears --
 * begin() still returns false overall (later stages unimplemented). Later tasks
 * (12,13) turn each remaining stage green in turn as the real TC358762 bridge
 * and PXP-fill drivers land.
 *
 * NOTE: uses Serial1 (the LPUART console every sibling gate's run_qemu.sh
 * captures via `-serial file:`), not Serial (native USB CDC, which QEMU
 * would not capture here) -- see cores/imxrt1176/usb_serial.h.
 */
#include <Arduino.h>
#include <Wire.h>   // needed by the RPiDisplay ATtiny driver (Display::begin())
#include "Display.h"

static const uint16_t COLOR = 0xF800; // solid red (RGB565) -- arbitrary, checksum is computed

// FNV-1a over the framebuffer bytes
static uint32_t fnv1a(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

// Standard bit-replication RGB565->RGB888 expansion, shared with PXP fill +
// the bridge oracle. Unused until Task 13's fillScreen()/gate check reuse it
// verbatim -- kept (not deleted) per the plan, annotated to stay warning-clean.
static inline uint32_t rgb565_to_888(uint16_t c) __attribute__((unused));
static inline uint32_t rgb565_to_888(uint16_t c) {
    uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

void setup() {
    Serial1.begin(115200);
    delay(200);
    Serial1.println("RPI_PANEL_BEGIN");

    bool ok = Display.begin();
    // stage tokens -- emitted unconditionally so the first false pinpoints the broken layer
    Serial1.printf("ATTINY_%s\n",   Display.attinyOk() ? "OK" : "FAIL");
    Serial1.printf("PLL_%s\n",      Display.pllOk()    ? "OK" : "FAIL");
    Serial1.printf("LCDIFV2_%s\n",  Display.lcdifOk()  ? "OK" : "FAIL");
    Serial1.printf("DSI_%s\n",      Display.dsiOk()    ? "OK" : "FAIL");
    Serial1.printf("TC358762_%s\n", Display.bridgeOk() ? "OK" : "FAIL");

    // TEMP(Task 7 probe) -- remove in Task 11. Proves the QEMU LCDIFv2 model
    // reads the SDRAM layer-0 framebuffer per its CTRLDESCL registers, ahead of
    // any real LCDIFv2/DSI driver. We point layer 0 at an extmem buffer filled
    // with a positional RGB565 pattern, then read the model's debug scan-checksum
    // tap and compare it to the same FNV-1a computed here in software. The tap
    // lives at a spare offset (0x3FFC) outside the real register file; Task 11
    // moves the firmware-visible checksum to the TC358762 bridge (DSI region).
    {
        const uint32_t PW = 64, PH = 48;             // small test frame
        const uint32_t PPITCH = PW * 2;              // RGB565, tightly packed
        uint16_t *pbuf = (uint16_t *)extmem_malloc((size_t)PW * PH * 2);
        if (!pbuf) {
            Serial1.println("PROBE_LCDIF=SUM:0 EXP:0 FAIL(alloc)");
        } else {
            // positional pattern: consecutive pixels differ, so a stride/pitch
            // bug in the model would change the checksum
            for (uint32_t i = 0; i < PW * PH; i++) pbuf[i] = (uint16_t)(0x1000 + i);
            arm_dcache_flush_delete(pbuf, PW * PH * 2); // no-op here; HW coherency

            // layer-0 descriptor: WxH, pitch, buffer addr, RGB565 format, enable
            LCDIFV2_CTRLDESCL1(0) = LCDIFV2_CTRLDESCL1_WIDTH(PW) |
                                    LCDIFV2_CTRLDESCL1_HEIGHT(PH);
            LCDIFV2_CTRLDESCL3(0) = LCDIFV2_CTRLDESCL3_PITCH(PPITCH);
            LCDIFV2_CTRLDESCL4(0) = (uint32_t)pbuf;
            LCDIFV2_CTRLDESCL5(0) = LCDIFV2_CTRLDESCL5_BPP(LCDIFV2_CTRLDESCL5_BPP_RGB565) |
                                    LCDIFV2_CTRLDESCL5_EN;

            uint32_t tap = LCDIFV2_REG(0x3FFC);       // model's on-demand scan checksum
            uint32_t exp = fnv1a((const uint8_t *)pbuf, PW * PH * 2);
            Serial1.printf("PROBE_LCDIF=SUM:%08lX EXP:%08lX %s\n",
                           (unsigned long)tap, (unsigned long)exp,
                           tap == exp ? "PASS" : "FAIL");
        }
    }

    // (The Task-9 DSI probes lived here: they poked DSI_DPHY/DSI_APB by hand and
    // read the QEMU model's debug tap to prove the D-PHY lock bit and the
    // long/short APB packet paths. Task 10 landed the real firmware driver --
    // RPiDisplay mipi_dsi.cpp, exercised by DSI_OK above -- so the probes are
    // gone. The QEMU-side tap itself stays; Task 11 relocates it to the
    // TC358762 bridge.)

    if (ok) {
        Display.fillScreen(COLOR);
        // FB_SUM: checksum the SDRAM framebuffer we painted (proves PXP.fill)
        uint32_t fb_sum = fnv1a((const uint8_t*)Display.framebuffer(), (uint32_t)Display.width() * Display.height() * 2u);
        // expected: whole buffer is COLOR
        uint32_t exp = 2166136261u; uint8_t lo = COLOR & 0xFF, hi = (COLOR >> 8) & 0xFF;
        for (uint32_t i = 0; i < (uint32_t)Display.width() * Display.height(); i++) { exp ^= lo; exp *= 16777619u; exp ^= hi; exp *= 16777619u; }
        Serial1.printf("FB_SUM=%08lX EXP=%08lX %s\n", (unsigned long)fb_sum, (unsigned long)exp, fb_sum == exp ? "PASS" : "FAIL");
        // PANEL_SUM: QEMU-only oracle -- the virtual TC358762's received-pixel checksum (Task 11/13)
        Serial1.println("PANEL_SUM_PENDING"); // filled in at Task 13
    }
    Serial1.println("RPI_PANEL_END");
}

void loop() {}
