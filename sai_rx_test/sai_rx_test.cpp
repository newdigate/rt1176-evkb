#include <Arduino.h>
#include "HardwareSerial.h"
#include "I2S.h"

static int16_t expect_sine[96 * 2];
static int16_t got[96 * 2];
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
}
void loop() { }
