#include "Arduino.h"
#include "HardwareSerial.h"
#include <math.h>                // fabsf -- named explicitly, as the sibling
                                 // audio examples do, rather than relying on
                                 // Arduino.h to drag it in transitively.
#include <Audio.h>
// Audio.h pulls in every codec driver EXCEPT this one -- control_wm8962.h is not
// in its include list (control_wm8960.h is), so the WM8962 header must be named
// explicitly, exactly as audiooutput_i2s_test does.
#include "control_wm8962.h"

// rt1176-only example: LPUART1 console (`Serial1` on the imxrt1176 core) and
// the WM8962 codec (control_wm8962.cpp talks Wire2/LPI2C5). Same wiring as
// audiooutput_i2s_test, minus that example's rt1062 half.
#define CONSOLE Serial1

// AudioSynthAcidBass -> {RMS, Peak, I2S L+R}. The SAI1 TX DMA self-clocks the
// graph in QEMU and on silicon (proven by audiooutput_i2s_test), so analyzer
// blocks keep arriving without manual pumping.
AudioSynthAcidBass acid;
AudioAnalyzeRMS    rms;
AudioAnalyzePeak   peak;
AudioOutputI2S     out;
AudioConnection    cRms(acid, 0, rms,  0);
AudioConnection    cPk (acid, 0, peak, 0);
AudioConnection    cL  (acid, 0, out,  0);
AudioConnection    cR  (acid, 0, out,  1);
AudioControlWM8962 wm;

// One measurement window: the largest AudioAnalyzeRMS::read() and the largest
// AudioAnalyzePeak::read() seen during `ms`, plus the number of reads.
//
// Both read() calls report the value accumulated SINCE THE LAST read(), not
// "this block" -- so the window is drained on entry, otherwise the first sample
// inside it carries energy from before the window began (which is exactly how a
// post-noteOff silence check can read loud).
//
// WHY BOTH STATISTICS, AND WHY THE ACCENT CHECK USES THE PEAK ONE:
// max-of-RMS is NOT invariant to how blocks group into reads. read() returns
// the RMS over however many blocks accumulated, so a read covering 2 decaying
// blocks reports less than either block alone, and the maximum over a window
// therefore depends on scheduling. Measured in the host model: at the musical
// settings the accent RMS ratio swings 1.11-1.51 purely from grouping and
// note-start phase. max-of-PEAK has no such dependence -- read() returns a
// max, so the maximum over the window equals the maximum over every block in
// it no matter how the reads fall. Same window, same data, a statistic that
// does not move. maxRms/rmsReads stay for the quiet/liveness checks, where the
// question is "was there any energy at all" and grouping cannot matter.
//
// `winReads` counts the reads the window actually got. It is the liveness half
// of every quiet assertion: a reading of 0.0f means EITHER real silence OR that
// no blocks arrived at all, and only the count separates them.
//
// IT MUST BE COUNTED OFF THE PEAK ANALYZER, NOT THE RMS ONE. The two disagree
// about what an empty update() means:
//   analyze_rms.cpp::update()   `if (!block) { count++; return; }`
//   analyze_peak.cpp::update()  `if (!block) { return; }`   (new_output untouched)
// So rms.available() goes true on an update() TICK whether or not a block was
// delivered, and a count taken from it cannot tell "the graph is running and
// silent" from "the graph is running and starved" -- which is the exact
// distinction the guard exists to make. peak.available() only rises on a
// delivered block, so it counts what the comment claims. In a healthy graph the
// two agree exactly (measured 86/86 in the accent window, 40/40 in the quiet
// window), so this is threshold-neutral -- it is the starved case it fixes.
//
// peak.read() is guarded by available() and must stay that way: with no block
// since the last read it returns abs(int16 min) / 32767 = 1.00003, i.e. a
// dead graph would report FULL SCALE, not zero.
static int   winReads;
static float winMaxRms;
static float winMaxPeak;
static void measureOver(uint32_t ms) {
    if (rms.available())  (void)rms.read();    // discard pre-window history
    if (peak.available()) (void)peak.read();
    winMaxRms = 0.0f; winMaxPeak = 0.0f; winReads = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < ms) {
        if (rms.available())  {             float v = rms.read();  if (v > winMaxRms)  winMaxRms  = v; }
        if (peak.available()) { winReads++; float v = peak.read(); if (v > winMaxPeak) winMaxPeak = v; }
        yield();
    }
}

