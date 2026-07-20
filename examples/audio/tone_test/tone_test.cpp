#include "Arduino.h"
#include "HardwareSerial.h"

#define TONE_PIN 9

static inline uint8_t tone_pin_level(void) {
    uint32_t base = digital_pin_to_info[TONE_PIN].gpio;   // GPIO port base
    uint8_t  bit  = digital_pin_to_info[TONE_PIN].bit;
    return (uint8_t)(((*(volatile uint32_t *)(base + 0x00u)) >> bit) & 1u);  // read DR
}

static uint32_t count_edges(uint32_t ms) {
    uint8_t last = tone_pin_level();
    uint32_t edges = 0, t0 = millis();
    while (millis() - t0 < ms) {
        uint8_t v = tone_pin_level();
        if (v != last) { edges++; last = v; }
    }
    return edges;
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    bool ok = true;

    // Check 1: frequency — tone 1000 Hz -> 2 edges/ms -> ~200 edges / 100 ms
    tone(TONE_PIN, 1000);
    uint32_t e1 = count_edges(100);
    if (e1 < 180 || e1 > 220) ok = false;

    // Check 4: frequency change — 2000 Hz -> ~2x
    tone(TONE_PIN, 2000);
    uint32_t e2 = count_edges(100);            // ~400
    if (e2 < 360 || e2 > 440) ok = false;

    // Check 3: noTone stops
    noTone(TONE_PIN);
    uint32_t e3 = count_edges(50);             // 0
    if (e3 != 0) ok = false;

    // Check 2: duration auto-stop — 1000 Hz for 100 ms
    tone(TONE_PIN, 1000, 100);
    uint32_t during = count_edges(50);         // ~100 during the tone
    delay(120);                                // let the 100 ms tone finish
    uint32_t after = count_edges(50);          // 0 after auto-stop
    if (during < 80 || after != 0) ok = false;

    Serial1.print("e1="); Serial1.print(e1);
    Serial1.print(" e2="); Serial1.print(e2);
    Serial1.print(" e3="); Serial1.print(e3);
    Serial1.print(" during="); Serial1.print(during);
    Serial1.print(" after="); Serial1.println(after);
    Serial1.println(ok ? "TONE=PASS" : "TONE=FAIL");
}
void loop() {}
