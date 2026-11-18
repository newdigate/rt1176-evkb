/* Line-out -> line-in loopback recording on the Nosh board (bench only).
 *
 * Setup: a 3.5 mm stereo cable from line out (J8) to line in (J9).
 *
 * The SGTL5000 plays 1000 Hz on the left and 1500 Hz on the right, both at
 * amplitude 0.5 (-6.0 dBFS peak), so a swapped, shorted or open channel shows.
 * AudioInputI2S records 1 s of each channel into SDRAM, then the tones are
 * muted and 0.5 s more is recorded for the noise floor. Each channel is then
 * measured on the board with a Hann-windowed Goertzel:
 *
 *   freq   zero-crossing frequency, within 1 % of the tone
 *   level  own tone, -9 .. -4 dBFS peak (sent -6.0)
 *   thd    H2..H5 relative to the tone, below -50 dB
 *   xtalk  the other channel's tone, below -50 dB
 *   floor  muted RMS, below -60 dBFS
 *   rate   1 s of blocks in 950 .. 1050 ms (the recording keeps real time)
 *
 * Without the cable the level check fails, so a PASS needs the real analogue
 * path: DAC, line out, J8, cable, J9, line in, ADC, SAI1 RX.
 *
 * The recordings stay in SDRAM (rec_tone_L/R, rec_quiet_L/R, int16, 44.1 kHz)
 * once rec_done = 1; analyze_swd.py reads them over SWD for an FFT.
 *
 * Console: LPUART1 = Serial1 = J11, 115200. Bench result: transcript_hw_nosh.txt.
 */
#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"
#include "input_i2s.h"
#include "output_i2s.h"
#include "synth_sine.h"
#include "record_queue.h"
#include "control_sgtl5000.h"
#include <math.h>

#if !defined(ARDUINO_NOSH)
#error "line_loopback_hw is for the Nosh board (SGTL5000 line out J8 -> line in J9)"
#endif

#define CONSOLE Serial1

AudioSynthWaveformSine toneL, toneR;
AudioOutputI2S         out;
AudioInputI2S          in;
AudioRecordQueue       recL, recR;
AudioConnection        c1(toneL, 0, out, 0), c2(toneR, 0, out, 1);
AudioConnection        c3(in, 0, recL, 0),   c4(in, 1, recR, 0);
AudioControlSGTL5000   codec;

static const float FREQ_L = 1000.0f, FREQ_R = 1500.0f, AMPL = 0.5f;
#define TONE_BLOCKS  344   // 344 * 128 = 44032 samples, ~1.0 s
#define QUIET_BLOCKS 172
#define TONE_N  (TONE_BLOCKS * AUDIO_BLOCK_SAMPLES)
#define QUIET_N (QUIET_BLOCKS * AUDIO_BLOCK_SAMPLES)

// Unmangled so analyze_swd.py can find them with nm.
extern "C" {
EXTMEM int16_t rec_tone_L[TONE_N];
EXTMEM int16_t rec_tone_R[TONE_N];
EXTMEM int16_t rec_quiet_L[QUIET_N];
EXTMEM int16_t rec_quiet_R[QUIET_N];
volatile uint32_t rec_done = 0;
}

static bool all_ok = true;

static void check(const char *what, bool ok) {
    CONSOLE.print("STAGE_"); CONSOLE.print(what); CONSOLE.println(ok ? "=PASS" : "=FAIL");
    all_ok = all_ok && ok;
}

// Record `blocks` blocks of each channel; returns the elapsed ms, or -1 on a timeout.
static long record(int16_t *L, int16_t *R, int blocks) {
    recL.clear(); recR.clear();
    recL.begin(); recR.begin();
    int nl = 0, nr = 0;
    const uint32_t t0 = millis();
    while ((nl < blocks || nr < blocks) && millis() - t0 < 5000) {
        if (recL.available()) {
            if (nl < blocks) memcpy(L + AUDIO_BLOCK_SAMPLES * nl++, recL.readBuffer(), AUDIO_BLOCK_SAMPLES * 2);
            recL.freeBuffer();
        }
        if (recR.available()) {
            if (nr < blocks) memcpy(R + AUDIO_BLOCK_SAMPLES * nr++, recR.readBuffer(), AUDIO_BLOCK_SAMPLES * 2);
            recR.freeBuffer();
        }
    }
    recL.end(); recR.end();
    const long ms = (long)(millis() - t0);
    CONSOLE.printf("info record blocks L=%d R=%d of %d in %ld ms\n", nl, nr, blocks, ms);
    return (nl == blocks && nr == blocks) ? ms : -1;
}

static float mean_of(const int16_t *x, int n) {
    double s = 0; for (int i = 0; i < n; i++) s += x[i];
    return (float)(s / n);
}

