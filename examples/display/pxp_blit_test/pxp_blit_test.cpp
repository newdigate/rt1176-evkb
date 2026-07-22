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

    if (!PXP.begin()) {
        Serial1.println("PXP_BEGIN=FAIL");
        Serial1.println("PXP_ALL=FAIL");
        return;
    }
    Serial1.println("PXP_BEGIN=PASS");
    Serial1.println("PXP_ALL=PASS");
}

void loop() {}
