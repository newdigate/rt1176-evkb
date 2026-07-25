/* camera_preview_live - RT1176 M3 step 5: LIVE camera video on the ILI9341.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *   (front-end register sequences from NXP BSD-3 camera_support.c /
 *    fsl_mipi_csi2rx.c / fsl_csi.c; OV5640 table = ov5640_init_table.h.)
 *
 * The whole pipeline, end to end, driven by the real sensor:
 *
 *   OV5640 640x480 YUV422 --MIPI 2-lane--> CSI2RX --gasket--> CSI DMA
 *      -> XYUV8888 640x480 in SDRAM (csi_fb0/fb1 ping-pong)
 *      -> PXP pass 1: CSC1 YUV444->RGB565        -> RGB565 640x480
 *      -> PXP pass 2: decimate /2 (pixel-drop)   -> RGB565 320x240
 *      -> ILI9341 writeRect, landscape 320x240
 *
 * This is camera_preview_synth's back-half (HW-verified) fed by the real M3
 * front-end (camera_capture, HW-verified) instead of gen_frame().  The CSI
 * stores 32-bit XYUV8888, so the PS source format is PXP_YUV1P444 (0x10), not
 * the synth path's UYVY - the CSC1 math and the two-pass (YUV can't decimate in
 * one op) are otherwise identical.
 *
 * Oracle: the ILI9341 is unmodelled and QEMU-blind - the moving picture is your
 * eyes on the glass.  VCOM prints per-frame liveness (capture ok, PXP ok, a
 * checksum + centre pixels) so a still can be sanity-checked, but "is it live
 * video" is a human check.
 *
 * Display wiring (same as ili9341 gate): SCK=D13 MOSI=D11 MISO=D12 CS=D10 DC=D8 RST=D9.
 * Camera on J2 (MIPI). See rt1176-ov5640-camera memory note for the full map.
 */
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <ILI9341_t3.h>
#include <PXP.h>
#include "lpi2c1176.h"
#include "ov5640_init_table.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define CAM_W 640
#define CAM_H 480
#define OUT_W 320
#define OUT_H 240

/* ===================== front-end: clocks + D-PHY power ===================== */
#define CCM_ROOT_CTRL(n)    (0x40CC0000u + (uint32_t)(n) * 0x80u)
#define CCM_LPCG_DIRECT(n)  (0x40CC6000u + (uint32_t)(n) * 0x20u)
#define ROOT_CTRL_60MHZ (CCM_CLOCK_ROOT_CONTROL_MUX(5) | CCM_CLOCK_ROOT_CONTROL_DIV(8u - 1u))
static void csi2_clocks_init(void)
{
    REG32(CCM_LPCG_DIRECT(136)) = 1u;                 /* Video Mux gate */
    VIDEO_MUX_VID_MUX_CTRL_SET = VIDEO_MUX_VID_MUX_CTRL_CSI_SEL;
    REG32(CCM_ROOT_CTRL(73)) = ROOT_CTRL_60MHZ;       /* Csi2     */
    REG32(CCM_ROOT_CTRL(74)) = ROOT_CTRL_60MHZ;       /* Csi2_Esc */
    REG32(CCM_ROOT_CTRL(75)) = ROOT_CTRL_60MHZ;       /* Csi2_Ui  */
    PGMC_BPC4_BPC_POWER_CTRL |= (PGMC_BPC_POWER_CTRL_PSW_ON_SOFT | PGMC_BPC_POWER_CTRL_ISO_OFF_SOFT);
}

