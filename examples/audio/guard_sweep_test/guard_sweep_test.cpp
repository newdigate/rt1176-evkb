#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"
#include "synth_sine.h"
#include "synth_karplusstrong.h"
#include "synth_simple_drum.h"
#include "synth_wavetable.h"
#include "effect_delay.h"
#include "play_queue.h"
#include "record_queue.h"
#include "analyze_peak.h"
#include <math.h>

// Guard-sweep revival gate: six stages, each impossible to pass with the
// pre-sweep Audio sources (dead update() bodies produce silence; the old
// delay/queue depth caps are below what the stages demand).

// No I/O node in this graph, so nothing calls the protected update_setup()
// that arms IRQ_SOFTWARE dispatch -- same pattern as audiostream_test.
struct GraphClock : public AudioStream {
    GraphClock() : AudioStream(0, NULL) { update_setup(); }
    void update(void) override {}
};
static GraphClock clock1;

static AudioSynthWaveformSine     sine1;      // feeds delay + record queue
static AudioEffectDelay           delay1;
static AudioSynthKarplusStrong    karplus1;
static AudioSynthSimpleDrum       drum1;
static AudioSynthWavetable        wt1;
static AudioPlayQueue             pq1;
static AudioRecordQueue           recq1;      // depth stage (from sine1)
static AudioRecordQueue           recp1;      // play-queue counting stage
static AudioAnalyzePeak           peak_del, peak_k, peak_d, peak_w;

static AudioConnection c1(sine1, 0, delay1, 0);
static AudioConnection c2(delay1, 0, peak_del, 0);   // 200 ms tap
static AudioConnection c3(sine1, 0, recq1, 0);
static AudioConnection c4(karplus1, 0, peak_k, 0);
static AudioConnection c5(drum1, 0, peak_d, 0);
static AudioConnection c6(wt1, 0, peak_w, 0);
static AudioConnection c7(pq1, 0, recp1, 0);

// --- synthetic single-cycle sine instrument for AudioSynthWavetable --------
// 256-sample cycle, INDEX_BITS=8: the 32-bit phase spans exactly one cycle,
// so looping is the natural uint32 wrap and the explicit loop adjustment
// (LOOP_PHASE_END/LENGTH = 0xFFFFFFFF) never fires. Entry 255 is fetched as a
// packed uint32 pair, so the table carries guard samples [256..259].
// Envelope counts are in 8-sample units; ATTACK/DECAY must be >=1 (a zero
// divides by zero in the envelope state machine). SUSTAIN_MULT=0 sustains at
// unity. Vibrato/modulation delays are maxed so the LFOs never engage.
static int16_t wt_table[260];
static const AudioSynthWavetable::sample_data wt_samples[1] = {{
    wt_table,                     // sample
    true,                         // LOOP
    8,                            // INDEX_BITS
    4294967296.0f / 44100.0f,     // PER_HERTZ_PHASE_INCREMENT
    0xFFFFFFFFu,                  // MAX_PHASE (unused when LOOP)
    0xFFFFFFFFu,                  // LOOP_PHASE_END
    0xFFFFFFFFu,                  // LOOP_PHASE_LENGTH
    0xFFFF,                       // INITIAL_ATTENUATION_SCALAR (no atten)
    0,                            // DELAY_COUNT
    6,                            // ATTACK_COUNT  (48 samples)
    1,                            // HOLD_COUNT
    1,                            // DECAY_COUNT   (>=1: div-by-zero guard)
    50,                           // RELEASE_COUNT (400 samples ~ 3 blocks)
    0,                            // SUSTAIN_MULT  (sustain at unity)
    0xFFFFFFFFu, 0, 0.0f, 0.0f,          // vibrato: never engages
    0xFFFFFFFFu, 0, 0.0f, 0.0f, 0, 0     // modulation: never engages
}};
static const uint8_t wt_ranges[1] = {127};
static const AudioSynthWavetable::instrument_data wt_instr = {1, wt_ranges, wt_samples};

static void pump(int n) {
    for (int i = 0; i < n; i++) {
        NVIC_SET_PENDING(IRQ_SOFTWARE);
        delayMicroseconds(200);
    }
}

