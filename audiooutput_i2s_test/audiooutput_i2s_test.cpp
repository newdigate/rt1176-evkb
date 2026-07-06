#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"
#include "synth_sine.h"
#include "analyze_peak.h"

// Task 1: prove AudioSynthWaveformSine compiles + runs on RT1176 (dspinst.h
// __ARM_ARCH_7EM__ path) by measuring a full-scale sine's peak. No peripheral.
//
// AudioStream::update_setup() (attachInterruptVector + NVIC_ENABLE_IRQ for
// IRQ_SOFTWARE) is normally called by a real I/O node's begin() -- e.g.
// AudioInputI2S/AudioOutputI2S -- which "takes responsibility" for the block
// dispatch clock. This gate has no I/O node yet (Task 3), so nothing enables
// IRQ_SOFTWARE and NVIC_SET_PENDING() alone never actually runs update_all().
// update_setup() is protected on AudioStream, so a minimal local subclass
// (same pattern as audiostream_test's TestSource) takes that responsibility
// here, standing in for the future AudioOutputI2S.
class DispatchEnabler : public AudioStream {
public:
    DispatchEnabler() : AudioStream(0, NULL) { update_setup(); }
    void update(void) override {}
};

AudioSynthWaveformSine sine;
AudioAnalyzePeak       peak;
AudioConnection        patchCord(sine, 0, peak, 0);
DispatchEnabler        dispatchEnabler;

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    AudioMemory(12);
    sine.frequency(1000.0f);
    sine.amplitude(0.5f);
    float pk = 0.0f;
    for (int i = 0; i < 200; i++) {
        NVIC_SET_PENDING(IRQ_SOFTWARE);
        for (volatile uint32_t d = 20000; d; d--) { }
        if (peak.available()) { float v = peak.read(); if (v > pk) pk = v; }
    }
    bool ok = pk > 0.40f && pk < 0.60f;   // full-scale sine @ amplitude 0.5
    Serial1.print("info synth_peak="); Serial1.println(pk, 4);
    Serial1.println(ok ? "STAGE_SYNTH=PASS" : "STAGE_SYNTH=FAIL");
    Serial1.println(ok ? "AUDIOOUTPUT_ALL=PASS" : "AUDIOOUTPUT_ALL=FAIL");
}
void loop() {}