void setup() {
    CONSOLE.begin(115200);
    while (!CONSOLE) {}
    CONSOLE.println("ACID-GATE v1");
    AudioMemory(16);
    wm.enable();
    wm.volume(0.6f);

    // The three the accent measurement below borrows and gives back. Named
    // once and used at BOTH sites: the restore used to repeat them as literals
    // ~90 lines further down, and nothing in the gate would have caught the two
    // drifting apart -- the only assertion on loop() is a nonzero heartbeat, so
    // the audible hardware evidence would have been of a voice nobody
    // configured.
    const float MUS_CUTOFF = 500.0f, MUS_RES = 0.75f, MUS_ENVMOD = 0.7f;
    acid.waveform(WAVEFORM_SAWTOOTH);
    acid.cutoff(MUS_CUTOFF);
    acid.resonance(MUS_RES);
    acid.envMod(MUS_ENVMOD);
    acid.decay(0.25f);
    acid.accent(0.8f);
    acid.subLevel(0.3f);
    acid.slideTime(0.08f);
    acid.level(0.7f);

    // --- ACID_ACCENT: same note, same window; velocity 1 vs 127 ------------
    // accentAmt = accent * vel/127, so vel=1 is effectively unaccented. The
    // accented note gets a hotter VCA target (1+0.5*accentAmt) AND a wider
    // filter sweep (accentAmt adds to envMod in the modOct term).
    //
    // TEST INSTRUMENTATION, DELIBERATE: the two measurement notes are played
    // at filter settings chosen for MEASUREMENT, not for music, and the
    // musical settings are restored immediately afterwards (the pattern player
    // in loop() -- the hardware evidence -- is unaffected). The reason is
    // measured, not aesthetic:
    //
    //   At the musical settings (cutoff 500, res 0.75, envMod 0.7) both notes
    //   sit inside the `tanhf(s)` at the end of the chain, and NOT equally --
    //   the saturator takes more away from the louder note, so the analytic
    //   VCA ratio of 1.4/1.0031 = 1.396 arrived as a measured ~1.14, with a
    //   margin over the old 1.10 threshold smaller than the run-to-run spread.
    //
    //   Backing the filter off (cutoff 55, res 0.30, envMod 0.2) does two
    //   things at once. It takes the UNACCENTED note out of saturation, so
    //   accent's VCA gain shows up undistorted; and it puts the plain note on
    //   the filter's skirt, so accent's 3.2-octave sweep of the cutoff --
    //   the other half of what accent does, and invisible when the filter is
    //   already wide open -- becomes the dominant term. Note `level` is NOT a
    //   knob here: it is applied AFTER the tanh (`y = level_ * tanhf(...)`),
    //   so it scales both notes equally and leaves every ratio untouched
    //   (verified: ratio identical at level 0.2 / 0.7 / 1.0).
    //
    // MEASURED, not predicted: accent_ratio lands in 3.131-3.336 across 22 QEMU
    // runs, spread 0.205. Against the 2.00 threshold that is a margin of 1.131,
    // or 5.5x the spread. (The 11 runs since ACID_ACCENT_VCA was added are
    // tighter still, 3.157-3.336; the wider figure is the conservative one and
    // is what the 5.5x is quoted against.)
    //
    // The threshold is 2.00 because it sits near the geometric mean of the
    // working value (~3.27) and the 1.0 that a no-op accent produces.
    //
    // WHAT THIS CHECK DOES AND DOES NOT COVER -- read before trusting it.
    // The ratio is DOMINATED BY THE FILTER SWEEP: of the ~3.23x, the cutoff
    // sweep supplies 3.26x and the VCA only 1.40x. So deleting `0.5f*accentAmt`
    // from ampTarget -- half of everything accent() does, and the half a player
    // hears as punch -- still measures 2.70 here and still PASSES. That was
    // measured, not reasoned: 2.776 / 2.752 / 2.695 against an analytic 2.698.
    // Raising this threshold is NOT the fix; separating 3.13 from 2.78 needs
    // ~3.0, whose margin (0.13) is below the 0.204 spread and reintroduces the
    // flake. ACID_ACCENT_VCA below covers that half instead, by nulling the
    // filter term. Keep both; neither is sufficient alone.
    //
    // MUTATION-TESTED. `acid.accent(0.0f)` here (accentAmt = 0, so neither half
    // does anything) measured 1.025 / 1.002 / 1.002 and failed with "FAIL:
    // accent" every time. Deleting only the FILTER term from modOct, leaving
    // the VCA intact, measured 1.310 / 1.432 / 1.346 -- also a fail, and the
    // run that proves this check is the filter half's coverage. pk_plain stayed
    // at ~0.18 throughout, so the liveness guards below cannot have been what
    // failed. The two mutations are orthogonal and the coverage is a 2x2:
    //
    //                          ACID_ACCENT   ACID_ACCENT_VCA
    //   nothing deleted           PASS 3.26      PASS 1.405
    //   VCA term deleted          PASS 2.70      FAIL 1.010
    //   filter term deleted       FAIL 1.35      PASS 1.395
    //
    // Each probe is blind to exactly the mutation the other catches. Re-run
    // both if you touch any number in either.
    const float MEAS_CUTOFF = 55.0f, MEAS_RES = 0.30f, MEAS_ENVMOD = 0.2f;
    acid.cutoff(MEAS_CUTOFF); acid.resonance(MEAS_RES); acid.envMod(MEAS_ENVMOD);

    // PRIMING NOTE -- throwaway, and it is not optional. Until the first
    // noteOn, curFreq_ is 0, so the oscillator phase is FROZEN and the ladder is
    // fed a constant -1.0 (saw at phase 0) + 0.3 (sub) = -0.7 DC for the whole
    // of setup(). The first note ever played therefore starts from a filter
    // charged to that DC. TWO things then go wrong together, and the DC alone
    // is not enough to explain the size of it:
    //   * the DC charge itself is modest -- the ladder has unity forward gain
    //     at DC, so s4 settles at `in - fb*tanh(s4)` = -0.324 and |y| = 0.219;
    //   * the damage is the CUTOFF JUMP at noteOn. filtEnv_ goes 0 -> 1, taking
    //     fc from 55 to 97.5 Hz, a 1.77x step in `f`. The stage steady states
    //     scale as s1 ~ f^3, s2 ~ f^2, s3 ~ f, so that step leaves s1 5.6x and
    //     s2 3.1x away from their new targets, and the mismatch overshoots into
    //     the tanh -- a clipped thump lasting ~35 ms, which is exactly the
    //     window the peak is taken over.
    // At the musical 500 Hz cutoff the same thing settles in ~3 ms and never
    // shows; at 55 Hz the four poles sit at tau ~7.8 ms each.
    //
    // Unprimed, this is INTERMITTENT, which is the strongest argument for the
    // note rather than a weaker one: three unprimed runs measured 0.841 (FAIL,
    // pk_plain 0.696 -- clipping -- against pk_accent 0.581), 3.248 and 3.275.
    // A check that fails one run in three with a WORKING accent is worse than
    // one that fails every time. noteOff leaves curFreq_ alone, so one
    // throwaway note gets the oscillator running for good.
    //
    // 500 + 400 ms, not 200 + 300: the unload needs ~450 ms of AUDIO time to
    // fall below the note's own peak, and QEMU's audio clock is not pinned to
    // wall clock, so wall-clock priming has to carry margin. At 200 + 300 the
    // two slowest of seven runs still caught some of it -- pk_plain 0.2025 and
    // 0.2008 against 0.1879 for the five that did not -- and that alone was the
    // whole 0.219 run-to-run spread in the ratio. Do not trim these delays
    // back; they are not padding.
    acid.noteOn(33, 100);
    delay(500);
    acid.noteOff(33);
    delay(400);

    acid.noteOn(33, 1);                  // A1 = 55 Hz
    measureOver(200);
    float rmsPlain = winMaxRms, pkPlain = winMaxPeak; int readsPlain = winReads;
    acid.noteOff(33);
    delay(300);                          // release (~8 ms tau) dies away
    acid.noteOn(33, 127);
    measureOver(200);
    float rmsAccent = winMaxRms, pkAccent = winMaxPeak; int readsAccent = winReads;
    acid.noteOff(33);

    // --- ACID_ACCENT_VCA: the OTHER half of accent ------------------------
    // Still at the measurement settings, deliberately -- this probe has to run
    // before the restore below.
    //
    // The check above cannot see a break confined to the VCA, because the
    // filter sweep swamps it. This one removes the filter from the comparison
    // instead of trying to out-vote it: hold each note for 2 s first, which
    // decays filtEnv_ to exp(-2/0.25) = e^-8 = 3.4e-4. The modOct difference
    // between the two velocities is then (0.8 - 0.0063) * 3.4e-4 * ENV_OCT =
    // 1.1e-3 octaves, which on a 24 dB/oct skirt is 0.026 dB. The filter term
    // is nulled to well below the measurement floor and the only thing left in
    // the ratio is the VCA target ratio, 1.4/1.00315 = 1.3956.
    //
    // 2 s, not 1 s: MEASURED at a 1 s hold this reads 1.8385 / 1.8279, because
    // e^-4 still leaves ~4% of the cutoff difference and at fc ~ 55 Hz that
    // lands on the ladder's skirt where it has plenty of leverage. The null
    // needs the extra four time constants. Costs ~5 s of guest time; that is
    // what it costs.
    //
    // The band is TWO-SIDED because an over-strong VCA is as wrong as a missing
    // one, and unlike the ratio above this band cannot be satisfied by the
    // filter term at all -- it is nulled here. The upper edge is not decorative:
    // it is what caught the 1 s hold above, which sailed past 1.20 on filter
    // contamination alone. An under-nulled probe fails loudly instead of
    // passing for the wrong reason.
    //
    // Measured working across 11 runs: 1.4026-1.4092, spread 0.0066, against
    // the analytic 1.3956. Measured with `0.5f*accentAmt` deleted from
    // ampTarget: 1.0095 / 1.0088 / 1.0132. The nearest band edge (1.20) sits
    // 0.203 below the working floor -- 31x the spread -- and 0.187 above the
    // broken ceiling. This is by far the tightest statistic in the sketch,
    // which is what nulling a term buys you.
    acid.noteOn(33, 1);
    delay(2000);                         // filtEnv_ -> e^-8; filter term nulled
    measureOver(200);
    float pkVcaPlain = winMaxPeak; int readsVcaPlain = winReads;
    acid.noteOff(33);
    delay(300);
    acid.noteOn(33, 127);
    delay(2000);
    measureOver(200);
    float pkVcaAccent = winMaxPeak; int readsVcaAccent = winReads;
    acid.noteOff(33);
    float vcaRatio = pkVcaAccent / pkVcaPlain;

    // Restore the musical voice before anything else runs. These are the same
    // named constants the settings were applied from, NOT copies of them.
    acid.cutoff(MUS_CUTOFF); acid.resonance(MUS_RES); acid.envMod(MUS_ENVMOD);
    CONSOLE.print("ACID: pk_plain=");   CONSOLE.println(pkPlain, 4);
    CONSOLE.print("ACID: pk_accent=");  CONSOLE.println(pkAccent, 4);
    CONSOLE.print("ACID: accent_ratio="); CONSOLE.println(pkAccent / pkPlain, 3);
    // The RMS pair is printed for diagnosis only -- it is the grouping-sensitive
    // statistic and nothing asserts on it. Keeping it visible is how a future
    // divergence between the two statistics gets noticed rather than guessed at.
    CONSOLE.print("ACID: rms_plain=");  CONSOLE.println(rmsPlain, 4);
    CONSOLE.print("ACID: rms_accent="); CONSOLE.println(rmsAccent, 4);
    CONSOLE.print("ACID: accent_reads="); CONSOLE.print(readsPlain);
    CONSOLE.print("/"); CONSOLE.println(readsAccent);
    // Two liveness guards, both on the UNACCENTED note, because a ratio is a
    // ratio however small its terms:
    //   pkPlain > 0.05 -- a nearly dead graph trickling out noise can still
    //     produce a large ratio between two tiny numbers, so the baseline has
    //     to be genuinely audible first. The measurement settings put it at
    //     ~0.18, ~11 dB of margin.
    //   reads >= 10 -- neither window may be empty. Without it a window that
    //     received nothing reports pk 0.0 and the check divides 0 by 0; the
    //     silence would be the harness's, not the synth's. A 200 ms window at
    //     2.9 ms/block should yield ~68. Counted off the PEAK analyzer -- see
    //     measureOver() for why the RMS one cannot serve here.
    bool accentOk = pkPlain > 0.05f && readsPlain >= 10 && readsAccent >= 10
                    && pkAccent > pkPlain * 2.00f;
    CONSOLE.println(accentOk ? "ACID_ACCENT=PASS" : "ACID_ACCENT=FAIL");

    CONSOLE.print("ACID: vca_plain=");  CONSOLE.println(pkVcaPlain, 4);
    CONSOLE.print("ACID: vca_accent="); CONSOLE.println(pkVcaAccent, 4);
    CONSOLE.print("ACID: vca_ratio=");  CONSOLE.println(vcaRatio, 4);
    CONSOLE.print("ACID: vca_reads=");  CONSOLE.print(readsVcaPlain);
    CONSOLE.print("/"); CONSOLE.println(readsVcaAccent);
    // 0.02 floor rather than the 0.05 above: after 2 s of decay the notes are
    // genuinely quiet, and 0.02 against the measured 0.0418 is ~6 dB. Same
    // read-count guard, same reason.
    bool vcaOk = pkVcaPlain > 0.02f && readsVcaPlain >= 10 && readsVcaAccent >= 10
                 && vcaRatio > 1.20f && vcaRatio < 1.65f;
    CONSOLE.println(vcaOk ? "ACID_ACCENT_VCA=PASS" : "ACID_ACCENT_VCA=FAIL");

    delay(300);

    // --- ACID_SLIDE: A1 -> A2 legato glide, monotone, no env retrigger -----
    acid.noteOn(33, 100);
    // 100 ms, not 50: this delay is the knob that sets how much filter envelope
    // decay `envBefore < 0.9` demands, and so it is the tightest audio-clock
    // assumption in the sketch. At 50 ms, exp(-0.05k/0.25) < 0.9 needs the QEMU
    // audio clock at k > 0.527 of wall-clock; at 100 ms, envBefore is nominally
    // exp(-0.4) = 0.670 and the requirement relaxes to k > 0.263 -- in line with
    // the other checks (k in [0.17, 5] for the decay ratio, k >= 0.29 for
    // rmsReads, k <= ~32 for sawGlide). IF THE AUDIO CLOCK TURNS OUT SLOW,
    // LENGTHEN THIS DELAY -- do not raise the 0.9 threshold, which is what
    // makes the check mean "the envelope moved".
    delay(100);
    float f0        = acid.currentFreq();  // non-slide noteOn jumps: exactly 55
    float envBefore = acid.filtEnv();      // sampled immediately BEFORE the slide
    acid.noteOn(45, 100, true);            // slide toward A2 = 110 Hz
    float envAtSlide = acid.filtEnv();
    // Retrigger check, stated relatively so it holds at ANY audio block rate:
    //   envBefore < 0.9   -- the graph clocked and the envelope actually decayed
    //   envAtSlide <= envBefore -- the slide did not reset it
    // Both halves are needed. An absolute `envAtSlide < 0.9` would silently
    // require the audio clock to run at a particular fraction of wall-clock; and
    // `envAtSlide <= envBefore` alone passes vacuously on a stalled graph,
    // where both read 1.0.
    bool mono     = true;
    bool sawGlide = false;
    float prev = f0;
    for (int i = 0; i < 60; i++) {       // 600 ms = 7.5 tau of the 80 ms glide
        delay(10);
        float f = acid.currentFreq();
        // Strictly mid-glide: neither the start value nor the destination.
        // Monotonicity alone is satisfied by a CONSTANT sequence, so a
        // slideTime bug that snaps straight to 110 Hz (e.g. the time read as
        // samples, making the glide coefficient ~1.0) would keep `mono` true
        // and pass with no glide whatsoever. A real tau=80 ms glide reads
        // ~61.5 Hz at 10 ms and ~67.2 Hz at 20 ms, so several polls land in
        // this band; a jump produces none.
        if (f > f0 + 1.0f && f < 109.0f) sawGlide = true;
        if (f < prev - 0.01f) mono = false;
        prev = f;
    }
    float fEnd = acid.currentFreq();
    acid.noteOff(45); acid.noteOff(33);
    CONSOLE.print("ACID: f_start=");     CONSOLE.println(f0, 2);
    CONSOLE.print("ACID: f_end=");       CONSOLE.println(fEnd, 2);
    CONSOLE.print("ACID: env_before=");  CONSOLE.println(envBefore, 4);
    CONSOLE.print("ACID: env_at_slide="); CONSOLE.println(envAtSlide, 4);
    bool slideOk = fabsf(f0 - 55.0f) < 1.0f && fabsf(fEnd - 110.0f) < 1.0f
                   && mono && sawGlide
                   && envBefore < 0.9f && envAtSlide <= envBefore + 1e-6f;
    CONSOLE.println(slideOk ? "ACID_SLIDE=PASS" : "ACID_SLIDE=FAIL");

    // --- ACID_DECAY: filter env falls while held; silence after noteOff ----
    acid.noteOn(33, 100);
    delay(30);
    float e1 = acid.filtEnv();
    delay(150);
    float e2 = acid.filtEnv();
    acid.noteOff(33);
    delay(300);                          // amp release tau 8 ms -> silence
    measureOver(100);                    // drains on entry; sets winReads
    // winReads counts DELIVERED BLOCKS (peak analyzer), which is what makes the
    // guard below mean anything -- the RMS analyzer's own count ticks on empty
    // updates too. See measureOver().
    float rmsQuiet = winMaxRms; int blockReads = winReads;
    // Expected at the nominal rate: e1 = exp(-0.03/0.25) = 0.887,
    // e2 = exp(-0.18/0.25) = 0.487, so ratio = exp(-0.15/0.25) = 0.549.
    // Checking the RATIO rather than only `e1 > e2` is what makes this a test
    // of the time constant: bare monotonicity passes on a decay 100x too fast
    // or 5x too slow. The band still tolerates an audio clock running anywhere
    // in k = [0.17, 5] of nominal -- 0.90 rejects tau = 1.42 s, 0.05 rejects
    // tau = 0.05 s.
    float ratio = e2 / e1;
    CONSOLE.print("ACID: env_early="); CONSOLE.println(e1, 4);
    CONSOLE.print("ACID: env_late=");  CONSOLE.println(e2, 4);
    CONSOLE.print("ACID: env_ratio="); CONSOLE.println(ratio, 4);
    CONSOLE.print("ACID: rms_quiet="); CONSOLE.println(rmsQuiet, 4);
    CONSOLE.print("ACID: block_reads="); CONSOLE.println(blockReads);
    // blockReads >= 10: a 100 ms window at 2.9 ms/block should yield ~34.
    // Without it, `rmsQuiet < 0.01` is equally satisfied by a graph that
    // delivered nothing at all -- the silence would be the harness's, not the
    // synth's.
    bool decayOk = ratio < 0.90f && ratio > 0.05f && e2 > 0.0f
                   && rmsQuiet < 0.01f && blockReads >= 10;
    CONSOLE.println(decayOk ? "ACID_DECAY=PASS" : "ACID_DECAY=FAIL");

    CONSOLE.println("ACID-DONE");
}

