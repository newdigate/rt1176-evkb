/* camera_ov5640_config - RT1176 M2 part 2: configure OV5640 for 640x480 YUV422
 * over MIPI, all over SCCB (LPI2C6).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT   (register tables: BSD-3, see ov5640_init_table.h)
 *
 * Loads the full sensor config the MIPI VGA preview needs: soft-reset, the
 * 195-entry base init table, VGA windowing, YUV422 format, the MIPI-VGA PLL,
 * and MIPI 2-lane interface, then starts streaming.  Config values are the
 * NXP fsl_ov5640 MIPI path (BSD-3), ported verbatim.
 *
 * What this CAN verify without a receiver: every SCCB write is ACKed and key
 * registers read back correct (0x300e MIPI mode, 0x3008 streaming).  The actual
 * pixels can only be seen once M3 (MIPI-CSI2 RX) captures a frame - that is also
 * where the 0x4300 byte-order (UYVY vs YUYV) gets pinned against the PXP.
 *
 * SCCB via the Wire lpi2c1176 core on LPI2C6 (see camera_ov5640_id for the map).
 */
#include <Arduino.h>
#include <Wire.h>
#include "lpi2c1176.h"
#include "ov5640_init_table.h"

#define OV5640_ADDR 0x3C

/* 0x4300 YUV422 byte order.  NXP's MIPI "YUYV" uses 0x3F; our PXP back-half
 * wants UYVY (PXP_UYVY1P422). Which byte order 0x3F actually emits can only be
 * confirmed with real pixels at M3 - keep it a single knob to flip there. */
#define OV5640_REG_4300  0x3F

static lpi2c1176_regs_t *const LPI2C6 = (lpi2c1176_regs_t *)0x40C38000u;
static const lpi2c1176_hw_t cam_i2c_hw = {
    (volatile uint32_t *)0x40CC6CE0u, (volatile uint32_t *)0x40CC1500u, (1u << 8),
    (volatile uint32_t *)0x40C0801Cu, 0x10u, (volatile uint32_t *)0x40C0805Cu,
    (volatile uint32_t *)0x40C08018u, 0x10u, (volatile uint32_t *)0x40C08058u,
    (volatile uint32_t *)0x40C0808Cu, 0u, (volatile uint32_t *)0x40C08090u, 0u, 0x20u,
};
#define REG32(a) (*(volatile uint32_t *)(a))
static void control_pins_init(void)
{
    REG32(0x400E8174u) = 0xAu; REG32(0x400E83B8u) = 0x0Cu;   /* PWDN GPIO9_IO25  */
    REG32(0x400E824Cu) = 0xAu; REG32(0x400E8490u) = 0x0Cu;   /* RST  GPIO11_IO15 */
    REG32(0x40C64004u) |= (1u << 25);   /* GPIO9 GDIR  */
    REG32(0x40C6C004u) |= (1u << 15);   /* GPIO11 GDIR */
}
static void pwdn(bool hi) { REG32(hi ? 0x40C64084u : 0x40C64088u) = (1u << 25); }
static void rst (bool hi) { REG32(hi ? 0x40C6C084u : 0x40C6C088u) = (1u << 15); }

static uint32_t g_writes = 0, g_nacks = 0;

/* SCCB write: [regHi, regLo, val]+STOP.  Tracks NACKs. */
static void wr(uint16_t reg, uint8_t val)
{
    uint8_t b[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    uint32_t r = lpi2c1176_master_write(LPI2C6, OV5640_ADDR, b, 3, 1);
    g_writes++;
    if (r != 0) g_nacks++;
}
/* SCCB multi-write: reg addr then N auto-incrementing values in one transaction. */
static void wr_multi(uint16_t reg, const uint8_t *vals, uint32_t n)
{
    uint8_t b[2 + 32];
    if (n > 32) return;
    b[0] = (uint8_t)(reg >> 8); b[1] = (uint8_t)(reg & 0xFF);
    for (uint32_t i = 0; i < n; i++) b[2 + i] = vals[i];
    uint32_t r = lpi2c1176_master_write(LPI2C6, OV5640_ADDR, b, 2 + n, 1);
    g_writes++;
    if (r != 0) g_nacks++;
}
static uint8_t rd(uint16_t reg)
{
    uint8_t w[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) }, v = 0;
    lpi2c1176_master_write(LPI2C6, OV5640_ADDR, w, 2, 1);
    lpi2c1176_master_read(LPI2C6, OV5640_ADDR, &v, 1, 1);
    return v;
}

/* VGA windowing (0x3800..0x3815), NXP resolutionParam[kVIDEO_ResolutionVGA]. */
static const uint8_t vga_param[22] = {
    0x00,0x00,0x00,0x04,0x0a,0x3f,0x07,0x9b,0x02,0x80,0x01,
    0xe0,0x07,0x68,0x03,0xd8,0x00,0x10,0x00,0x06,0x31,0x31,
};

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("OV5640_CFG_BEGIN");

    control_pins_init();
    lpi2c1176_begin(LPI2C6, &cam_i2c_hw, 100000);

    /* Power-up. */
    pwdn(true); rst(false); delay(5);
    pwdn(false);            delay(1);
    rst(true);              delay(20);

    uint16_t id = ((uint16_t)rd(0x300A) << 8) | rd(0x300B);
    Serial1.printf("CHIP_ID=0x%04X\n", id);

    /* Software reset. */
    wr(0x3103, 0x11); wr(0x3008, 0x82); delay(5);

    /* Base init table (195 entries, verbatim). */
    for (uint32_t i = 0; i < (sizeof(ov5640_init_table)/sizeof(ov5640_init_table[0])); i++)
        wr(ov5640_init_table[i].reg, ov5640_init_table[i].val);

    /* VGA resolution + format + MIPI-VGA clock. */
    wr_multi(0x3800, vga_param, 22);
    wr(0x302c, 0xc2);
    wr(0x4300, OV5640_REG_4300); wr(0x501f, 0x00);        /* YUV422 */
    wr(0x3035, 0x22); wr(0x3036, 0x38); wr(0x460c, 0x22); /* MIPI VGA PLL */
    wr(0x3824, 0x02); wr(0x4837, 0x0a);

    /* MIPI 2-lane interface. */
    wr(0x3034, 0x18); wr(0x3017, 0x00); wr(0x3018, 0x00);
    wr(0x300e, 0x45);                    /* MIPI, 2 data lanes */
    wr(0x4800, 0x04);

    /* Start streaming. */
    wr(0x3008, 0x02);

    Serial1.printf("SCCB_WRITES=%lu NACKS=%lu\n",
                   (unsigned long)g_writes, (unsigned long)g_nacks);
    /* Read-back confirmations (independent of the write path). */
    uint8_t r300e = rd(0x300e), r3008 = rd(0x3008), r4300 = rd(0x4300);
    Serial1.printf("READBACK 0x300e=0x%02X(MIPI 0x45) 0x3008=0x%02X(stream 0x02) 0x4300=0x%02X\n",
                   r300e, r3008, r4300);

    bool ok = (id == 0x5640) && (g_nacks == 0) && (r300e == 0x45) && (r3008 == 0x02);
    Serial1.printf("OV5640_CFG=%s\n", ok ? "PASS" : "FAIL");
    Serial1.println("OV5640_CFG_END");
}

void loop() {}
