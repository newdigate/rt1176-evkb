/* rpi_panel_test - RT1176 -> RPi 7" MIPI-DSI display v1 gate
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * As of Task 6: ATTINY_OK + PLL_OK (the ATtiny88 driver and the VIDEO_PLL /
 * LCDIFv2+MIPI clock-root bring-up are live), remaining stage tokens FAIL and
 * FB_SUM never appears -- begin() still returns false overall (later stages
 * unimplemented). Later tasks (8,10,12,13) turn each remaining stage green in
 * turn as the real LCDIFv2/DSI/TC358762/PXP-fill drivers land.
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

// TEMP(Task 9 probe) -- remove in Task 10. Sends one DSI DCS/generic packet via
// the DSI_APB packet interface using the exact register sequence Task 10's
// dsiWrite() will use: pack the payload little-endian into the TX_PAYLOAD FIFO,
// write the PKT_CONTROL header (word count + data type), trigger SEND_PACKET,
// then bounded-poll PKT_STATUS back to idle. Mirrors the NXP driver's
// DSI_WriteApbTxPayload + DSI_SetApbPacketControl + DSI_SendApbPacket.
static void dsiApbLongWrite(uint8_t dataType, const uint8_t *p, uint16_t len) {
    uint16_t i = 0;
    while (i < len) {                       // pack 4 bytes/word, LE, into the FIFO
        uint32_t w = 0;
        for (uint8_t b = 0; b < 4 && i < len; b++, i++) w |= (uint32_t)p[i] << (b * 8);
        DSI_APB_TX_PAYLOAD = w;
    }
    DSI_APB_PKT_CONTROL = DSI_APB_PKT_CONTROL_WORD_COUNT(len) |
                          DSI_APB_PKT_CONTROL_HEADER_TYPE(dataType);
    DSI_APB_SEND_PACKET = DSI_APB_SEND_PACKET_TX_SEND;   // trigger the send
    for (int g = 0; g < 1000 &&
         (DSI_APB_PKT_STATUS & DSI_APB_PKT_STATUS_NOT_IDLE); g++) { /* wait idle */ }
}

// TEMP(Task 9 probe) -- remove in Task 10. Sends a DSI SHORT packet via the
// DSI_APB interface with NO TX_PAYLOAD write: the two data bytes ride in
// PKT_CONTROL's WORD_COUNT[15:0] as (data1<<8)|data0, matching how the NXP
// fsl_mipi_dsi driver emits a short packet (wordCount = (data1<<8)|data0, no
// FIFO write). Same DSI_APB sequence Task 10's dsiWrite() takes for a <=2-byte
// transfer, and the path Task 12's tc358762Init leans on.
static void dsiApbShortWrite(uint8_t dataType, uint8_t data0, uint8_t data1) {
    uint16_t wc = (uint16_t)data0 | ((uint16_t)data1 << 8);   // no FIFO write
    DSI_APB_PKT_CONTROL = DSI_APB_PKT_CONTROL_WORD_COUNT(wc) |
                          DSI_APB_PKT_CONTROL_HEADER_TYPE(dataType);
    DSI_APB_SEND_PACKET = DSI_APB_SEND_PACKET_TX_SEND;   // trigger the send
    for (int g = 0; g < 1000 &&
         (DSI_APB_PKT_STATUS & DSI_APB_PKT_STATUS_NOT_IDLE); g++) { /* wait idle */ }
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

    // TEMP(Task 9 probe) -- remove in Task 10. Proves the QEMU MIPI-DSI host
    // model: (1) the D-PHY PLL LOCK bit reads set once the PLL is powered up
    // (PD_PLL cleared), and (2) the DSI_APB packet path assembles + forwards a
    // known DCS long-write packet (data type + word count + payload) to its
    // internal debug-capture stub sink. We drive the same DSI_DPHY/DSI_APB
    // register sequence Task 10's dsiWrite() will use, then read the model's
    // temporary debug tap (spare offsets 0x3F00+ in the DSI region) and compare.
    // Task 10 lands the real firmware DSI driver (turns DSI_ green) and removes
    // this; the tap is revisited at Task 11 when the TC358762 bridge is the sink.
    {
        DSI_APB_IRQ_MASK  = 0xFFFFFFFFu;          // mask all APB IRQs (as DSI_Init)
        DSI_APB_IRQ_MASK2 = 0xFFFFFFFFu;
        DSI_DPHY_PD_PLL = 0;                       // power up the D-PHY PLL
        DSI_DPHY_PD_TX  = 0;                       // power up the D-PHY TX
        uint32_t lock = 0;
        for (int i = 0; i < 1000; i++) {           // bounded lock poll
            lock = DSI_DPHY_LOCK & DSI_DPHY_LOCK_MASK;
            if (lock) break;
        }

        // known DCS long write (data type 0x39): multi-word payload so a FIFO
        // byte-packing / word-count bug in the model would change the checksum
        static const uint8_t pld[] = { 0xB0, 0x11, 0x22, 0x33, 0x44, 0x55 };
        const uint16_t plen = (uint16_t)sizeof(pld);
        const uint8_t DT = 0x39;
        dsiApbLongWrite(DT, pld, plen);

        uint32_t tapDT  = DSI_HOST_REG(0x3F00);    // last forwarded data type
        uint32_t tapWC  = DSI_HOST_REG(0x3F04);    // last forwarded word count
        uint32_t tapSUM = DSI_HOST_REG(0x3F08);    // FNV-1a of forwarded payload
        uint32_t expSUM = fnv1a(pld, plen);
        bool pld_ok = (tapDT == DT) && (tapWC == plen) && (tapSUM == expSUM);
        Serial1.printf("PROBE_DSI=LOCK:%lu DT:%02lX WC:%lu PLD:%s\n",
                       (unsigned long)(lock ? 1u : 0u), (unsigned long)tapDT,
                       (unsigned long)tapWC, pld_ok ? "PASS" : "FAIL");

        // TEMP(Task 9 probe) -- remove in Task 10. Exercise the SHORT-packet
        // branch (data type 0x23 = generic short write, 2-param): the two data
        // bytes ride in PKT_CONTROL's word-count field with no TX_PAYLOAD write.
        // Confirm the model captured DT=0x23, WC=0xCDAB and the payload FNV-1a
        // of {0xAB,0xCD} (len is always 2 for a short packet). Task 12's
        // tc358762Init uses this short path, so it is verified at runtime here.
        const uint8_t sDT = 0x23, s0 = 0xAB, s1 = 0xCD;
        dsiApbShortWrite(sDT, s0, s1);
        uint32_t sTapDT  = DSI_HOST_REG(0x3F00);
        uint32_t sTapWC  = DSI_HOST_REG(0x3F04);
        uint32_t sTapSUM = DSI_HOST_REG(0x3F08);
        const uint8_t sExpPld[2] = { s0, s1 };
        uint32_t sExpSUM = fnv1a(sExpPld, 2);
        uint32_t sExpWC  = (uint32_t)s0 | ((uint32_t)s1 << 8);
        bool short_ok = (sTapDT == sDT) && (sTapWC == sExpWC) && (sTapSUM == sExpSUM);
        Serial1.printf("PROBE_DSI_SHORT=DT:%02lX WC:%04lX PLD:%s\n",
                       (unsigned long)sTapDT, (unsigned long)sTapWC,
                       short_ok ? "PASS" : "FAIL");
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