// Hann-windowed Goertzel: peak amplitude of a sine at f, full scale = 1.0.
// The window keeps the main tone's leakage out of the harmonic and crosstalk
// bins (a rectangular window limits both to about -65 dB here).
static float tone_amp(const int16_t *x, int n, float mean, float f) {
    const float w = 2.0f * (float)M_PI * f / AUDIO_SAMPLE_RATE_EXACT, c = 2.0f * cosf(w);
    const float k = 2.0f * (float)M_PI / (n - 1);
    float s1 = 0, s2 = 0, wsum = 0;
    for (int i = 0; i < n; i++) {
        const float win = 0.5f - 0.5f * cosf(k * i);
        const float s0 = win * (x[i] - mean) / 32768.0f + c * s1 - s2;
        s2 = s1; s1 = s0; wsum += win;
    }
    return sqrtf(fmaxf(s1 * s1 + s2 * s2 - c * s1 * s2, 0.0f)) * 2.0f / wsum;
}

static float zc_freq(const int16_t *x, int n, float mean) {
    int zc = 0;
    for (int i = 1; i < n; i++) if (((x[i - 1] - mean) < 0) != ((x[i] - mean) < 0)) zc++;
    return zc / 2.0f / (n / AUDIO_SAMPLE_RATE_EXACT);
}

static float rms_dbfs(const int16_t *x, int n) {
    const float m = mean_of(x, n);
    double sq = 0; for (int i = 0; i < n; i++) { const double v = x[i] - m; sq += v * v; }
    return 20.0f * log10f((float)sqrt(sq / n) / 32768.0f + 1e-9f);
}

static float db(float a) { return 20.0f * log10f(a + 1e-9f); }

static void kv(const char *k, float v, int digits, const char *unit) {
    CONSOLE.print(' '); CONSOLE.print(k); CONSOLE.print('='); CONSOLE.print(v, digits); CONSOLE.print(unit);
}

static void measure(const char *name, const int16_t *x, float f0, float other) {
    const float m = mean_of(x, TONE_N);
    const float a = tone_amp(x, TONE_N, m, f0);
    float h = 0;
    for (int k = 2; k <= 5; k++) { const float ak = tone_amp(x, TONE_N, m, k * f0); h += ak * ak; }
    const float freq = zc_freq(x, TONE_N, m);
    const float level = db(a), thd = db(sqrtf(h) / a), xt = db(tone_amp(x, TONE_N, m, other) / a);
    CONSOLE.print("info "); CONSOLE.print(name);
    kv("freq", freq, 1, "Hz"); kv("level", level, 2, "dBFS"); kv("thd", thd, 1, "dB");
    kv("xtalk", xt, 1, "dB"); kv("dc", m, 1, "LSB"); CONSOLE.println();
    char tag[24];
    snprintf(tag, sizeof tag, "%s_FREQ", name);  check(tag, fabsf(freq - f0) < 0.01f * f0);
    snprintf(tag, sizeof tag, "%s_LEVEL", name); check(tag, level > -9.0f && level < -4.0f);
    snprintf(tag, sizeof tag, "%s_THD", name);   check(tag, thd < -50.0f);
    snprintf(tag, sizeof tag, "%s_XTALK", name); check(tag, xt < -50.0f);
}

void setup() {
    CONSOLE.begin(115200);
    while (!CONSOLE && millis() < 2000) {}
    CONSOLE.println("=== BOOT ===");
    CONSOLE.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n", "line_loopback_hw", 1, __DATE__, __TIME__);

    AudioMemory(80);
    const bool en = codec.enable();
    codec.inputSelect(AUDIO_INPUT_LINEIN);
    codec.lineInLevel(0);     // 3.12 Vpp full scale
    codec.lineOutLevel(13);   // 3.16 Vpp full scale: unity round trip
    check("CODEC", en);

    toneL.frequency(FREQ_L); toneL.amplitude(AMPL);
    toneR.frequency(FREQ_R); toneR.amplitude(AMPL);
    delay(500);
    const long t_tone = record(rec_tone_L, rec_tone_R, TONE_BLOCKS);
    toneL.amplitude(0); toneR.amplitude(0);
    delay(300);
    const long t_quiet = record(rec_quiet_L, rec_quiet_R, QUIET_BLOCKS);
    check("RATE", t_tone >= 950 && t_tone <= 1050 && t_quiet > 0);
    rec_done = 1;

    if (t_tone > 0) {
        measure("L", rec_tone_L, FREQ_L, FREQ_R);
        measure("R", rec_tone_R, FREQ_R, FREQ_L);
    }
    if (t_quiet > 0) {
        const float fl = rms_dbfs(rec_quiet_L, QUIET_N), fr = rms_dbfs(rec_quiet_R, QUIET_N);
        CONSOLE.print("info floor"); kv("L", fl, 1, "dBFS"); kv("R", fr, 1, "dBFS"); CONSOLE.println();
        check("FLOOR", fl < -60.0f && fr < -60.0f);
    }
    CONSOLE.println((all_ok && t_tone > 0 && t_quiet > 0) ? "LOOPBACK=PASS" : "LOOPBACK=FAIL");
}

void loop() {}
