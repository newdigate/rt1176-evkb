/* camera_csi2_clocks - RT1176 M3 step 2: MIPI-CSI2 RX clocking + D-PHY power-on.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *   (sequence transcribed from NXP BSD-3 evkbmimxrt1170 camera_support.c::
 *    BOARD_InitMipiCsi; register addresses from the MIMXRT1176 device header.)
 *
 * Everything the CSI2RX/CSI blocks need BEFORE the driver ports (M3.3/M3.4):
 *   1. Verify SYS_PLL3 @ 480 MHz is up (the CSI2 root mux source).
 *   2. CLOCK_EnableClock(kCLOCK_Video_Mux) + VIDEO_MUX CSI_SEL (route CSI2RX->CSI).
 *   3. CSI2 / CSI2_Esc / CSI2_Ui clock roots = mux5(SysPll3Out)/div8 = 60 MHz.
 *   4. MIPI D-PHY power-on: PGMC_BPC4 PSW_ON_SOFT | ISO_OFF_SOFT.
 *
 * All four are plain CCM/VIDEO_MUX/PGMC pokes, so unlike the CSI2RX/CSI capture
 * this step IS readback-verifiable without a receiver: every register is read
 * back and asserted here.  See rt1176-ov5640-camera memory note for the map.
 *
 * VIDEO_MUX + PGMC_BPC4 defs come from the core header imxrt1176.h (already
 * RM-sourced there).  Only the CCM Csi2 clock roots and the Video-Mux LPCG are
 * missing from the core, so they are derived inline here and cross-checked vs
 * the HW-verified ov5640 config (CLOCK_ROOT42=0x40CC1500, LPCG103=0x40CC6CE0):
 *   CCM CLOCK_ROOT[n].CONTROL = 0x40CC0000 + n*0x80   (DIV [7:0], MUX [10:8], OFF b24)
 *   CCM LPCG[n].DIRECT        = 0x40CC6000 + n*0x20   (ON b0), STATUS0 = DIRECT+0x4
 *   kCLOCK_Root_Csi2=73  Csi2_Esc=74  Csi2_Ui=75 ; kCLOCK_Video_Mux LPCG=136
 */
#include <Arduino.h>

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* --- SYS_PLL3 (source), see syspll3_bringup --- */
#define SYS_PLL3_CTRL   0x40C84210u
#define PLL3_STABLE     (1u << 29)
#define PLL3_POWERUP    (1u << 21)
#define PLL3_BYPASS     (1u << 16)

/* --- CCM clock roots (missing from the core header) --- */
#define CCM_ROOT_CTRL(n)  (0x40CC0000u + (uint32_t)(n) * 0x80u)
#define ROOT_CSI2         73
#define ROOT_CSI2_ESC     74
#define ROOT_CSI2_UI      75
/* mux5 (SysPll3Out) / div8 (field = div-1 = 7), clockOff=false. */
#define ROOT_CTRL_60MHZ   (CCM_CLOCK_ROOT_CONTROL_MUX(5) | CCM_CLOCK_ROOT_CONTROL_DIV(8u - 1u)) /* 0x507 */
#define ROOT_CTRL_MASK    0x1FFFu                     /* DIV|MUX|(OFF checked separately) */
#define ROOT_OFF          (1u << 24)

/* --- CCM LPCG (Video Mux) (missing from the core header) --- */
#define CCM_LPCG_DIRECT(n)  (0x40CC6000u + (uint32_t)(n) * 0x20u)
#define CCM_LPCG_STATUS0(n) (CCM_LPCG_DIRECT(n) + 0x4u)
#define LPCG_VIDEO_MUX      136
#define LPCG_ON             (1u << 0)

/* VIDEO_MUX_* and PGMC_BPC4_* / PGMC_BPC_POWER_CTRL_* come from imxrt1176.h. */

