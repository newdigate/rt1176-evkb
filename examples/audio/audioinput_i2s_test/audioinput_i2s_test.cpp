#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"
#include "input_i2s.h"
#include "analyze_peak.h"
#include "control_wm8962.h"

// Task 4 integration gate: SAI injector -> AudioInputI2S (SAI1-RX DMA capture)
// -> AudioAnalyzePeak. Proves the capture->graph->peak plumbing; QEMU is
// injector/timer-paced so the 44.1 kHz clock rate itself is a HW item (Task 5).
AudioInputI2S      in;
AudioAnalyzePeak   peak;
// RIGHT channel (channel 1): AudioInputI2S::isr deinterleaves even samples ->
// left (ch 0), odd samples -> right (ch 1); the on-board mic is wired to the
// right channel (WM8962 Input3), and the SAI RX work's injector convention
// puts the primary signal on the right channel to match.
AudioConnection    patchCord(in, 1, peak, 0);
AudioControlWM8962 wm;

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    AudioMemory(24);
    wm.enable();
    // AudioInputI2S ctor auto-calls begin()
    float pk = 0.0f;
    uint32_t t0 = millis();
    while (millis() - t0 < 500) {
        if (peak.available()) { float v = peak.read(); if (v > pk) pk = v; }
        yield();
    }
    bool ok = pk > 0.02f;
    Serial1.print("info peak="); Serial1.println(pk, 4);
    Serial1.println(ok ? "STAGE_PEAK=PASS" : "STAGE_PEAK=FAIL");
    Serial1.println(ok ? "AUDIOINPUT_ALL=PASS" : "AUDIOINPUT_ALL=FAIL");
}
// Periodic peak report for the HW mic test: every ~500 ms print the peak over
// that window (analyze_peak accumulates until read()). Gated on available() so
// a dataless read() (which would return ~1.0 from the reset min/max) is skipped.
// In QEMU the injector keeps this at ~0.7500; on silicon it tracks the mic
// (low when quiet, rising on sound near the on-board mic / WM8962 Input3).
void loop() {
    static uint32_t last = 0;
    if (millis() - last > 500) {
        last = millis();
        if (peak.available()) {
            Serial1.print("MIC peak="); Serial1.println(peak.read(), 4);
        } else {
            Serial1.println("MIC peak=(no blocks)");
        }
    }
}
