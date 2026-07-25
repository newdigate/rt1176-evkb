/* camera_capture - RT1176 M3 front-end: OV5640 -> MIPI-CSI2 RX (-> CSI, M3.4).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *   (register sequences transcribed from NXP BSD-3 camera_support.c,
 *    fsl_mipi_csi2rx.c + fsl_soc_mipi_csi2rx.c; OV5640 table = ov5640_init_table.h.)
 *
 * The whole camera front-end in one image, built incrementally:
 *   M3.2  CSI2 clock roots + VIDEO_MUX CSI_SEL + D-PHY power-on   (HW-verified)
 *   M2    OV5640 640x480 YUV422 over MIPI, all via SCCB (LPI2C6)  (HW-verified)
 *   M3.3  CSI2RX_Init: 2-lane D-PHY, tHsSettle=0x1F for VGA@30    <-- this step
 *   M3.4  fsl_csi frame grabber -> SDRAM (the first "see pixels" checkpoint)
 *
 * M3.3 checkpoint (no frame grabber yet): confirm CSI2RX_Init took (config regs
 * read back correct, GPR59 D-PHY control = expected) and dump the D-PHY receive
 * liveness - ULPS/mark state, ECC/CRC BIT_ERR, PPI error counters - with the
 * camera streaming.  Lanes leaving ULPS / sitting in a stable mark state is
 * evidence the receiver front-end sees the sensor.  Real pixels prove it at M3.4.
 */
#include <Arduino.h>
#include <Wire.h>
#include "lpi2c1176.h"
#include "ov5640_init_table.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* ===================== M3.2: CSI2 clocks + D-PHY power ===================== */
#define SYS_PLL3_CTRL       0x40C84210u
#define CCM_ROOT_CTRL(n)    (0x40CC0000u + (uint32_t)(n) * 0x80u)
#define CCM_LPCG_DIRECT(n)  (0x40CC6000u + (uint32_t)(n) * 0x20u)
#define ROOT_CSI2 73
#define ROOT_CSI2_ESC 74
#define ROOT_CSI2_UI 75
#define ROOT_CTRL_60MHZ (CCM_CLOCK_ROOT_CONTROL_MUX(5) | CCM_CLOCK_ROOT_CONTROL_DIV(8u - 1u)) /* 0x507 */
#define LPCG_VIDEO_MUX 136

static void csi2_clocks_init(void)
{
    /* Route CSI2RX->CSI (Video Mux), roots to 60 MHz off SysPll3, D-PHY power on. */
    REG32(CCM_LPCG_DIRECT(LPCG_VIDEO_MUX)) = 1u;      /* enable Video Mux gate */
    VIDEO_MUX_VID_MUX_CTRL_SET = VIDEO_MUX_VID_MUX_CTRL_CSI_SEL;
    REG32(CCM_ROOT_CTRL(ROOT_CSI2))     = ROOT_CTRL_60MHZ;
    REG32(CCM_ROOT_CTRL(ROOT_CSI2_ESC)) = ROOT_CTRL_60MHZ;
    REG32(CCM_ROOT_CTRL(ROOT_CSI2_UI))  = ROOT_CTRL_60MHZ;
    PGMC_BPC4_BPC_POWER_CTRL |= (PGMC_BPC_POWER_CTRL_PSW_ON_SOFT | PGMC_BPC_POWER_CTRL_ISO_OFF_SOFT);
}

/* ===================== M2: OV5640 config over SCCB (LPI2C6) ================ */
#define OV5640_ADDR 0x3C
#define OV5640_REG_4300 0x3F   /* NXP "YUYV"; byte order pinned at M3.6 */

static lpi2c1176_regs_t *const LPI2C6 = (lpi2c1176_regs_t *)0x40C38000u;
static const lpi2c1176_hw_t cam_i2c_hw = {
    (volatile uint32_t *)0x40CC6CE0u, (volatile uint32_t *)0x40CC1500u, (1u << 8),
    (volatile uint32_t *)0x40C0801Cu, 0x10u, (volatile uint32_t *)0x40C0805Cu,
    (volatile uint32_t *)0x40C08018u, 0x10u, (volatile uint32_t *)0x40C08058u,
    (volatile uint32_t *)0x40C0808Cu, 0u, (volatile uint32_t *)0x40C08090u, 0u, 0x20u,
};
static void control_pins_init(void)
{
    REG32(0x400E8174u) = 0xAu; REG32(0x400E83B8u) = 0x0Cu;   /* PWDN GPIO9_IO25  */
    REG32(0x400E824Cu) = 0xAu; REG32(0x400E8490u) = 0x0Cu;   /* RST  GPIO11_IO15 */
    REG32(0x40C64004u) |= (1u << 25);
    REG32(0x40C6C004u) |= (1u << 15);
}
static void pwdn(bool hi) { REG32(hi ? 0x40C64084u : 0x40C64088u) = (1u << 25); }
static void rst (bool hi) { REG32(hi ? 0x40C6C084u : 0x40C6C088u) = (1u << 15); }

