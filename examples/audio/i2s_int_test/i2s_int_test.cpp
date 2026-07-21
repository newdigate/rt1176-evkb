// i2s_int_test: CM7 proof of the interrupt-driven SAI nodes (Plan 2 Task 1).
// Sine 1 kHz -> AudioOutputI2SInt (SAI1 TX FIFO-request interrupt, NO DMA,
// audible on J101) and AudioInputI2SInt -> AudioAnalyzePeak (EVKB onboard
// mic = RIGHT channel via WM8962 Input3). The SAI ISR IS the graph clock:
// every 128 frames it pends IRQ_SOFTWARE (dispatch), mirroring the DMA
// node's cadence (~344/s at 44.1 kHz).
//
// Phasing (the QEMU world-split dance -- see output_i2s_int.h):
//  A. out.begin() with pauseAfter=DISPATCH_TARGET: the ISR free-runs the
//     graph. In QEMU the TX FIFO-request level never deasserts (no audio
//     backend), so the main thread is INTENTIONALLY starved until the ISR
//     self-disarms at the target -- every measurable below is accumulated
//     by the ISR/graph alone.
//  B. Codec bring-up (WM8962 over Wire): MCLK/BCLK keep running (TE|BCE
//     stay set; only FRIE was cleared).
//  C. in.begin() arms RX (synchronous to TX). QEMU: no RX data unless
//     injected; mic assertions are HW-only (world split).
//  D. Print the measurables + verdict + DONE, THEN resume() for the
//     continuous audible tone; in QEMU nothing runs after resume()
//     (documented), on HW loop() reports the mic peak.
//
// QEMU asserts: dispatches >= target, underruns == 0, fef == 0, codec ACK.
// HW adds: audible 1 kHz on J101 (human) + MIC=PASS (peak > threshold).

#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"
#include "synth_sine.h"
#include "analyze_peak.h"
#include "output_i2s_int.h"
#include "input_i2s_int.h"
#include "control_wm8962.h"

#define DISPATCH_TARGET 600u   // ~1.74 s of audio at 344 dispatches/s

AudioSynthWaveformSine sine1;
AudioOutputI2SInt      out;
AudioInputI2SInt       in;
AudioAnalyzePeak       synthpeak;   // taps the source (graph-health proof)
AudioAnalyzePeak       micpeak;     // taps the mic (HW-only assertion)
AudioConnection        c1(sine1, 0, out, 0);       // left  = sine
AudioConnection        c2(sine1, 0, out, 1);       // right = sine
AudioConnection        c3(sine1, 0, synthpeak, 0);
AudioConnection        c4(in, 1, micpeak, 0);      // mic is the RIGHT channel
AudioControlWM8962     wm;

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    Serial1.println("I2SINT-GATE v1");

    AudioMemory(16);
    sine1.frequency(1000.0f);
    sine1.amplitude(0.5f);

    // Phase A: interrupt-clocked measurement window.
    AudioOutputI2SInt::setPauseAfter(DISPATCH_TARGET);
    out.begin();
    uint32_t t0 = millis();
    while (AudioOutputI2SInt::dispatchCount() < DISPATCH_TARGET &&
           millis() - t0 < 8000) { }
    uint32_t dispatches = AudioOutputI2SInt::dispatchCount();
    uint32_t isrs       = sai1176_isr_count;
    uint32_t underruns  = AudioOutputI2SInt::underrunCount();
    uint32_t fef        = AudioOutputI2SInt::fef() ? 1u : 0u;
    // synthpeak was updated by the ISR-clocked graph during phase A; reading
    // it now proves the dispatches really ran update_all end-to-end.
    float synth_pk = synthpeak.available() ? synthpeak.read() : -1.0f;

    // Phase B: codec (clocks still running; only FRIE is disarmed).
    bool codec_ok = wm.enable();

    // Phase C: mic capture path armed (rides the same SAI1 ISR).
    in.begin();

    Serial1.print("info dispatches=");  Serial1.println(dispatches);
    Serial1.print("info isr_count=");   Serial1.println(isrs);
    Serial1.print("info underruns=");   Serial1.println(underruns);
    Serial1.print("info fef=");         Serial1.println(fef);
    Serial1.print("info codec_ack=");   Serial1.println(codec_ok ? 1 : 0);
    Serial1.print("info synth_peak=");  Serial1.println(synth_pk, 4);

    bool synth_ok = synth_pk > 0.40f && synth_pk < 0.60f;
    bool pass = (dispatches >= DISPATCH_TARGET) && (underruns == 0u) &&
                (fef == 0u) && codec_ok && synth_ok;
    Serial1.println(pass ? "I2SINT=PASS" : "I2SINT=FAIL");
    Serial1.println("HUMAN: listen for 1 kHz on J101");
    Serial1.println("I2SINT-DONE");

    // Phase D: continuous tone + mic capture (HW). In QEMU the main thread
    // parks here forever (TX FRF level; world-split, documented in the
    // runner) -- everything above is already on the wire.
    AudioOutputI2SInt::setPauseAfter(0);
    AudioOutputI2SInt::resume();
}

void loop() {
    // HW-only in practice: report the mic peak (RIGHT channel = onboard
    // mics). MIC=PASS requires a real acoustic signal above the noise
    // floor threshold; the 1 kHz tone leaking from the room/headphones or
    // ambient sound is enough.
    static uint32_t last = 0;
    if (millis() - last > 500) {
        last = millis();
        if (micpeak.available()) {
            float mp = micpeak.read();
            Serial1.print("MIC mic_peak="); Serial1.print(mp, 4);
            Serial1.print(" rx_frames=");   Serial1.print(AudioInputI2SInt::frameCount());
            Serial1.println(mp > 0.001f ? " MIC=PASS" : " MIC=low");
        } else {
            Serial1.println("MIC mic_peak=(no update)");
        }
    }
}
