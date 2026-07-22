#include "Arduino.h"
#include "HardwareSerial.h"
#include <Audio.h>
#include <math.h>

// Proves the FULL Audio library aggregates: every type below comes only from
// <Audio.h>. Chain: sine -> FIR low-pass -> mixer -> FFT256 (+peak tap).
static AudioSynthWaveformSine sine1;
static AudioFilterFIR         fir1;
static AudioMixer4            mix1;
static AudioAnalyzeFFT256     fft1;
static AudioAnalyzePeak       peak1;
static AudioConnection c1(sine1, 0, fir1, 0);
static AudioConnection c2(fir1, 0, mix1, 0);
static AudioConnection c3(mix1, 0, fft1, 0);
static AudioConnection c4(mix1, 0, peak1, 0);

// No I/O node in this graph -> nothing calls the protected update_setup() that
// arms IRQ_SOFTWARE. Same GraphClock pattern as filter_fir_test/audiostream_test.
struct GraphClock : public AudioStream {
    GraphClock() : AudioStream(0, NULL) { update_setup(); }
    void update(void) override {}
};
static GraphClock clock1;

static const short lp_coeffs[8] = {4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096};

static void pump(int n) {
    for (int i = 0; i < n; i++) { NVIC_SET_PENDING(IRQ_SOFTWARE); delayMicroseconds(200); }
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    Serial1.println("AUDIOH-GATE v1");
    AudioMemory(30);
    sine1.amplitude(0.9f);
    sine1.frequency(1033.59375f);   // exactly bin 6 (6*44100/256)
    mix1.gain(0, 1.0f);
    fir1.begin(lp_coeffs, 8);

    pump(48);
    (void)fft1.available();
    float pb = -1.0f;
    for (int i = 0; i < 400; i++) { pump(2); if (fft1.available()) { pb = fft1.read(6); break; } }
    float pk = peak1.available() ? peak1.read() : -1.0f;

    Serial1.print("AUDIOH: fft_bin6="); Serial1.println(pb, 4);
    Serial1.print("AUDIOH: peak=");     Serial1.println(pk, 4);
    bool pass = (pb > 0.30f && pb < 0.55f) && (pk > 0.7f && pk < 0.95f);
    Serial1.println(pass ? "AUDIOH_CHAIN=PASS" : "AUDIOH_CHAIN=FAIL");
    Serial1.println("AUDIOH-DONE");
}
void loop() {}
