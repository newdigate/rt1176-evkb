/* camera_ov5640_id - RT1176 M2 checkpoint: OV5640 (901-77346) chip-ID over SCCB
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * First HW proof of the camera front-end: read the OV5640's chip-ID registers
 * over SCCB.  A correct 0x5640 proves, in one shot, that (1) the on-board 24 MHz
 * OSC is clocking the sensor, (2) the PWDN/RST power sequence worked, and (3)
 * LPI2C6 SCCB is talking to it.  No MIPI/CSI and no display needed - serial only.
 *
 * All register addresses are inlined here (bring-up first); once HW-proven they
 * get promoted to the core header + a Wire3=LPI2C6 instance.  Sourced from the
 * RM (rev.5) and the NXP SDK IOMUXC tables:
 *   SCCB  = LPI2C6 @0x40C38000, LPCG103, CLOCK_ROOT42 mux1(24M);
 *           SCL=GPIO_LPSR_07, SDA=GPIO_LPSR_06 (ALT0|SION, LPSR pad 0x20).
 *   PWDN  = GPIO_AD_26 -> GPIO9_IO25  (ALT10), active HIGH.
 *   RST   = GPIO_DISP_B2_14 -> GPIO11_IO15 (ALT10), active LOW.
 * The XCLK is a dedicated board 24 MHz OSC - nothing to configure.
 *
 * SCCB is the Wire library's instance-agnostic LPI2C core (lpi2c1176), pointed
 * at LPI2C6.  Serial1 (LPUART) console.
 */
#include <Arduino.h>
#include <Wire.h>          /* pulls in the lpi2c1176 core it's built on */
#include "lpi2c1176.h"

#define OV5640_ADDR  0x3C  /* 7-bit SCCB address */

/* --- LPI2C6 (SCCB) hardware description ------------------------------------ */
static lpi2c1176_regs_t *const LPI2C6 = (lpi2c1176_regs_t *)0x40C38000u;
static const lpi2c1176_hw_t cam_i2c_hw = {
    /* lpcg        */ (volatile uint32_t *)0x40CC6CE0u,  /* LPCG103 DIRECT   */
    /* clock_root  */ (volatile uint32_t *)0x40CC1500u,  /* CLOCK_ROOT42     */
    /* root_val    */ (1u << 8),                         /* mux1 = Osc24M    */
    /* scl mux/pad */ (volatile uint32_t *)0x40C0801Cu, 0x10u, (volatile uint32_t *)0x40C0805Cu,
    /* sda mux/pad */ (volatile uint32_t *)0x40C08018u, 0x10u, (volatile uint32_t *)0x40C08058u,
    /* scl select  */ (volatile uint32_t *)0x40C0808Cu, 0u,
    /* sda select  */ (volatile uint32_t *)0x40C08090u, 0u,
    /* pad_ctl_val */ 0x20u,                             /* LPSR ODE         */
};

/* --- control pins (raw IOMUXC + GPIO) -------------------------------------- */
#define REG32(a)  (*(volatile uint32_t *)(a))
/* GPIO_AD_26 -> GPIO9_IO25 (PWDN) */
#define PWDN_MUX   REG32(0x400E8174u)
#define PWDN_PAD   REG32(0x400E83B8u)
#define GPIO9_GDIR REG32(0x40C64004u)
#define GPIO9_SET  REG32(0x40C64084u)
#define GPIO9_CLR  REG32(0x40C64088u)
#define PWDN_BIT   (1u << 25)
/* GPIO_DISP_B2_14 -> GPIO11_IO15 (RST) */
#define RST_MUX    REG32(0x400E824Cu)
#define RST_PAD    REG32(0x400E8490u)
#define GPIO11_GDIR REG32(0x40C6C004u)
#define GPIO11_SET REG32(0x40C6C084u)
#define GPIO11_CLR REG32(0x40C6C088u)
#define RST_BIT    (1u << 15)

static void pwdn(bool high) { if (high) GPIO9_SET  = PWDN_BIT; else GPIO9_CLR  = PWDN_BIT; }
static void rst (bool high) { if (high) GPIO11_SET = RST_BIT;  else GPIO11_CLR = RST_BIT; }

static void control_pins_init(void)
{
    PWDN_MUX = 0xAu; PWDN_PAD = 0x0Cu;   /* ALT10 (GPIO9), modest drive */
    RST_MUX  = 0xAu; RST_PAD  = 0x0Cu;   /* ALT10 (GPIO11)              */
    GPIO9_GDIR  |= PWDN_BIT;             /* outputs */
    GPIO11_GDIR |= RST_BIT;
}

/* SCCB 16-bit register read: write [hi,lo]+STOP, then read one byte+STOP
 * (OV5640 wants two phases with a STOP between, not a repeated start). */
static uint8_t sccb_read16(uint16_t reg, bool *ok)
{
    uint8_t w[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    uint32_t we = lpi2c1176_master_write(LPI2C6, OV5640_ADDR, w, 2, 1);
    uint8_t v = 0;
    uint32_t n = lpi2c1176_master_read(LPI2C6, OV5640_ADDR, &v, 1, 1);
    if (ok) *ok = (we == 0 && n == 1);
    return v;
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("OV5640_ID_BEGIN");

    control_pins_init();
    lpi2c1176_begin(LPI2C6, &cam_i2c_hw, 100000);   /* 100 kHz SCCB */

    /* Power-up sequence (OV5640_Init): PWDN high + RST low, then release. */
    pwdn(true);  rst(false); delay(5);
    pwdn(false);             delay(1);
    rst(true);               delay(20);

    bool okH = false, okL = false;
    uint8_t idH = sccb_read16(0x300A, &okH);
    uint8_t idL = sccb_read16(0x300B, &okL);
    uint16_t id = ((uint16_t)idH << 8) | idL;

    Serial1.printf("SCCB_ACK=%s\n", (okH && okL) ? "PASS" : "FAIL");
    Serial1.printf("CHIP_ID=0x%04X (expect 0x5640)\n", id);
    Serial1.printf("OV5640_ID=%s\n", (id == 0x5640) ? "PASS" : "FAIL");
    Serial1.println("OV5640_ID_END");
}

void loop() {}
