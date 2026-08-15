#include "Arduino.h"
#include "HardwareSerial.h"
#include <math.h>
#include <Audio.h>
#include "control_wm8962.h"   // Audio.h includes control_wm8960.h but NOT this one

#define CONSOLE Serial1

// ★ There is deliberately no `#define PPQN 96` here. transport.h declares the
// resolution as a class constant, `AudioTransport::PPQN`, and a MACRO of that
// name would rewrite the library's own declaration into
// `static const uint32_t 96 = 96;` -- a syntax error inside <Audio.h>. This
// sketch only ever compiled with the macro because the #include happened to
// come first; moving the include below the define, or any user sketch that
// defines PPQN before including <Audio.h>, would detonate it. Use the class
// constant at every site.

// Ordering probes. Each records the transport's sample count at the moment its
// own update() runs. If update order is construction order, `before` runs
// while the transport still holds the PREVIOUS block's count and `after` runs
// once it has advanced -- so `after` leads `before` by exactly one block. This
// discriminates; `samples() > 0` did not, and would have passed even if the
// transport updated last.
//
// ★ `active = true` in the constructor is NOT optional and is not boilerplate.
// AudioStream's constructor sets `active = false` (AudioStream.h:140) and the
// ONLY thing that ever sets it true is AudioConnection::connect()
// (AudioStream.cpp:222,225). software_isr skips every node whose `active` is
// false (AudioStream.cpp:323). These probes are deliberately unconnected --
// they observe the transport rather than passing audio -- so without this line
// their update() is never called at all, `seen` stays 0 on both, and
// TRANSPORT_ORDER reads lead=0 and FAILS for a reason that has nothing to do
// with update order. `active` is protected, so a node with no connections can
// and must opt itself in.
struct OrderProbe : public AudioStream {
    volatile uint64_t seen = 0;
    OrderProbe() : AudioStream(0, NULL) { active = true; }
    void update(void) override;      // defined after `transport` exists
};

// The transport is declared BETWEEN the two probes: AudioStream.h:147-154
// appends each node at the tail of first_update and software_isr walks that
// list head-first, so update order is construction order. A consumer declared
// after it therefore sees the CURRENT block's tick span, not the previous
// one. TRANSPORT_ORDER below asserts this at runtime rather than trusting it.
OrderProbe         probeBefore;
AudioTransport     transport;
OrderProbe         probeAfter;
AudioSynthAcidBass acid;
AudioAnalyzePeak   peak;
AudioOutputI2S     out;
AudioConnection    cPk(acid, 0, peak, 0);
AudioConnection    cL (acid, 0, out,  0);
AudioConnection    cR (acid, 0, out,  1);
AudioControlWM8962 wm;

// Defined here, not inline above: it dereferences `transport`, which does not
// exist yet at the point the struct is declared.
void OrderProbe::update(void) { seen = transport.samples(); }

// Count audio blocks by watching the transport's own sample counter: it
// advances by exactly AUDIO_BLOCK_SAMPLES per update(), so a change in it IS a
// block tick. This is an AUDIO-clock reference. A wall-clock delay() is not,
// and using one here would make every measurement below host-speed-dependent
// -- the exact defect that flaked four acid_bass_test assertions under load.
static uint64_t waitBlocks(int n) {
    uint64_t start = transport.samples(), prev = start;
    int seen = 0;
    uint32_t t0 = millis();
    while (seen < n && millis() - t0 < 10000) {
        uint64_t s = transport.samples();
        if (s != prev) { seen++; prev = s; }
        yield();
    }
    return prev - start;
}

// Total ticks the transport reports across `blocks` blocks. Reading tickCount()
// once per block is only correct because the loop above wakes on every block
// change; a slower poll would miss spans entirely.
static uint32_t countTicksOver(int blocks) {
    uint32_t total = 0;
    uint64_t prev = transport.samples();
    int seen = 0;
    uint32_t t0 = millis();
    while (seen < blocks && millis() - t0 < 10000) {
        uint64_t s = transport.samples();
        if (s != prev) { seen++; prev = s; total += transport.tickCount(); }
        yield();
    }
    return total;
}