// --- the audible part: a looping 16-step acid line for the bench session ---
// {note, accent, slide, gate}; ~130 BPM sixteenths (115 ms). A-minor cliche.
// slide=true means "tie FROM the previous step": press the new key, THEN
// release the old one (legato), which is what makes noteOn glide.
//
// Two things to know before editing this table:
//  * A slide only bends if the PREVIOUS step was gated -- there has to be a
//    note already sounding to bend away from. Steps 2, 6, 9, 12 and 15 slide,
//    and each is preceded by a gated step; step 11 is gated purely to give
//    step 12 something to glide from (it was a rest, which silently degraded
//    that slide to a hard attack).
//  * Rests (gate=false) still carry a `note` value. It is unused -- the field
//    is there to keep the rows aligned, not because the pitch means anything.
struct Step { uint8_t note; bool accent, slide, gate; };
static const Step pattern[16] = {
    {33,true ,false,true }, {33,false,false,true }, {45,false,true ,true }, {33,false,false,true },
    {36,true ,false,true }, {33,false,false,true }, {31,false,true ,true }, {33,false,false,false},
    {33,false,false,true }, {40,true ,true ,true }, {33,false,false,true }, {45,false,false,true },
    {38,false,true ,true }, {36,true ,false,true }, {31,false,false,true }, {33,false,true ,true },
};

