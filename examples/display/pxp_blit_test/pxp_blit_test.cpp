/* pxp_blit_test - RT1176 PXP Phase 1 gate
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 */
#include <Arduino.h>
#include <PXP.h>

void setup() {
    Serial1.begin(115200);
    delay(200);
    Serial1.println("PXP_GATE_START");

    /* Emit CTRL unconditionally, BEFORE branching, so the token is a live
     * observation on every run - clean success, begin()'s self-check tripping,
     * or dirty readback bits alike - and a QEMU-vs-HW datapoint for Task 11.
     * After the RM 52.5 sequence, SFTRST and CLKGATE must both be clear. */
    bool ok = PXP.begin();
    uint32_t ctrl = PXP_CTRL;
    Serial1.print("PXP_CTRL_POST_BEGIN=0x"); Serial1.println(ctrl, HEX);
    if (!ok || (ctrl & (PXP_CTRL_SFTRST | PXP_CTRL_CLKGATE))) {
        Serial1.println("PXP_BEGIN=FAIL");
        Serial1.println("PXP_ALL=FAIL");
        return;
    }
    Serial1.println("PXP_BEGIN=PASS");
    Serial1.println("PXP_ALL=PASS");
}

void loop() {}