/* ===================== front-end: OV5640 over SCCB (LPI2C6) ================ */
#define OV5640_ADDR 0x3C
#define OV5640_REG_4300 0x3F   /* NXP "YUYV"; pinned at M3.6 */
static lpi2c1176_regs_t *const LPI2C6 = (lpi2c1176_regs_t *)0x40C38000u;
static const lpi2c1176_hw_t cam_i2c_hw = {
    (volatile uint32_t *)0x40CC6CE0u, (volatile uint32_t *)0x40CC1500u, (1u << 8),
    (volatile uint32_t *)0x40C0801Cu, 0x10u, (volatile uint32_t *)0x40C0805Cu,
    (volatile uint32_t *)0x40C08018u, 0x10u, (volatile uint32_t *)0x40C08058u,
    (volatile uint32_t *)0x40C0808Cu, 0u, (volatile uint32_t *)0x40C08090u, 0u, 0x20u,
};
static void control_pins_init(void)
{
    REG32(0x400E8174u) = 0xAu; REG32(0x400E83B8u) = 0x0Cu;
    REG32(0x400E824Cu) = 0xAu; REG32(0x400E8490u) = 0x0Cu;
    REG32(0x40C64004u) |= (1u << 25);
    REG32(0x40C6C004u) |= (1u << 15);
}
static void pwdn(bool hi) { REG32(hi ? 0x40C64084u : 0x40C64088u) = (1u << 25); }
static void rst (bool hi) { REG32(hi ? 0x40C6C084u : 0x40C6C088u) = (1u << 15); }
static uint32_t g_nacks = 0;
static void wr(uint16_t reg, uint8_t val)
{
    uint8_t b[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    if (lpi2c1176_master_write(LPI2C6, OV5640_ADDR, b, 3, 1) != 0) g_nacks++;
}
static void wr_multi(uint16_t reg, const uint8_t *vals, uint32_t n)
{
    uint8_t b[2 + 32];
    if (n > 32) return;
    b[0] = (uint8_t)(reg >> 8); b[1] = (uint8_t)(reg & 0xFF);
    for (uint32_t i = 0; i < n; i++) b[2 + i] = vals[i];
    if (lpi2c1176_master_write(LPI2C6, OV5640_ADDR, b, 2 + n, 1) != 0) g_nacks++;
}
static uint8_t rd(uint16_t reg)
{
    uint8_t w[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) }, v = 0;
    lpi2c1176_master_write(LPI2C6, OV5640_ADDR, w, 2, 1);
    lpi2c1176_master_read(LPI2C6, OV5640_ADDR, &v, 1, 1);
    return v;
}
static const uint8_t vga_param[22] = {
    0x00,0x00,0x00,0x04,0x0a,0x3f,0x07,0x9b,0x02,0x80,0x01,
    0xe0,0x07,0x68,0x03,0xd8,0x00,0x10,0x00,0x06,0x31,0x31,
};
static uint16_t ov5640_config(void)
{
    control_pins_init();
    lpi2c1176_begin(LPI2C6, &cam_i2c_hw, 100000);
    pwdn(true); rst(false); delay(5);
    pwdn(false);            delay(1);
    rst(true);              delay(20);
    uint16_t id = ((uint16_t)rd(0x300A) << 8) | rd(0x300B);
    wr(0x3103, 0x11); wr(0x3008, 0x82); delay(5);
    for (uint32_t i = 0; i < (sizeof(ov5640_init_table)/sizeof(ov5640_init_table[0])); i++)
        wr(ov5640_init_table[i].reg, ov5640_init_table[i].val);
    wr_multi(0x3800, vga_param, 22);
    wr(0x302c, 0xc2);
    wr(0x4300, OV5640_REG_4300); wr(0x501f, 0x00);
    wr(0x3035, 0x22); wr(0x3036, 0x38); wr(0x460c, 0x22);
    wr(0x3824, 0x02); wr(0x4837, 0x0a);
    wr(0x3034, 0x18); wr(0x3017, 0x00); wr(0x3018, 0x00);
    wr(0x300e, 0x45); wr(0x4800, 0x04);
    wr(0x3008, 0x02);
    return id;
}

