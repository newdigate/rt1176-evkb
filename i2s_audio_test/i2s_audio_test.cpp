#include <Arduino.h>
#include "HardwareSerial.h"
#include "I2S.h"

// 1 kHz @ 48 kHz = 48 samples/cycle. L = full sine, R = half amplitude
// (distinct so a channel swap is detectable). 2 cycles = 96 frames.
static int16_t g_sine[96 * 2];
static void build_sine() {
    for (int i = 0; i < 96; i++) {
        double ph = 2.0 * 3.14159265358979 * (i % 48) / 48.0;
        int16_t v = (int16_t)(0x6000 * __builtin_sin(ph));
        g_sine[2*i + 0] = v;                 // L
        g_sine[2*i + 1] = (int16_t)(v / 2);  // R
    }
}

void setup() {
    Serial1.begin(115200);
    delay(50);
    I2S.begin(48000);
    // Stage A: verify SAI1 configured as 48k/16-bit I2S master.
    uint32_t tcsr = SAI1_TCSR, tcr2 = SAI1_TCR2, tcr4 = SAI1_TCR4, tcr5 = SAI1_TCR5;
    bool te   = (tcsr & SAI_TCSR_TE) != 0;
    bool div7 = (tcr2 & 0xFFu) == 7;
    bool mclk1= ((tcr2 >> 26) & 0x3u) == 1;
    bool i2s4 = (tcr4 & SAI_TCR4_MF) && (tcr4 & SAI_TCR4_FSD) &&
                (tcr4 & SAI_TCR4_FSE) && (((tcr4 >> 16) & 0x1Fu) == 1);
    bool w16  = (((tcr5 >> 16) & 0x1Fu) == 15) && (((tcr5 >> 24) & 0x1Fu) == 15);
    Serial1.print("STAGE_A tcsr="); Serial1.print(tcsr, HEX);
    Serial1.print(" tcr2="); Serial1.print(tcr2, HEX);
    Serial1.print(" tcr4="); Serial1.print(tcr4, HEX);
    Serial1.print(" tcr5="); Serial1.println(tcr5, HEX);
    if (te && div7 && mclk1 && i2s4 && w16) Serial1.println("STAGE_A_PASS");
    else Serial1.println("STAGE_A_FAIL");

    build_sine();
    I2S.write(g_sine, 96);
    Serial1.println("STAGE_B_DONE");   // tap capture is checked host-side
}
void loop() {}
