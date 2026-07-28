/* rk055_panel_test - RT1176 -> RK055HDMIPI4MA0 720x1280 MIPI-DSI panel gate
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * As of M3 this gate is a CLOSED, self-validating loop: paint the SDRAM
 * framebuffer with a colour-bar/diagonal test pattern, then prove -- twice,
 * against two independent oracles -- that exactly those pixels are what the
 * panel gets.
 *
 *   CLK_OK / LCDIFV2_OK / DSI_OK / PANEL_OK   (M1 + M2)
 *       each bring-up layer.
 *   FRAME_OK
 *       a whole LCDIFv2 frame scanned out after the paint.  Neither checksum
 *       below can see this: both read pixels, and pixels read the same whether
 *       the display is scanning or stopped dead.
 *   FB_SUM
 *       FNV-1a of the framebuffer as it sits in SDRAM, vs a software-computed
 *       expectation.  Proves drawTestPattern() wrote what it was asked to.
 *   PANEL_SUM
 *       the virtual HX8394's received-pixel checksum vs the SAME expectation.
 *       QEMU-only by construction; the tap returns a sentinel unless the whole
 *       chain (init contract, DSI link, enabled layer) really would put those
 *       pixels on glass.
 *   UNDERRUN
 *       the LCDIFv2 output-FIFO underrun sampler.  HARDWARE-ONLY by
 *       construction -- QEMU has no timing model, so the QEMU value is vacuous
 *       and is labelled as such where it is printed.
 *
 * Why a pattern and not a solid fill: a uniform colour is indistinguishable
 * from the LCDIFv2 emitting its safe colour after a FIFO underrun.  Bars plus a
 * diagonal make blank output, a wrong stride, a wrong pixel format and a
 * flipped image each look different.
 *
 * Every stage token is emitted UNCONDITIONALLY so the first FAIL pinpoints the
 * broken layer rather than the run simply stopping.
 *
 * Uses Serial1 (the LPUART console run_qemu.sh captures via `-serial file:`),
 * not Serial (native USB CDC, which QEMU would not capture here).
 */
#include <Arduino.h>
#include "Display.h"
// The two diagnostic accessors used only on the PANEL_FAIL path below.
// hx8394.h says WHICH of hx8394Init()'s six early returns fired (and, for the
// tuning table, at which index/opcode); mipi_dsi.h says WHY the DSI host
// refused that packet.  Neither is reached on a passing run.
#include "hx8394.h"
#include "mipi_dsi.h"

// --- the QEMU-only virtual HX8394 debug tap ----------------------------------
// Layout: qemu2 include/hw/display/imxrt_hx8394.h.  Mapped over the RESERVED
// upper part of the MIPI-DSI host's own AIPS slot, so on real silicon these
// reads return 0 instead of faulting -- TAP_ID is how this gate tells "no tap
// here" (hardware) from "tap says the panel is dark" (a real failure).  A
// DIFFERENT 4 KiB window from the TC358762's 0x4080F000, because both panel
// models are built into the one QEMU binary.  Nothing in the firmware may
// depend on any of it.
#define HX_TAP(off)         (*(volatile uint32_t *)(0x4080E000u + (off)))
#define HX_TAP_ID           HX_TAP(0x00)
#define HX_TAP_STATUS       HX_TAP(0x04)
#define HX_TAP_PANEL_SUM    HX_TAP(0x08)
#define HX_TAP_ID_MAGIC     0x48583934u   // "HX94"; 0 on real silicon

// --- FNV-1a-32 ---------------------------------------------------------------
// Duplicated from rpi_panel_test.cpp rather than shared: five lines, and it
// keeps the two gates independent of each other.
//
// One folding primitive, two walkers: the real framebuffer, and the frame we
// EXPECT.  Spelling the round out twice is how the two would silently drift
// into agreeing with each other instead of with the pixels.
static const uint32_t FNV1A_OFFSET = 2166136261u;
static const uint32_t FNV1A_PRIME  = 16777619u;

static inline uint32_t fnv1a_byte(uint32_t h, uint8_t b) {
    return (h ^ b) * FNV1A_PRIME;
}

// The bytes actually in memory.
static uint32_t fnv1a(const uint8_t *p, uint32_t n) {
    uint32_t h = FNV1A_OFFSET;
    while (n--) h = fnv1a_byte(h, *p++);
    return h;
}