/* ===================== front-end: CSI2RX (MIPI D-PHY) ===================== */
#define IOMUXC_GPR_GPR59 0x400E40ECu
static void csi2rx_init(uint8_t laneNum, uint8_t tHsSettle)
{
    REG32(CCM_LPCG_DIRECT(132)) = 1u;                 /* MIPI_CSI LPCG */
    REG32(IOMUXC_GPR_GPR59) |= (1u << 1);             /* SOFT_RST_N (release) */
    REG32(0x40810100u) = (uint32_t)laneNum - 1u;      /* CFG_NUM_LANES */
    REG32(0x40810104u) = 0x3u & ~((1u << laneNum) - 1u); /* CFG_DISABLE_DATA_LANES */
    REG32(0x4081012Cu) = 0u;                          /* CFG_DISABLE_PAYLOAD_0 */
    REG32(0x40810130u) = 0u;                          /* CFG_DISABLE_PAYLOAD_1 */
    REG32(0x40810110u) = 0x1FFu;                      /* IRQ_MASK all */
    REG32(0x40818020u) = 0u;                          /* VIDEO_MUX PLM_CTRL.RW */
    uint32_t settle = (tHsSettle > 0u) ? ((uint32_t)tHsSettle - 1u) : 0u;
    REG32(IOMUXC_GPR_GPR59) =
        (REG32(IOMUXC_GPR_GPR59) & ~0x3F000u) |
        (1u<<5) | (1u<<0) | (1u<<1) | (1u<<4) | (1u<<3) | (1u<<2) | /* RX_EN|AUTO_PD|RST_N|PD_RX|DDRCLK|CONT_CLK */
        ((settle << 12) & 0x3F000u);
    REG32(0x40818050u) = 0u;                          /* CFG_DT_DISABLE.RW */
    REG32(0x40818024u) = (1u << 0) | (1u << 3);       /* PLM_CTRL.SET ENABLE|VALID_OVERRIDE */
    REG32(IOMUXC_GPR_GPR59) &= ~(1u << 4);            /* clear PD_RX -> power up PHY */
}

/* ===================== front-end: CSI frame grabber ======================= */
#define CSI_CR1   0x40800000u
#define CSI_CR2   0x40800004u
#define CSI_CR3   0x40800008u
#define CSI_SR    0x40800018u
#define CSI_DMASA_FB1 0x40800028u
#define CSI_DMASA_FB2 0x4080002Cu
#define CSI_FBUF_PARA 0x40800030u
#define CSI_IMAG_PARA 0x40800034u
#define CSI_CR18  0x40800048u
#define SR_DMA_TSF_DONE_FB1 (1u<<19)
#define SR_DMA_TSF_DONE_FB2 (1u<<20)

EXTMEM __attribute__((aligned(64))) static uint32_t csi_fb0[CAM_W * CAM_H];
EXTMEM __attribute__((aligned(64))) static uint32_t csi_fb1[CAM_W * CAM_H];
EXTMEM __attribute__((aligned(64))) static uint16_t rgb640[CAM_W * CAM_H];
EXTMEM __attribute__((aligned(64))) static uint16_t rgb320[OUT_W * OUT_H];

static void csi_reflash_rxdma(void)
{
    REG32(CSI_CR3) |= (1u << 14);
    while (REG32(CSI_CR3) & (1u << 14)) { }
}
static void csi_init(void)
{
    REG32(CSI_CR18) &= ~(1u << 31);                   /* stop */
    REG32(CSI_CR3) = 0u;
    REG32(CSI_CR3) |= (1u << 15);                     /* FRMCNT_RST */
    while (REG32(CSI_CR3) & (1u << 15)) { }
    /* clear RxFIFO (FCC low, set CLR_RXFIFO, wait, restore) */
    { uint32_t cr1 = REG32(CSI_CR1) & ~(1u<<8);
      REG32(CSI_CR1) = cr1 | (1u<<5); while (REG32(CSI_CR1) & (1u<<5)) {}
      REG32(CSI_CR1) |= (1u<<8); }
    csi_reflash_rxdma();
    REG32(CSI_SR) = REG32(CSI_SR);
    REG32(CSI_CR1)  = (1u<<11) | (1u<<30);            /* reset defaults */
    REG32(CSI_CR2)  = 0u; REG32(CSI_CR3) = 0u;
    REG32(CSI_CR18) = (0xDu << 12);                   /* AHB_HPROT */
    REG32(CSI_FBUF_PARA) = 0u; REG32(CSI_IMAG_PARA) = 0u;
    /* Init: GatedClock, 24-bit, XYUV8888 -> CR1=0x40000912 */
    REG32(CSI_CR1) = (1u<<4)|(1u<<11)|(1u<<1)|(1u<<8)|(1u<<30);
    REG32(CSI_CR18) |= (1u << 3);                     /* PARALLEL24_EN */
    REG32(CSI_IMAG_PARA) = ((uint32_t)CAM_W << 16) | (uint32_t)CAM_H;
    REG32(CSI_FBUF_PARA) = 0u;
    REG32(CSI_CR3) |= (1u << 0);                      /* ECC_AUTO_EN */
    REG32(CSI_CR2) = (3u << 30);                      /* burst 16*8 */
    REG32(CSI_CR3) = (REG32(CSI_CR3) & ~0x70u) | (2u << 4);
    csi_reflash_rxdma();
}
static void csi_start_capture(void)
{
    REG32(CSI_CR18) = (REG32(CSI_CR18) & ~0xC0000u) | (1u<<5) | (1u<<4); /* BASEADDR_SWITCH */
    REG32(CSI_DMASA_FB1) = (uint32_t)(uintptr_t)csi_fb0;
    REG32(CSI_DMASA_FB2) = (uint32_t)(uintptr_t)csi_fb1;
    csi_reflash_rxdma();
    REG32(CSI_SR) = REG32(CSI_SR);
    REG32(CSI_CR3) |= (1u << 12);                     /* DMA_REQ_EN_RFF */
    REG32(CSI_CR18) |= (1u << 31);                    /* CSI enable */
}

