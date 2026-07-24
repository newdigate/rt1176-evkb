/* rpi_panel_test - RT1176 -> RPi 7" MIPI-DSI display v1 gate
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * As of Task 11: ATTINY_OK + PLL_OK + LCDIFV2_OK + DSI_OK (the ATtiny88 driver,
 * the VIDEO_PLL / LCDIFv2+MIPI clock-root bring-up, the LCDIFv2 driver and the
 * MIPI-DSI host driver are all live), TC358762_FAIL and FB_SUM never appears --
 * begin() still returns false overall (later stages unimplemented). Later tasks
 * (12,13) turn each remaining stage green in turn as the real TC358762 bridge
 * and PXP-fill drivers land.
 *
 * PROBE_BRIDGE is a TEMP Task-11 probe of the QEMU-side virtual TC358762
 * (readiness FSM + PANEL_SUM oracle), removed once Task 12's real bridge
 * driver drives the same path.
 *
 * NOTE: uses Serial1 (the LPUART console every sibling gate's run_qemu.sh
 * captures via `-serial file:`), not Serial (native USB CDC, which QEMU
 * would not capture here) -- see cores/imxrt1176/usb_serial.h.
 */
#include <Arduino.h>
#include <Wire.h>   // needed by the RPiDisplay ATtiny driver (Display::begin())
#include "Display.h"
#include "mipi_dsi.h"   // TEMP(Task 11 probe): dsiWrite() -- remove in Task 12

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

    // (The Task-7 LCDIFv2 scan-checksum probe and the Task-9 DSI packet probes
    // lived here. Both read TEMP debug taps that Task 11 retired: the LCDIFv2
    // tap at 0x3FFC and the DSI tap at 0x3F00.. are gone from the QEMU model,
    // superseded by the TC358762 bridge's tap probed below -- which checks the
    // same scan checksum, but only once the bridge agrees the panel is lit.)

    // TEMP(Task 11 probe) -- remove in Task 12. Proves the virtual TC358762
    // bridge's readiness FSM and its PANEL_SUM oracle, ahead of the real
    // firmware bridge driver (Task 12). The bridge is QEMU-only fiction: on a
    // real board the TC358762 has no registers in the SoC map at all, so this
    // whole block only means anything under QEMU (TAP_ID reads back a magic
    // there and 0 on silicon).
    //
    // Sequence:
    //   1. read the tap with NO bridge init sent -- Display.begin() has already
    //      left layer 0 enabled over a real 800x480 SDRAM framebuffer and the
    //      DSI host up, so there ARE pixels to checksum; the bridge must still
    //      report not-ready and hand back the sentinel. That negative case is
    //      the point: the panel must not light without the init sequence.
    //   2. point layer 0 at a small buffer holding a known positional pattern;
    //   3. send the minimal init sequence the bridge requires (see the
    //      required-init contract in qemu2 include/hw/display/imxrt_tc358762.h);
    //   4. re-read: ready, and PANEL_SUM == the software FNV-1a of that buffer.
    {
        const uint32_t TAP_BASE = 0x4080F000u;   // QEMU-only bridge debug tap
        #define BRIDGE_TAP(off) (*(volatile uint32_t *)(TAP_BASE + (off)))
        const uint32_t TAP_ID = 0x00, TAP_STATUS = 0x04, TAP_PANEL_SUM = 0x08;
        const uint32_t TAP_ID_MAGIC = 0x54433632u;   // "TC62"
        const uint32_t ST_READY = 1u << 3;

        uint32_t id       = BRIDGE_TAP(TAP_ID);
        uint32_t pre_stat = BRIDGE_TAP(TAP_STATUS);
        uint32_t pre_sum  = BRIDGE_TAP(TAP_PANEL_SUM);

        const uint32_t PW = 64, PH = 48;             // small test frame
        const uint32_t PPITCH = PW * 2;              // RGB565, tightly packed
        uint16_t *pbuf = (uint16_t *)extmem_malloc((size_t)PW * PH * 2);
        uint32_t exp = 0, post_stat = 0, post_sum = 0;
        bool ok_probe = false;

        if (pbuf) {
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

            // Minimal sequence satisfying the model's required-init contract:
            // a DSI/PPI-layer write (0x0100..0x02FF), then a video-path write
            // (0x0400..0x04FF), then one more write (the final start). The
            // ADDRESSES are the contract's regions; the DATA is deliberately 0
            // -- these are NOT transcribed TC358762 values and must not be
            // mistaken for the real sequence, which Task 12 transcribes from
            // the RPi references and Task 14 proves on silicon.
            static const uint16_t probe_regs[] = { 0x0210, 0x0404, 0x0104 };
            bool sent = true;
            for (unsigned i = 0; i < sizeof(probe_regs)/sizeof(probe_regs[0]); i++) {
                uint8_t pkt[6] = { (uint8_t)(probe_regs[i] & 0xFF),
                                   (uint8_t)(probe_regs[i] >> 8),
                                   0, 0, 0, 0 };
                sent = dsiWrite(0, 0x29 /* generic long write */, pkt, sizeof(pkt)) && sent;
            }

            post_stat = BRIDGE_TAP(TAP_STATUS);
            post_sum  = BRIDGE_TAP(TAP_PANEL_SUM);
            exp = fnv1a((const uint8_t *)pbuf, PW * PH * 2);
            ok_probe = sent && id == TAP_ID_MAGIC &&
                       !(pre_stat & ST_READY) && pre_sum == 0 &&
                       (post_stat & ST_READY) && post_sum == exp && exp != 0;
        }
        Serial1.printf("PROBE_BRIDGE=PRE:%08lX POST:%08lX EXP:%08lX STATUS:%08lX %s\n",
                       (unsigned long)pre_sum, (unsigned long)post_sum,
                       (unsigned long)exp, (unsigned long)post_stat,
                       ok_probe ? "PASS" : "FAIL");
        #undef BRIDGE_TAP
    }

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