// The bytes the pattern SHOULD be, folded in ascending address order,
// little-endian halfwords -- how an RGB565 pixel sits in SDRAM.
//
// A SECOND, INDEPENDENT implementation of the pattern rule, deliberately not a
// call into the driver.  If the gate asked the driver what it drew, the two
// would agree with each other even when both disagree with the panel.  The bar
// table is restated here for the same reason.
static uint32_t fnv1a_pattern() {
    static const uint16_t bars[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000,
    };
    const uint32_t barWidth = PANEL_WIDTH / 8u;
    uint32_t h = FNV1A_OFFSET;
    for (uint32_t y = 0; y < PANEL_HEIGHT; y++) {
        const uint32_t dx = (uint32_t)(((uint64_t)y * PANEL_WIDTH) / PANEL_HEIGHT);
        for (uint32_t x = 0; x < PANEL_WIDTH; x++) {
            uint32_t bar = x / barWidth;
            if (bar > 7u) bar = 7u;
            const uint16_t px = (x == dx) ? 0xFFFFu : bars[bar];
            h = fnv1a_byte(h, (uint8_t)(px & 0xFFu));
            h = fnv1a_byte(h, (uint8_t)(px >> 8));
        }
    }
    return h;
}

void setup() {
    Serial1.begin(115200);
    delay(200);
    Serial1.println("RK055_PANEL_BEGIN");
    Serial1.printf("PANEL=%s\n", PANEL_NAME);
    Serial1.printf("GEOM=%ux%u PIXCLK=%lu LANES=%lu\n",
                   (unsigned)PANEL_WIDTH, (unsigned)PANEL_HEIGHT,
                   (unsigned long)PANEL_PIXEL_CLK_HZ,
                   (unsigned long)PANEL_DSI_LANES);

    const bool ok = Display.begin();
    Serial1.printf("CLK_%s\n",     Display.clkOk()   ? "OK" : "FAIL");
    Serial1.printf("LCDIFV2_%s\n", Display.lcdifOk() ? "OK" : "FAIL");
    Serial1.printf("DSI_%s\n",     Display.dsiOk()   ? "OK" : "FAIL");
    Serial1.printf("PANEL_%s\n", Display.panelOk() ? "OK" : "FAIL");

    // Diagnostics, printed ONLY when the panel stage failed and strictly AFTER
    // the PANEL_ token -- run_qemu.sh greps for "PANEL_OK", so nothing here may
    // precede it or contain that string.  These lines are additive: a passing
    // run's output is byte-identical to before.
    //
    // Two independent questions, answered separately because they have
    // different answers:
    //   PANEL_ERR  -- WHICH of hx8394Init()'s six early returns fired.  For
    //                 TUNING the index into the 21-entry table and the failing
    //                 command's opcode come too; IDX=255 means "not a tuning
    //                 failure" (HX8394_NO_INDEX).
    //   DSI_ERR    -- WHY the DSI host refused that packet, plus the host
    //                 status registers sampled at the failure point.  This is
    //                 still valid here because hx8394Init() bails at the FIRST
    //                 refusal and sends nothing afterwards, so the last
    //                 dsiWrite() recorded IS the one that failed.
    //                 SPINS=entry/start/idle separates the three timeout
    //                 flavours: a full ENTRY count means the previous packet
    //                 never drained, a full START count means this packet never
    //                 left the gate, a full IDLE count means it started and
    //                 never finished.
    // On HX8394_ERR_GPIO no packet was ever sent, so the DSI_ERR line describes
    // whatever the last write was (nothing, on a fresh boot) -- read PANEL_ERR
    // first and only trust DSI_ERR when PANEL_ERR names a packet.
    if (!Display.panelOk()) {
        Serial1.printf("PANEL_ERR=%s IDX=%u OPCODE=0x%02X\n",
                       hx8394ErrorName(hx8394LastError()),
                       (unsigned)hx8394LastErrorIndex(),
                       (unsigned)hx8394LastErrorOpcode());
        const DsiWriteStatus &d = dsiLastWriteStatus();
        Serial1.printf("DSI_ERR=%s DT=0x%02X LEN=%lu\n",
                       dsiWriteErrorName(d.error), (unsigned)d.data_type,
                       (unsigned long)d.len);
        Serial1.printf("DSI_PKT_STATUS=0x%08lX IRQ1=0x%08lX IRQ2=0x%08lX "
                       "RXERR=0x%08lX\n",
                       (unsigned long)d.pkt_status,
                       (unsigned long)d.irq_status,
                       (unsigned long)d.irq_status2,
                       (unsigned long)d.rx_error_status);
        Serial1.printf("DSI_FIFO=wr:%lu rd:%lu LOCK=0x%08lX "
                       "SPINS=entry:%lu start:%lu idle:%lu\n",
                       (unsigned long)d.fifo_wr_level,
                       (unsigned long)d.fifo_rd_level,
                       (unsigned long)d.dphy_lock,
                       (unsigned long)d.entry_spins,
                       (unsigned long)d.start_spins,
                       (unsigned long)d.idle_spins);
    }

    // M3 -- pixels.  Guarded on the whole bring-up rather than on panelOk()
    // alone: with no framebuffer there is nothing to paint or checksum, and the
    // stage tokens above have already named the broken layer.
    if (ok) {
        Display.drawTestPattern();
        Serial1.printf("FRAME_%s\n", Display.frameOk() ? "OK" : "TIMEOUT");

        // The flat FB_SUM walk and the per-pixel expectation only agree while
        // the pitch has no padding, so pin that at compile time rather than
        // letting it surface as a confusing runtime mismatch.
        static_assert(PANEL_FB_BYTES ==
                          (uint32_t)PANEL_WIDTH * PANEL_HEIGHT * PANEL_BYTES_PER_PIXEL,
                      "FB_SUM walks the framebuffer flat and the expectation counts "
                      "pixels; a padded pitch breaks both");
        const uint32_t expect = fnv1a_pattern();

        // FB_SUM -- oracle 1: the bytes really in SDRAM.  Proves the paint.
        // (Read straight back with the CPU: this core runs with the D-cache
        // off, so there is no stale line between the writes and this.)
        const uint32_t fb_sum =
            fnv1a((const uint8_t *)Display.framebuffer(), PANEL_FB_BYTES);
        Serial1.printf("FB_SUM=0x%08lX EXP=0x%08lX %s\n",
                       (unsigned long)fb_sum, (unsigned long)expect,
                       fb_sum == expect ? "PASS" : "FAIL");

        // PANEL_SUM -- oracle 2: what the panel would put on glass.  Branch on
        // TAP_ID, never on the checksum: the tap reads 0 on silicon, where a
        // sentinel PANEL_SUM means "no tap", not "dark panel".  Testing the
        // checksum against 0 would conflate the two and fail every hardware run.
        const uint32_t tap_id = HX_TAP_ID;
        if (tap_id != HX_TAP_ID_MAGIC) {
            Serial1.printf("PANEL_SUM_HW=TAP_ABSENT (TAP_ID=0x%08lX)"
                           " -- verify by eye\n", (unsigned long)tap_id);
        } else {
            const uint32_t panel_sum = HX_TAP_PANEL_SUM;
            const bool panel_pass = (panel_sum == expect);
            Serial1.printf("PANEL_SUM=0x%08lX EXP=0x%08lX %s\n",
                           (unsigned long)panel_sum, (unsigned long)expect,
                           panel_pass ? "PASS" : "FAIL");
            // On a mismatch, say WHICH precondition is missing rather than just
            // "wrong number": TAP_STATUS carries one bit per contract condition
            // (HX8394_ST_* in imxrt_hx8394.h).
            if (!panel_pass) {
                Serial1.printf("TAP_STATUS=0x%08lX\n",
                               (unsigned long)HX_TAP_STATUS);
            }
        }

        // The LCDIFv2 output-FIFO underrun sampler.  The "(QEMU: vacuous)"
        // suffix keys off the SAME TAP_ID that already separates emulator from
        // silicon, so a QEMU transcript can never be misread as evidence about
        // this panel's ~108 MB/s scanout bandwidth.
        Display.sampleUnderrun(10);
        Serial1.printf("UNDERRUN=%lu/%lu%s\n",
                       (unsigned long)Display.underrunCount(),
                       (unsigned long)Display.underrunFrames(),
                       (tap_id == HX_TAP_ID_MAGIC) ? " (QEMU: vacuous)" : "");
    }

    Serial1.println("RK055_PANEL_END");
}

void loop() {}