void setup() {
    CONSOLE.begin(115200);
    while (!CONSOLE) {}
    CONSOLE.println("TRANSPORT-GATE v1");
    AudioMemory(16);
    wm.enable();
    wm.volume(0.6f);
    acid.cutoff(700.0f); acid.resonance(0.7f); acid.envMod(0.6f);
    acid.decay(0.15f); acid.level(0.7f);

    // --- TRANSPORT_ORDER: the transport updates before later-declared nodes.
    transport.tempo(120.0f);
    transport.play();
    waitBlocks(8);
    // Both probes are read under one IRQ guard so they come from the SAME
    // software_isr pass. Unguarded, this is two separate user-context loads of
    // a value the ISR rewrites every block: an update landing between them
    // takes `sb` from pass N and `sa` from pass N+1 and reports lead=256 -- an
    // intermittent FAIL on a perfectly ordered graph. (uint64_t loads are not
    // atomic on Cortex-M7 either, so the guard covers tearing as well.) This
    // does not weaken the assertion below; it is what makes the number
    // measured actually be the one the assertion is about.
    uint64_t sb, sa;
    __disable_irq();
    sb = probeBefore.seen; sa = probeAfter.seen;
    __enable_irq();
    int64_t lead = (int64_t)sa - (int64_t)sb;
    CONSOLE.print("TR: probe_before="); CONSOLE.println((uint32_t)sb);
    CONSOLE.print("TR: probe_after=");  CONSOLE.println((uint32_t)sa);
    CONSOLE.print("TR: probe_lead=");   CONSOLE.println((int32_t)lead);
    // Exactly one block of lead. Not >=0, which a same-order pair would also give.
    bool orderOk = (lead == AUDIO_BLOCK_SAMPLES);
    CONSOLE.println(orderOk ? "TRANSPORT_ORDER=PASS" : "TRANSPORT_ORDER=FAIL");

    // --- TEMPO: ticks over a counted number of BLOCKS vs the analytic value.
    // At 120 BPM, 96 PPQN: ticks/block = PPQN*bpm*128/(60*44100) = 0.557278.
    // Over 400 blocks that is 222.9 ticks. Both sides are audio-clock
    // quantities, so this holds at any host speed.
    transport.looping(false);
    transport.stop();
    transport.play();
    uint32_t ticks400 = countTicksOver(400);
    float expect400 = (float)AudioTransport::PPQN * 120.0f * 128.0f * 400.0f
                      / (60.0f * AUDIO_SAMPLE_RATE_EXACT);
    float tempoErr = (float)ticks400 / expect400 - 1.0f;
    CONSOLE.print("TR: ticks400=");   CONSOLE.println(ticks400);
    CONSOLE.print("TR: expect400=");  CONSOLE.println(expect400, 2);
    bool tempoOk = fabsf(tempoErr) < 0.02f;
    CONSOLE.println(tempoOk ? "TEMPO=PASS" : "TEMPO=FAIL");

    // --- LOOP: the seam must neither lose nor duplicate a tick.
    // A 1-bar loop at 4/4 is PPQN*4 = 384 ticks. Running long enough to wrap
    // several times, the total ticks reported must still equal the analytic
    // count -- wrapping changes WHICH tick indices appear, never HOW MANY
    // boundaries are crossed. An off-by-one at the seam is exactly the defect
    // this component would otherwise ship with, and it shows up here.
    transport.stop();
    transport.loop(0.0f, 1.0f);
    transport.looping(true);
    transport.play();
    uint32_t ticksLoop = countTicksOver(1200);
    float expectLoop = (float)AudioTransport::PPQN * 120.0f * 128.0f * 1200.0f
                       / (60.0f * AUDIO_SAMPLE_RATE_EXACT);
    float loopErr = (float)ticksLoop / expectLoop - 1.0f;
    float barsNow = transport.bars();
    CONSOLE.print("TR: ticks_loop=");  CONSOLE.println(ticksLoop);
    CONSOLE.print("TR: expect_loop="); CONSOLE.println(expectLoop, 2);
    CONSOLE.print("TR: bars_in_loop="); CONSOLE.println(barsNow, 4);
    bool loopOk = fabsf(loopErr) < 0.02f && barsNow >= 0.0f && barsNow < 1.0f;
    CONSOLE.println(loopOk ? "LOOP=PASS" : "LOOP=FAIL");

    // --- ELAPSED: across a wrap, song position wraps but audio time does not.
    // Without this, a refactor could collapse the two query families into one
    // and every other assertion here would still pass.
    uint64_t s0 = transport.samples();
    float    b0 = transport.bars();
    waitBlocks(900);                       // >1 loop length at this tempo
    uint64_t s1 = transport.samples();
    float    b1 = transport.bars();
    bool elapsedOk = s1 > s0 && b1 < 1.0f && b0 < 1.0f;
    CONSOLE.print("TR: samples_delta="); CONSOLE.println((uint32_t)(s1 - s0));
    CONSOLE.print("TR: bars_b0=");       CONSOLE.println(b0, 4);
    CONSOLE.print("TR: bars_b1=");       CONSOLE.println(b1, 4);
    CONSOLE.println(elapsedOk ? "ELAPSED=PASS" : "ELAPSED=FAIL");

    // --- TEMPOCHANGE: loop length in BARS survives a tempo change. This is
    // the property that motivated ticks-as-master, so it is asserted.
    transport.tempo(180.0f);
    waitBlocks(20);
    float barsAfter = transport.bars();
    uint32_t ticks180 = countTicksOver(400);
    float expect180 = (float)AudioTransport::PPQN * 180.0f * 128.0f * 400.0f
                      / (60.0f * AUDIO_SAMPLE_RATE_EXACT);
    bool changeOk = barsAfter >= 0.0f && barsAfter < 1.0f
                    && fabsf((float)ticks180 / expect180 - 1.0f) < 0.02f;
    CONSOLE.print("TR: bars_after_tempo="); CONSOLE.println(barsAfter, 4);
    CONSOLE.print("TR: ticks180=");         CONSOLE.println(ticks180);
    CONSOLE.println(changeOk ? "TEMPOCHANGE=PASS" : "TEMPOCHANGE=FAIL");

    // --- STATE: paused advances nothing; stop() rewinds.
    transport.tempo(120.0f);
    transport.pause();
    uint32_t pausedTicks = countTicksOver(200);
    float barsPaused = transport.bars();
    transport.play();
    waitBlocks(50);
    transport.stop();
    float barsStopped = transport.bars();
    bool stateOk = pausedTicks == 0 && barsStopped == 0.0f && barsPaused >= 0.0f;
    CONSOLE.print("TR: paused_ticks=");  CONSOLE.println(pausedTicks);
    CONSOLE.print("TR: bars_stopped=");  CONSOLE.println(barsStopped, 4);
    CONSOLE.println(stateOk ? "STATE=PASS" : "STATE=FAIL");

    CONSOLE.println("TRANSPORT-DONE");

    // Leave a musical loop running for the hardware session.
    transport.tempo(120.0f);
    transport.loop(0.0f, 1.0f);
    transport.looping(true);
    transport.play();
}

