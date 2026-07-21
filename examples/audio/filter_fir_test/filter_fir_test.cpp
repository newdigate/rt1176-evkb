#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"
#include "synth_sine.h"
#include "filter_fir.h"
#include "analyze_fft256.h"
#include "analyze_peak.h"
#include <math.h>

// sine -> FIR low-pass -> FFT256 (+peak tap). Passband tone must survive,
// stopband tone (at the boxcar's exact null) must vanish.

// No I/O node in this graph, so nothing calls the protected
// AudioStream::update_setup() that attaches+enables IRQ_SOFTWARE. Take the
// software-IRQ dispatch ourselves (same pattern as audiostream_test).
struct GraphClock : AudioStream {
    GraphClock() : AudioStream(0, NULL) { update_setup(); }
    void update(void) override {}
};
static GraphClock graph_clock;

static AudioSynthWaveformSine sine1;
static AudioFilterFIR         fir1;
static AudioAnalyzeFFT256     fft1;
static AudioAnalyzePeak       peak1;
static AudioConnection c1(sine1, 0, fir1, 0);
static AudioConnection c2(fir1, 0, fft1, 0);
static AudioConnection c3(fir1, 0, peak1, 0);

// 8-tap boxcar (each 0.125 in q15): exact null at fs/8 = 5512.5 Hz.
static const short lp_coeffs[8] = {4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096};

static void pump(int n) {
    for (int i = 0; i < n; i++) {
        NVIC_SET_PENDING(IRQ_SOFTWARE);   // stand in for the audio clock
        delayMicroseconds(200);
    }
}

// Set the tone, flush the filter+FFT averaging pipeline, return a fresh read.
static float measure(float freq, int bin) {
    sine1.frequency(freq);
    pump(48);                  // 24 FFTs -> 3 full 8-FFT averaging rounds settle
    (void)fft1.available();    // discard any stale output flag
    for (int i = 0; i < 400; i++) {
        pump(2);
        if (fft1.available()) return fft1.read(bin);
    }
    return -1.0f;              // graph never produced output -> hard fail
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    AudioMemory(30);
    sine1.amplitude(0.9f);
    fir1.begin(lp_coeffs, 8);

    float pb = measure(1033.59f, 6);    // bin 6
    float pk = peak1.available() ? peak1.read() : -1.0f;
    float sb = measure(5512.5f, 32);    // bin 32 = boxcar null

    Serial1.print("FIR: pb=");   Serial1.println(pb, 4);
    Serial1.print("FIR: peak="); Serial1.println(pk, 4);
    Serial1.print("FIR: sb=");   Serial1.println(sb, 4);
    float atten_db = (sb > 0.0001f && pb > 0.0f) ? 20.0f * log10f(sb / pb) : -60.0f;
    Serial1.print("FIR: atten_db="); Serial1.println(atten_db, 1);

    bool pass_pb = (pb > 0.30f);
    bool pass_sb = (sb >= 0.0f && sb < 0.04f);
    Serial1.println(pass_pb ? "STAGE_PB=PASS" : "STAGE_PB=FAIL");
    Serial1.println(pass_sb ? "STAGE_SB=PASS" : "STAGE_SB=FAIL");
    Serial1.println((pass_pb && pass_sb) ? "FIR_ALL=PASS" : "FIR_ALL=FAIL");
}
void loop() {}