static float read_peak(AudioAnalyzePeak &p) {
    return p.available() ? p.read() : 0.0f;   // no blocks seen = silence
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    AudioMemory(150);   // 69-block delay ring + 60-block recq hold + graph slack
    for (int i = 0; i < 260; i++)
        wt_table[i] = (int16_t)lrintf(20000.0f * sinf(2.0f * (float)M_PI * (i % 256) / 256.0f));

    sine1.amplitude(0.9f);
    sine1.frequency(440.0f);
    delay1.delay(0, 200.0f);      // 69 blocks: exceeds the old 48-block queue

    // STAGE_DELAY -- must run FIRST: the block count since boot is the clock.
    pump(55);                                  // 160 ms: before the 200 ms tap
    float d_early = read_peak(peak_del);
    pump(30);                                  // 246 ms: tap must be live
    float d_late = read_peak(peak_del);
    bool pass_delay = (d_early < 0.02f) && (d_late > 0.5f);
    Serial1.print("GS: delay early="); Serial1.print(d_early, 4);
    Serial1.print(" late=");           Serial1.println(d_late, 4);
    Serial1.println(pass_delay ? "STAGE_DELAY=PASS" : "STAGE_DELAY=FAIL");

    // STAGE_KARPLUS -- pluck burst then decay (windows separated so the
    // peak-hold reset in read() isolates the quiet tail).
    karplus1.noteOn(220.0f, 1.0f);
    pump(4);
    float k_a = read_peak(peak_k);
    pump(300); (void)read_peak(peak_k);        // discard the loud middle
    pump(60);
    float k_b = read_peak(peak_k);
    bool pass_k = (k_a > 0.10f) && (k_b < k_a * 0.6f);
    Serial1.print("GS: karplus a="); Serial1.print(k_a, 4);
    Serial1.print(" b=");            Serial1.println(k_b, 4);
    Serial1.println(pass_k ? "STAGE_KARPLUS=PASS" : "STAGE_KARPLUS=FAIL");

    // STAGE_DRUM -- 150 ms drum hit: burst, then silence after the envelope.
    drum1.frequency(60.0f);
    drum1.length(150);
    drum1.secondMix(0.0f);
    drum1.pitchMod(0.5f);
    drum1.noteOn();
    pump(3);
    float dr_a = read_peak(peak_d);
    pump(100); (void)read_peak(peak_d);        // ride out the 52-block decay
    pump(30);
    float dr_b = read_peak(peak_d);
    bool pass_dr = (dr_a > 0.2f) && (dr_b < 0.05f);
    Serial1.print("GS: drum a="); Serial1.print(dr_a, 4);
    Serial1.print(" b=");         Serial1.println(dr_b, 4);
    Serial1.println(pass_dr ? "STAGE_DRUM=PASS" : "STAGE_DRUM=FAIL");

    // STAGE_WAVETABLE -- synthetic instrument: sustained tone, then release.
    wt1.setInstrument(wt_instr);
    wt1.playNote(69, 127);                     // A4 = 440 Hz
    pump(12);
    float w_a = read_peak(peak_w);
    bool w_sustain = (wt1.getEnvState() == AudioSynthWavetable::STATE_SUSTAIN);
    wt1.stop();
    pump(6); (void)read_peak(peak_w);          // ride out the ~3-block release
    pump(10);
    float w_b = read_peak(peak_w);
    bool pass_w = (w_a > 0.2f) && (w_a < 0.9f) && w_sustain && (w_b < 0.05f);
    Serial1.print("GS: wt a="); Serial1.print(w_a, 4);
    Serial1.print(" b=");       Serial1.print(w_b, 4);
    Serial1.print(" sustain="); Serial1.println(w_sustain ? 1 : 0);
    Serial1.println(pass_w ? "STAGE_WAVETABLE=PASS" : "STAGE_WAVETABLE=FAIL");

    // STAGE_RECQ -- hold >52 blocks (old ring cap) then drain intact.
    recq1.begin();
    pump(60);
    int held = recq1.available();
    int drained = 0;
    bool content_ok = false;
    while (recq1.available() > 0) {
        int16_t *b = recq1.readBuffer();
        if (b) {
            if (drained == 0) {                // sine content, not silence
                for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
                    if (b[i] > 8000 || b[i] < -8000) { content_ok = true; break; }
            }
            drained++;
        }
        recq1.freeBuffer();
    }
    recq1.end();
    bool pass_rq = (held >= 55) && (drained == held) && content_ok;
    Serial1.print("GS: recq held="); Serial1.print(held);
    Serial1.print(" drained=");      Serial1.print(drained);
    Serial1.print(" content=");      Serial1.println(content_ok ? 1 : 0);
    Serial1.println(pass_rq ? "STAGE_RECQ=PASS" : "STAGE_RECQ=FAIL");

    // STAGE_PLAYQ -- enqueue 40 pattern blocks (old cap 32), play them
    // through, count and spot-check them on the far side.
    recp1.begin();
    for (int k = 0; k < 40; k++) {
        int16_t *b = pq1.getBuffer();          // pool is ample: never stalls
        if (!b) break;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) b[i] = (int16_t)((k + 1) * 100);
        pq1.playBuffer();
    }
    pump(45);
    int got = recp1.available();
    int16_t first_val = 0, last_val = 0;
    for (int k = 0; k < got; k++) {
        int16_t *b = recp1.readBuffer();
        if (b) {
            if (k == 0)       first_val = b[0];
            if (k == got - 1) last_val  = b[0];
        }
        recp1.freeBuffer();
    }
    recp1.end();
    bool pass_pq = (got == 40) && (first_val == 100) && (last_val == 4000);
    Serial1.print("GS: playq got="); Serial1.print(got);
    Serial1.print(" first=");        Serial1.print(first_val);
    Serial1.print(" last=");         Serial1.println(last_val);
    Serial1.println(pass_pq ? "STAGE_PLAYQ=PASS" : "STAGE_PLAYQ=FAIL");

    bool all = pass_delay && pass_k && pass_dr && pass_w && pass_rq && pass_pq;
    Serial1.println(all ? "GUARD_SWEEP_ALL=PASS" : "GUARD_SWEEP_ALL=FAIL");
}
void loop() {}