/* ===================== display back-half ================================== */
#define TFT_CS 10
#define TFT_DC  8
#define TFT_RST 9
ILI9341_t3 tft = ILI9341_t3(TFT_CS, TFT_DC, TFT_RST);

/* Two-pass PXP: XYUV8888(YUV444) 640x480 -> CSC RGB565 640x480 -> /2 320x240. */
static PXPError run_pipeline(uint32_t *srcFb)
{
    PXPSurface src(srcFb,  CAM_W, CAM_H, PXP_YUV1P444);
    PXPSurface mid(rgb640, CAM_W, CAM_H, PXP_RGB565);
    PXPError e = PXP.op().source(src).output(mid).run(200);
    if (e != PXP_OK) return e;
    PXPSurface dst(rgb320, OUT_W, OUT_H, PXP_RGB565);
    return PXP.op().source(mid).output(dst).decimate(PXP_DEC_2, PXP_DEC_2).run(200);
}

/* Wait (bounded) for a completed CSI frame; return the finished buffer or NULL. */
static uint32_t *wait_frame(uint32_t timeout_ms)
{
    uint32_t t0 = millis();
    while ((millis() - t0) < timeout_ms) {
        uint32_t sr = REG32(CSI_SR);
        if (sr & SR_DMA_TSF_DONE_FB1) { REG32(CSI_SR) = SR_DMA_TSF_DONE_FB1; return csi_fb0; }
        if (sr & SR_DMA_TSF_DONE_FB2) { REG32(CSI_SR) = SR_DMA_TSF_DONE_FB2; return csi_fb1; }
    }
    return nullptr;
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("CAM_LIVE_BEGIN");

    Serial1.printf("PXP_BEGIN=%s\n", PXP.begin() ? "PASS" : "FAIL");
    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(ILI9341_BLACK);

    csi2_clocks_init();
    uint16_t id = ov5640_config();
    Serial1.printf("CHIP_ID=0x%04X NACKS=%lu\n", id, (unsigned long)g_nacks);
    csi2rx_init(2, 0x1F);
    delay(50);
    csi_init();
    csi_start_capture();

    /* First frame end-to-end, with diagnostics printed before the slow SPI push. */
    uint32_t *fb = wait_frame(1000);
    Serial1.printf("first frame: %s\n", fb ? "captured" : "TIMEOUT");
    if (fb) {
        PXPError e = run_pipeline(fb);
        uint32_t sum = 2166136261u;
        for (uint32_t i = 0; i < OUT_W * OUT_H; i++) { sum = (sum ^ (rgb320[i] & 0xFF)) * 16777619u; sum = (sum ^ (rgb320[i] >> 8)) * 16777619u; }
        Serial1.printf("PIPE_RUN=%s (err=%d) RGB_SUM=0x%08lX center=%04X %04X\n",
                       e == PXP_OK ? "PASS" : "FAIL", (int)e, (unsigned long)sum,
                       rgb320[OUT_H/2*OUT_W + OUT_W/2], rgb320[OUT_H/2*OUT_W + OUT_W/2 + 1]);
        if (e == PXP_OK) tft.writeRect(0, 0, OUT_W, OUT_H, rgb320);
    }
    Serial1.println("CAM_LIVE_SETUP_DONE (look at the ILI9341 for live video)");
}

void loop()
{
    uint32_t *fb = wait_frame(200);
    if (fb && run_pipeline(fb) == PXP_OK)
        tft.writeRect(0, 0, OUT_W, OUT_H, rgb320);
}
