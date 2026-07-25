/* syspll3_bringup - RT1176 M3 checkpoint 1: bring up SYS_PLL3 @ 480 MHz
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * The MIPI-CSI2 clocks (Csi2/Csi2_Esc/Csi2_Ui = mux5 SysPll3Out / div8 = 60 MHz)
 * need SYS_PLL3 @ 480 MHz, but the boot ROM leaves SYS_PLL3 BYPASSED on this
 * board (startup.c:376; only SYS_PLL2 @ 528 is up).  This brings it up and
 * confirms lock (SYS_PLL3_STABLE) - the bounded first step of M3.
 *
 * Sequence = NXP CLOCK_InitSysPll3 (fsl_clock.c) minus two steps that don't
 * apply here: the PLL LDO is already enabled (SYS_PLL2 is running), and
 * HOLD_RING_OFF is "only needed for avpll" (SYS_PLL3 is a fixed integer PLL).
 * The PFDs are left alone - CSI2 uses SYS_PLL3_OUT (mux5), not a PFD.
 *
 * SYS_PLL3_CTRL @ ANADIG 0x40C84210 (RM 15.9.4.3):
 *   bit30 SYS_PLL3_GATE, bit29 SYS_PLL3_STABLE(RO), bit21 POWERUP,
 *   bit16 BYPASS, bit13 ENABLE_CLK, bit4 PLL_REG_EN, bit3 SYS_PLL3_DIV2.
 */
#include <Arduino.h>

#define SYS_PLL3_CTRL   (*(volatile uint32_t *)0x40C84210u)
#define PLL3_GATE       (1u << 30)
#define PLL3_STABLE     (1u << 29)
#define PLL3_POWERUP    (1u << 21)
#define PLL3_BYPASS     (1u << 16)
#define PLL3_ENABLE_CLK (1u << 13)
#define PLL3_PLL_REG_EN (1u << 4)
#define PLL3_DIV2       (1u << 3)

static void delay_us(uint32_t us) { delayMicroseconds(us); }

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("SYSPLL3_BEGIN");

    uint32_t before = SYS_PLL3_CTRL;
    Serial1.printf("SYS_PLL3_CTRL before=0x%08lX (POWERUP=%d STABLE=%d BYPASS=%d)\n",
                   (unsigned long)before, !!(before & PLL3_POWERUP),
                   !!(before & PLL3_STABLE), !!(before & PLL3_BYPASS));

    bool alreadyUp = (before & PLL3_POWERUP) != 0;
    if (alreadyUp) {
        SYS_PLL3_CTRL |= PLL3_ENABLE_CLK | PLL3_DIV2;
        SYS_PLL3_CTRL &= ~PLL3_GATE;
    } else {
        /* 1. Enable PLL regulator, keep clock gated. */
        SYS_PLL3_CTRL |= PLL3_PLL_REG_EN | PLL3_GATE;
        delay_us(30);
        /* 2. Power up. */
        SYS_PLL3_CTRL |= PLL3_POWERUP;
        delay_us(30);
        /* 3. Wait for lock. */
        uint32_t t = 0;
        while (!(SYS_PLL3_CTRL & PLL3_STABLE)) {
            if (++t > 1000000u) break;   /* ~timeout guard */
        }
        /* 4. Enable clock output + DIV2, ungate. */
        SYS_PLL3_CTRL |= PLL3_ENABLE_CLK | PLL3_DIV2;
        SYS_PLL3_CTRL &= ~PLL3_GATE;
    }

    uint32_t after = SYS_PLL3_CTRL;
    bool stable = (after & PLL3_STABLE) != 0;
    bool up     = (after & PLL3_POWERUP) != 0;
    bool ungated = (after & PLL3_GATE) == 0;
    bool clken  = (after & PLL3_ENABLE_CLK) != 0;
    Serial1.printf("SYS_PLL3_CTRL after=0x%08lX (POWERUP=%d STABLE=%d GATE=%d ENABLE_CLK=%d)\n",
                   (unsigned long)after, up, stable, !!(after & PLL3_GATE), clken);

    bool ok = stable && up && ungated && clken;
    Serial1.printf("SYSPLL3_480=%s\n", ok ? "PASS" : "FAIL");
    Serial1.println("SYSPLL3_END");
}

void loop() {}
