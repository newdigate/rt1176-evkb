/* rk055_panel_test - RT1176 -> RK055HDMIPI4MA0 720x1280 MIPI-DSI panel gate
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * M1 scope: the SoC chain only -- display clocks, LCDIFv2 scanout, MIPI-DSI
 * host.  The HX8394 panel driver (M2) and the test pattern + checksums (M3)
 * are added to this same gate as those milestones land.
 *
 * Every stage token is emitted UNCONDITIONALLY so the first FAIL pinpoints the
 * broken layer rather than the run simply stopping.
 *
 * Uses Serial1 (the LPUART console run_qemu.sh captures via `-serial file:`),
 * not Serial (native USB CDC, which QEMU would not capture here).
 */
#include <Arduino.h>
#include "Display.h"

void setup() {
    Serial1.begin(115200);
    delay(200);
    Serial1.println("RK055_PANEL_BEGIN");
    Serial1.printf("PANEL=%s\n", PANEL_NAME);
    Serial1.printf("GEOM=%ux%u PIXCLK=%lu LANES=%lu\n",
                   (unsigned)PANEL_WIDTH, (unsigned)PANEL_HEIGHT,
                   (unsigned long)PANEL_PIXEL_CLK_HZ,
                   (unsigned long)PANEL_DSI_LANES);

    Display.begin();
    Serial1.printf("CLK_%s\n",     Display.clkOk()   ? "OK" : "FAIL");
    Serial1.printf("LCDIFV2_%s\n", Display.lcdifOk() ? "OK" : "FAIL");
    Serial1.printf("DSI_%s\n",     Display.dsiOk()   ? "OK" : "FAIL");
    Serial1.printf("PANEL_%s\n", Display.panelOk() ? "OK" : "FAIL");

    Serial1.println("RK055_PANEL_END");
}

void loop() {}