static uint32_t g_writes = 0, g_nacks = 0;
static void wr(uint16_t reg, uint8_t val)
{
    uint8_t b[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    if (lpi2c1176_master_write(LPI2C6, OV5640_ADDR, b, 3, 1) != 0) g_nacks++;
    g_writes++;
}
static void wr_multi(uint16_t reg, const uint8_t *vals, uint32_t n)
{
    uint8_t b[2 + 32];
    if (n > 32) return;
    b[0] = (uint8_t)(reg >> 8); b[1] = (uint8_t)(reg & 0xFF);
    for (uint32_t i = 0; i < n; i++) b[2 + i] = vals[i];
    if (lpi2c1176_master_write(LPI2C6, OV5640_ADDR, b, 2 + n, 1) != 0) g_nacks++;
    g_writes++;
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
    wr(0x3008, 0x02);   /* start streaming */
    return id;
}

/* ===================== M3.3: MIPI-CSI2 RX (CSI2RX_Init) ==================== */
/* MIPI_CSI2RX @ 0x40810000 (HAS_NO_REG_PREFIX -> unprefixed field regs). */
#define CSI2RX_CFG_NUM_LANES        0x40810100u
#define CSI2RX_CFG_DISABLE_DATA_LANES 0x40810104u
#define CSI2RX_BIT_ERR              0x40810108u
#define CSI2RX_IRQ_STATUS           0x4081010Cu
#define CSI2RX_IRQ_MASK             0x40810110u
#define CSI2RX_ULPS_STATUS          0x40810114u
#define CSI2RX_PPI_ERRSOT_HS        0x40810118u
#define CSI2RX_PPI_ERRSOTSYNC_HS    0x4081011Cu
#define CSI2RX_PPI_ERRESC           0x40810120u
#define CSI2RX_PPI_ERRSYNCESC       0x40810124u
#define CSI2RX_PPI_ERRCONTROL       0x40810128u
#define CSI2RX_CFG_DISABLE_PAYLOAD_0 0x4081012Cu
#define CSI2RX_CFG_DISABLE_PAYLOAD_1 0x40810130u
#define CSI2RX_NUM_LANES_MASK       0x3u
#define CSI2RX_IRQ_MASK_ALL         0x1FFu
#define LPCG_MIPI_CSI               132   /* kCLOCK_Mipi_Csi */

/* The CSI2RX "CSR" lives in IOMUXC_GPR->GPR59 + VIDEO_MUX PLM_CTRL/CFG_DT_DISABLE. */
#define IOMUXC_GPR_GPR59            0x400E40ECu
#define GPR59_AUTO_PD_EN            (1u << 0)
#define GPR59_SOFT_RST_N           (1u << 1)   /* active-low reset (1 = released) */
#define GPR59_CONT_CLK_MODE        (1u << 2)
#define GPR59_DDRCLK_EN            (1u << 3)
#define GPR59_PD_RX                (1u << 4)   /* PHY power-down (1=down) */
#define GPR59_RX_ENABLE            (1u << 5)
#define GPR59_RXHS_SETTLE_SHIFT    12
#define GPR59_RXHS_SETTLE_MASK     0x3F000u
#define VIDEO_MUX_PLM_CTRL_RW      0x40818020u
#define VIDEO_MUX_PLM_CTRL_SET     0x40818024u
#define VIDEO_MUX_CFG_DT_DISABLE_RW 0x40818050u
#define PLM_CTRL_ENABLE            (1u << 0)
#define PLM_CTRL_VALID_OVERRIDE    (1u << 3)

/* laneNum=2, tHsSettle=0x1F (VGA@30).  Mirrors fsl_mipi_csi2rx.c CSI2RX_Init +
 * fsl_soc_mipi_csi2rx.c MIPI_CSI2RX_SoftwareReset/InitInterface. */
static void csi2rx_init(uint8_t laneNum, uint8_t tHsSettle)
{
    /* Un-gate the MIPI_CSI LPCG. */
    REG32(CCM_LPCG_DIRECT(LPCG_MIPI_CSI)) = 1u;

    /* SoftwareReset(false): release the controller reset (SOFT_RST_N=1). */
    REG32(IOMUXC_GPR_GPR59) |= GPR59_SOFT_RST_N;

    /* Lane + payload config. */
    REG32(CSI2RX_CFG_NUM_LANES) = (uint32_t)laneNum - 1u;
    REG32(CSI2RX_CFG_DISABLE_DATA_LANES) =
        CSI2RX_NUM_LANES_MASK & ~((1u << laneNum) - 1u);
    REG32(CSI2RX_CFG_DISABLE_PAYLOAD_0) = 0u;
    REG32(CSI2RX_CFG_DISABLE_PAYLOAD_1) = 0u;
    REG32(CSI2RX_IRQ_MASK) = CSI2RX_IRQ_MASK_ALL;   /* mask all IRQs */

    /* InitInterface(tHsSettle): D-PHY control via GPR59 + PLM via VIDEO_MUX. */
    REG32(VIDEO_MUX_PLM_CTRL_RW) = 0u;
    uint32_t settle = (tHsSettle > 0u) ? ((uint32_t)tHsSettle - 1u) : 0u;
    REG32(IOMUXC_GPR_GPR59) =
        (REG32(IOMUXC_GPR_GPR59) & ~GPR59_RXHS_SETTLE_MASK) |
        GPR59_RX_ENABLE | GPR59_AUTO_PD_EN | GPR59_SOFT_RST_N | GPR59_PD_RX |
        GPR59_DDRCLK_EN | GPR59_CONT_CLK_MODE |
        ((settle << GPR59_RXHS_SETTLE_SHIFT) & GPR59_RXHS_SETTLE_MASK);
    REG32(VIDEO_MUX_CFG_DT_DISABLE_RW) = 0u;
    REG32(VIDEO_MUX_PLM_CTRL_SET) = (PLM_CTRL_ENABLE | PLM_CTRL_VALID_OVERRIDE);
    /* Power up the PHY (clear PD_RX). */
    REG32(IOMUXC_GPR_GPR59) &= ~GPR59_PD_RX;
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("CAM_CAP_BEGIN");

    csi2_clocks_init();
    uint16_t id = ov5640_config();
    Serial1.printf("CHIP_ID=0x%04X  SCCB_WRITES=%lu NACKS=%lu\n",
                   id, (unsigned long)g_writes, (unsigned long)g_nacks);

    csi2rx_init(2, 0x1F);
    delay(50);   /* let the D-PHY settle on the incoming stream */

    /* --- CSI2RX_Init readback (config took). --- */
    uint32_t numLanes = REG32(CSI2RX_CFG_NUM_LANES);
    uint32_t disLanes = REG32(CSI2RX_CFG_DISABLE_DATA_LANES);
    uint32_t irqMask  = REG32(CSI2RX_IRQ_MASK);
    uint32_t gpr59    = REG32(IOMUXC_GPR_GPR59);
    Serial1.printf("CSI2RX cfg: NUM_LANES=%lu DISABLE_DATA_LANES=0x%lX IRQ_MASK=0x%03lX\n",
                   (unsigned long)numLanes, (unsigned long)disLanes, (unsigned long)irqMask);
    /* Only the MIPI_CSI *control* bits are ours to set; GPR59 also holds analog
     * D-PHY trim (RX_RCAL/RXCDRP/RXLPRP, bits 6-11) left at silicon defaults,
     * which InitInterface preserves.  So compare under the control mask, not the
     * whole word: RX_ENABLE|AUTO_PD_EN|SOFT_RST_N|DDRCLK_EN|CONT_CLK_MODE|PD_RX|
     * RXHS_SETTLE = 0x3F03F, expected 0x1E02F (settle 0x1E, PD_RX cleared). */
    const uint32_t GPR59_CTRL_MASK = 0x3F03Fu;
    Serial1.printf("GPR59=0x%08lX  ctrl&0x3F03F=0x%05lX (expect 0x1E02F)\n",
                   (unsigned long)gpr59, (unsigned long)(gpr59 & GPR59_CTRL_MASK));
    bool cfgOk = (numLanes == 1u) && (disLanes == 0u) && (irqMask == CSI2RX_IRQ_MASK_ALL) &&
                 ((gpr59 & GPR59_CTRL_MASK) == 0x1E02Fu);

    /* --- D-PHY receive liveness (informational; frame proof is M3.4). --- */
    uint32_t irqStat = REG32(CSI2RX_IRQ_STATUS);
    Serial1.printf("D-PHY: ULPS=0x%08lX BIT_ERR=0x%08lX IRQ_STATUS=0x%08lX%s\n",
                   (unsigned long)REG32(CSI2RX_ULPS_STATUS),
                   (unsigned long)REG32(CSI2RX_BIT_ERR),
                   (unsigned long)irqStat,
                   (irqStat & (1u << 3)) ? " [ULPS-change latched = lane activity]" : "");
    Serial1.printf("PPI err: SotHS=0x%lX SotSync=0x%lX Esc=0x%lX SyncEsc=0x%lX Ctrl=0x%lX\n",
                   (unsigned long)REG32(CSI2RX_PPI_ERRSOT_HS),
                   (unsigned long)REG32(CSI2RX_PPI_ERRSOTSYNC_HS),
                   (unsigned long)REG32(CSI2RX_PPI_ERRESC),
                   (unsigned long)REG32(CSI2RX_PPI_ERRSYNCESC),
                   (unsigned long)REG32(CSI2RX_PPI_ERRCONTROL));

    bool ok = (id == 0x5640) && (g_nacks == 0) && cfgOk;
    Serial1.printf("CSI2RX_INIT=%s\n", ok ? "PASS" : "FAIL");
    Serial1.println("CAM_CAP_END");
}

void loop() {}
