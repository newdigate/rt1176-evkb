#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"
#include "synth_sine.h"
#include "analyze_peak.h"
#include "output_i2s.h"
#include "control_wm8962.h"

// Task 3: AudioSynthWaveformSine -> AudioOutputI2S (SAI1 TX DMA) + a synth peak
// sanity. STAGE_SYNTH proves the source; STAGE_TONE is asserted host-side from
// the SAI1 TX tap file. QEMU is timer/tap-paced -> proves the graph->SAI-TX
// plumbing; the real 44.1 kHz rate + audibility are the HW item (Task 4).
AudioSynthWaveformSine sine;
AudioAnalyzePeak       peak;
AudioOutputI2S         out;
AudioConnection        pcPeak(sine, 0, peak, 0);   // sanity tap of the source
AudioConnection        pcL(sine, 0, out, 0);       // left  = sine
AudioConnection        pcR(sine, 0, out, 1);       // right = sine
AudioControlWM8962     wm;

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    AudioMemory(12);
    wm.enable();
    sine.frequency(1000.0f);
    sine.amplitude(0.5f);
    // out ctor auto-called begin(): SAI1 TX DMA is running; its isr pends
    // update_all() as the FIFO drains, so the graph self-clocks. Give it time.
    float pk = 0.0f;
    uint32_t t0 = millis();
    while (millis() - t0 < 500) {
        if (peak.available()) { float v = peak.read(); if (v > pk) pk = v; }
        yield();
    }
    bool synth_ok = pk > 0.40f && pk < 0.60f;
    Serial1.print("info synth_peak="); Serial1.println(pk, 4);
    Serial1.println(synth_ok ? "STAGE_SYNTH=PASS" : "STAGE_SYNTH=FAIL");
    // STAGE_TONE is decided host-side from the tap; emit a marker so the run
    // script knows the firmware reached steady state.
    Serial1.println("TONE_PLAYING");
}
void loop() {
    static uint32_t last = 0;
    if (millis() - last > 500) {
        last = millis();
        // Periodic health readout for the HW test: the sine also feeds `peak`,
        // and `peak` only advances when the graph runs (the TX DMA isr pends
        // update_all). ~0.5 here => the graph self-clocks on silicon => the same
        // sine is going out SAI1 TX -> WM8962 DAC -> J101. "(no update)" => the
        // TX DMA isn't driving the graph (a silicon-vs-QEMU divergence to chase).
        if (peak.available()) {
            Serial1.print("TONE_PLAYING synth_peak="); Serial1.println(peak.read(), 4);
        } else {
            Serial1.println("TONE_PLAYING synth_peak=(no update)");
        }
    }
}
