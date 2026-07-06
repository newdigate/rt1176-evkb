#include <Arduino.h>
#include "HardwareSerial.h"
#include "I2S.h"
#include "core_pins.h"

static int16_t expect_sine[96 * 2];
static int16_t got[96 * 2];
static DMAMEM int16_t rx_ring[96 * 2];
static DMAMEM int16_t tx_ring[96 * 2];
static void build_sine() {
    for (int i = 0; i < 96; i++) {
        double ph = 2.0 * 3.14159265358979 * (i % 48) / 48.0;
        int16_t v = (int16_t)(0x6000 * __builtin_sin(ph));
        expect_sine[2*i + 0] = v;
        expect_sine[2*i + 1] = (int16_t)(v / 2);
    }
}

void setup() {
    Serial1.begin(115200);
    delay(50);
    build_sine();
    I2S.begin(48000);
    I2S.read(got, 96);
    bool ok = true;
    for (int i = 0; i < 96 * 2; i++) if (got[i] != expect_sine[i]) ok = false;
    Serial1.println(ok ? "STAGE_A_PASS" : "STAGE_A_FAIL");
    I2S.beginReceiveDMA(rx_ring, 96);
    Serial1.println("STAGE_B_DONE");
    for (int i = 0; i < 96 * 2; i++) tx_ring[i] = expect_sine[i];   // same sine out the TX
    I2S.beginDMA(tx_ring, 96);       // full-duplex: TX + RX now both DMA-driven
    Serial1.println("STAGE_C_DONE");
}
void loop() {
    static bool bdone = false, fdone = false;
    if (!bdone && I2S.rxBlockCount() >= 2) {
        bool ok = true;
        for (int i = 0; i < 96 * 2; i++) if (rx_ring[i] != expect_sine[i]) ok = false;
        Serial1.println(ok ? "STAGE_B_PASS" : "STAGE_B_FAIL");
        bdone = true;
    }
    // RX DMA started ~2 blocks earlier (Stage B) than TX DMA (Stage C), so
    // by the time TX has 2 blocks, RX (running the whole time) should have >=4.
    if (bdone && !fdone && I2S.dmaBlockCount() >= 2 && I2S.rxBlockCount() >= 4) {
        Serial1.println("STAGE_FD_PASS");     // TX DMA + RX DMA both advancing
        fdone = true;
    }
    // HW: TX plays the 1 kHz sine (tx_ring) out the codec; the mic captures it.
    // Report the left-channel energy at ~1 kHz and the peak amplitude so the
    // human can see the tone appear (and rise when the mic hears the speaker).
    static uint32_t last = 0;
    if (fdone && (millis() - last) > 500) {
        last = millis();
        const double w = 2.0 * 3.14159265358979 * 1000.0 / 48000.0;
        const double c = 2.0 * __builtin_cos(w);
        double s1 = 0, s2 = 0; int16_t peak = 0;
        for (int i = 0; i < 96; i++) {                 // left channel of the RX ring
            double x = rx_ring[2*i];
            double s0 = x + c * s1 - s2; s2 = s1; s1 = s0;
            int16_t a = rx_ring[2*i] < 0 ? -rx_ring[2*i] : rx_ring[2*i];
            if (a > peak) peak = a;
        }
        double mag = s1*s1 + s2*s2 - c*s1*s2;          // Goertzel power at 1 kHz
        Serial1.print("MIC peak="); Serial1.print(peak);
        Serial1.print(" e1k="); Serial1.println((uint32_t)(mag / 1e6));
    }
}