// --- the audible part: four-on-the-floor driven by the transport ------------
// One note per quarter = every AudioTransport::PPQN ticks. Reading the span
// each loop pass is enough for an audible demo; the sample-accurate path is the
// sequencer's job.
//
// ★ The tick indices the transport emits are ABSOLUTE musical ticks, and they
// must be folded into the loop before a quarter is derived from them. Across
// the seam of a 1-bar loop the raw stream runs ...382, 383, 384, 1, 2... -- so
// `idx / PPQN` alone yields FIVE distinct quarters per bar (0,1,2,3 and then 4
// for the single tick 384), firing two note-ons about 5 ms apart and flamming
// every downbeat. That is audible at the bench and is a defect of THIS mapping,
// not of the transport: 384 and 0 are the same musical position, and
// `idx % ticksPerLoop` says so exactly (384 % 384 == 0). The sequencer will
// need the same fold for the same reason.
//
// ticksPerLoop is hardcoded to one 4/4 bar because it must match the
// `transport.loop(0.0f, 1.0f)` call at the end of setup() above, and the
// transport exposes no getter for its loop bounds. Change one and you must
// change the other. A real sequencer needs a genuine answer here rather than a
// constant that agrees with a call site by hand.
void loop() {
    const uint32_t ticksPerLoop = AudioTransport::PPQN * 4;   // 1 bar at 4/4
    // Sentinel is outside 0..3, so the very first downbeat fires exactly once.
    static uint32_t lastQuarter = 0xFFFFFFFFu;
    static uint32_t lastBeat    = 0;
    // Snapshot the span under a brief IRQ guard: this runs in USER context,
    // and update() can retire the span mid-read. A sequencer would not need
    // this -- it runs inside update_all()'s serial walk, where the span is
    // stable by construction. Copying is cheap: the span holds <= 8 entries
    // and is normally 0-2.
    uint32_t idx[8]; int n;
    __disable_irq();
    n = transport.tickCount();
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) idx[i] = transport.tickAt(i);
    __enable_irq();
    for (int i = 0; i < n; i++) {
        // Fold into the loop FIRST, then derive the quarter: q is 0..3.
        uint32_t q = (idx[i] % ticksPerLoop) / AudioTransport::PPQN;
        if (q != lastQuarter) {
            lastQuarter = q;
            acid.noteOff(33);
            acid.noteOn(33, (q == 0) ? 127 : 90);   // accent the downbeat
        }
    }
    uint32_t now = millis();
    // Seed on first entry rather than leaving lastBeat at 0, which would make
    // heartbeat 1 fire on the very first loop() pass -- before the pattern has
    // produced a block, so it reads peak=0.0000 and the committed transcript
    // shows an unrepresentative first line. Same fix as acid_bass_test.
    if (lastBeat == 0) lastBeat = now;
    if (now - lastBeat >= 250) {
        lastBeat = now;
        if (peak.available()) {
            CONSOLE.print("TR_ALIVE bar="); CONSOLE.print(transport.barNumber());
            CONSOLE.print(" beat=");        CONSOLE.print(transport.beatInBar());
            CONSOLE.print(" peak=");        CONSOLE.println(peak.read(), 4);
        } else {
            CONSOLE.println("TR_ALIVE peak=(no update)");
        }
    }
}