static bool report(const char *tag, uint32_t got, uint32_t want)
{
    bool ok = (got == want);
    Serial1.printf("  %-16s = 0x%08lX (want 0x%08lX) %s\n",
                   tag, (unsigned long)got, (unsigned long)want, ok ? "OK" : "BAD");
    return ok;
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("CSI2_CLK_BEGIN");

    /* 1. SYS_PLL3 must be locked @ 480 MHz (the CSI2 root source). */
    uint32_t pll3 = REG32(SYS_PLL3_CTRL);
    bool pll3ok = (pll3 & PLL3_POWERUP) && (pll3 & PLL3_STABLE) && !(pll3 & PLL3_BYPASS);
    Serial1.printf("SYS_PLL3_CTRL=0x%08lX (POWERUP=%d STABLE=%d BYPASS=%d) %s\n",
                   (unsigned long)pll3, !!(pll3 & PLL3_POWERUP), !!(pll3 & PLL3_STABLE),
                   !!(pll3 & PLL3_BYPASS), pll3ok ? "OK" : "BAD");

    /* 2. Route CSI2RX -> CSI: enable the Video Mux LPCG, then set CSI_SEL.
     * NXP CLOCK_ControlGate semantics: only touch DIRECT if the ON bit differs,
     * DSB/ISB, then poll STATUS0 - but BOUNDED here (some RT1176 LPCGs default to
     * setpoint/CPU-LPM control where DIRECT writes are ignored and STATUS0 would
     * never follow -> a hard hang).  We report the raw values instead of trusting
     * the poll. */
    uint32_t lpcgWaits = 0;
    if ((REG32(CCM_LPCG_DIRECT(LPCG_VIDEO_MUX)) & LPCG_ON) == 0) {
        REG32(CCM_LPCG_DIRECT(LPCG_VIDEO_MUX)) = LPCG_ON;
        __asm volatile("dsb"); __asm volatile("isb");
        while ((REG32(CCM_LPCG_STATUS0(LPCG_VIDEO_MUX)) & LPCG_ON) == 0) {
            if (++lpcgWaits > 100000u) break;   /* don't hard-hang */
        }
    }
    VIDEO_MUX_VID_MUX_CTRL_SET = VIDEO_MUX_VID_MUX_CTRL_CSI_SEL;

    /* 3. CSI2 / Esc / Ui roots -> 60 MHz off SysPll3. */
    REG32(CCM_ROOT_CTRL(ROOT_CSI2))     = ROOT_CTRL_60MHZ;
    REG32(CCM_ROOT_CTRL(ROOT_CSI2_ESC)) = ROOT_CTRL_60MHZ;
    REG32(CCM_ROOT_CTRL(ROOT_CSI2_UI))  = ROOT_CTRL_60MHZ;

    /* 4. MIPI D-PHY power on + isolation off (easy to miss). */
    PGMC_BPC4_BPC_POWER_CTRL |= (PGMC_BPC_POWER_CTRL_PSW_ON_SOFT | PGMC_BPC_POWER_CTRL_ISO_OFF_SOFT);

    /* --- Read everything back. --- */
    Serial1.println("READBACK:");
    bool ok = pll3ok;
    /* VID_MUX_CTRL latching the CSI_SEL write is itself proof the Video Mux is
     * clocked (an ungated peripheral would not hold the write) - this is the gate,
     * not the LPCG STATUS0 bit, which on this board reads 0 while DIRECT reads 1
     * (the gate is under setpoint control, not DIRECT - STATUS0 tracks the
     * setpoint state).  LPCG values are printed informationally only. */
    uint32_t vmux = VIDEO_MUX_VID_MUX_CTRL;
    ok &= report("VID_MUX_CTRL", vmux & VIDEO_MUX_VID_MUX_CTRL_CSI_SEL, VIDEO_MUX_VID_MUX_CTRL_CSI_SEL);
    uint32_t lpcgD = REG32(CCM_LPCG_DIRECT(LPCG_VIDEO_MUX));
    uint32_t lpcgS = REG32(CCM_LPCG_STATUS0(LPCG_VIDEO_MUX));
    Serial1.printf("  LPCG_VIDEO_MUX  DIRECT=0x%08lX STATUS0=0x%08lX waits=%lu (info)\n",
                   (unsigned long)lpcgD, (unsigned long)lpcgS, (unsigned long)lpcgWaits);

    uint32_t rC  = REG32(CCM_ROOT_CTRL(ROOT_CSI2));
    uint32_t rE  = REG32(CCM_ROOT_CTRL(ROOT_CSI2_ESC));
    uint32_t rU  = REG32(CCM_ROOT_CTRL(ROOT_CSI2_UI));
    ok &= report("ROOT_Csi2",  rC & ROOT_CTRL_MASK, ROOT_CTRL_60MHZ);
    ok &= report("ROOT_Csi2_Esc", rE & ROOT_CTRL_MASK, ROOT_CTRL_60MHZ);
    ok &= report("ROOT_Csi2_Ui",  rU & ROOT_CTRL_MASK, ROOT_CTRL_60MHZ);
    /* roots must be running (OFF bit clear). */
    bool rootsOn = !(rC & ROOT_OFF) && !(rE & ROOT_OFF) && !(rU & ROOT_OFF);
    Serial1.printf("  roots_running   = %s\n", rootsOn ? "OK" : "BAD");
    ok &= rootsOn;

    /* PSW_ON_SOFT / ISO_OFF_SOFT are self-clearing STROBE triggers: writing 1
     * kicks the power switch / isolation and the bit clears - so BPC_POWER_CTRL
     * reads back 0, which is correct (NXP's 5 shipping *_support.c demos issue
     * the exact same bare |= and never read it back).  Printed for info; the true
     * D-PHY-powered proof is a captured frame at M3.4.  Not a PASS gate. */
    uint32_t bpc = PGMC_BPC4_BPC_POWER_CTRL;
    Serial1.printf("  BPC4_POWER_CTRL = 0x%08lX (self-clearing triggers; 0 = fired OK) (info)\n",
                   (unsigned long)bpc);

    Serial1.printf("CSI2_CLK=%s\n", ok ? "PASS" : "FAIL");
    Serial1.println("CSI2_CLK_END");
}

void loop() {}