void loop() {
    static int      stepIdx      = 0;
    static uint32_t lastStep     = 0;
    static uint8_t  soundingNote = 255;  // 255 = nothing sounding
    static uint32_t lastBeat     = 0;
    uint32_t now = millis();
    // Seed the heartbeat clock on first entry. Left at 0, the first heartbeat
    // fires on the very first loop() iteration -- before step 0 has produced a
    // single audio block -- so it reports the trailing silence of setup() as
    // `peak=0.0000` and only two of the three heartbeats the gate waits for
    // ever sample the pattern player. millis() is already in the thousands by
    // the time setup() returns, so 0 is a safe "not yet seeded" sentinel.
    if (lastBeat == 0) lastBeat = now;
    if (now - lastStep >= 115) {
        lastStep = now;
        const Step& s = pattern[stepIdx];
        stepIdx = (stepIdx + 1) & 15;
        // A tie requires BOTH a slide flag and a note to press. Keying the
        // release off `!s.slide` alone leaves a {slide=true, gate=false} step
        // taking neither branch -- no release here, no press below -- which
        // holds the previous note forever. Unreachable in the table above, but
        // the table is precisely what gets edited at the bench.
        bool tie = s.gate && s.slide && soundingNote != 255;
        if (soundingNote != 255 && !tie) { acid.noteOff(soundingNote); soundingNote = 255; }
        if (s.gate) {
            acid.noteOn(s.note, s.accent ? 127 : 80, tie);
            // legato: release the old key only AFTER the new one is pressed
            if (soundingNote != 255) acid.noteOff(soundingNote);
            soundingNote = s.note;
        }
    }
    if (now - lastBeat >= 500) {         // heartbeat for the HW transcript
        lastBeat = now;
        if (peak.available()) {
            CONSOLE.print("ACID_ALIVE peak="); CONSOLE.println(peak.read(), 4);
        } else {
            CONSOLE.println("ACID_ALIVE peak=(no update)");
        }
    }
}
