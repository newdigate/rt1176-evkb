#include "Arduino.h"
#include "HardwareSerial.h"
#include <math.h>
#include <Audio.h>
#include "control_wm8962.h"   // Audio.h includes control_wm8960.h but NOT this one

#define CONSOLE Serial1

// ★ No `#define PPQN` here. transport.h declares the resolution as a class
// constant and a macro of that name would rewrite the library's own
// declaration into `static const uint32_t 96 = 96;`. Use AudioTransport::PPQN.

// The sequencer takes the transport by reference, so `transport` MUST be
// constructed first -- which is exactly the update-order requirement the
// transport documents, made structural. You cannot write these two lines in
// the wrong order and have it compile.
AudioTransport     transport;
AudioStepSequencer seq(transport);
AudioSynthAcidBass acid;
AudioAnalyzePeak   peak;
AudioOutputI2S     out;
AudioConnection    cPk(acid, 0, peak, 0);
AudioConnection    cL (acid, 0, out,  0);
AudioConnection    cR (acid, 0, out,  1);
AudioControlWM8962 wm;

// ★ HOW LONG EACH MEASUREMENT RUNS, and why this number and not another.
//
// One pattern is 384 ticks. At 120 BPM / 96 PPQN a block advances
// PPQN*bpm*128/(60*sr) = 24576/44100 = 0.5571428 ticks, so a pattern is
// exactly 384*44100/24576 = 689.0625 blocks.
//
// The obvious choice, 2 patterns = 1378 blocks, is WRONG here, and the reason
// is a property of the transport rather than of this sketch:
// AudioTransport::emit() records boundaries strictly inside (from, to], so
// tick 0 is NOT emitted at phase 0 (transport.cpp says so explicitly). Step 0
// therefore only ever fires via the folded tick 0 that the loop seam produces
// when ABSOLUTE tick 384 is crossed -- i.e. once per wrap, never at the start
// of a run. 1378 blocks is 767.74 ticks: one wrap, not two, and 0.26 ticks shy
// of the second. Every count below would have sat on a knife edge, and the
// WRAP assertion ("two patterns must give two note-ons") would have been
// unreachable.
//
// 1500 blocks is 835.71 ticks: 2 wraps, 8 note-ons from a 4-step pattern, and
// the nearest event boundary in either direction is ~50 blocks away, so the
// one-block race between play() and the first counted block cannot move any
// count at all.
static const int RUN_BLOCKS = 1500;

// ---- event capture -------------------------------------------------------
// The gate needs to inspect the ORDER of emitted events, not just their count,
// so the drainer records them into a ring before applying them. Recording and
// applying in one pass is deliberate: it is exactly what a real consumer does,
// so the log cannot drift from what the voice actually heard.
struct LogEntry { uint8_t type, note, velocity; bool slide; uint16_t offset; };
static const int LOG_MAX = 256;
static LogEntry logBuf[LOG_MAX];
static volatile int logCount = 0;

// ★ AT MOST ONCE PER AUDIO BLOCK. The queue is not consumed by reading it:
// update() clears eventCount_ at the top of each block and refills it, so it
// keeps reporting the same events until the next block arrives. A drainer
// called more often than that -- which loop() does, thousands of times per
// block -- would re-log and RE-APPLY every event, retriggering the voice's
// envelope for the whole 2.9 ms of the block. Gating on the transport's own
// sample counter is the audio-clock way to say "once per block"; it leaves
// runBlocks() below (which already only calls in on a block change) exactly as
// it was.
static void drainSequencer(void) {
    static uint64_t lastDrained = 0;
    uint64_t now = transport.samples();
    if (now == lastDrained) return;
    lastDrained = now;

    SeqEvent ev[8]; int n;
    __disable_irq();                     // user context: update() can retire the queue
    n = seq.eventCount();
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) ev[i] = seq.eventAt(i);
    __enable_irq();
    for (int i = 0; i < n; i++) {
        if (logCount < LOG_MAX) {
            logBuf[logCount].type     = ev[i].type;
            logBuf[logCount].note     = ev[i].note;
            logBuf[logCount].velocity = ev[i].velocity;
            logBuf[logCount].slide    = ev[i].slide;
            logBuf[logCount].offset   = ev[i].sampleOffset;
            logCount++;
        }
        if (ev[i].type == SEQ_NOTE_ON) acid.noteOn(ev[i].note, ev[i].velocity, ev[i].slide);
        else                           acid.noteOff(ev[i].note);
    }
}

// Advance `n` audio blocks, draining every block. Block-referenced, not
// wall-clock: transport.samples() steps by exactly AUDIO_BLOCK_SAMPLES per
// update(), so a change in it IS a block tick. A delay()-based loop here would
// make every measurement below depend on how fast the host runs the guest.
static void runBlocks(int n) {
    uint64_t prev = transport.samples();
    int seen = 0;
    uint32_t t0 = millis();
    while (seen < n && millis() - t0 < 20000) {
        uint64_t s = transport.samples();
        if (s != prev) { seen++; prev = s; drainSequencer(); }
        yield();
    }
}

static void resetLog(void) { __disable_irq(); logCount = 0; __enable_irq(); }
static int  countType(uint8_t t) {
    int c = 0; for (int i = 0; i < logCount; i++) if (logBuf[i].type == t) c++; return c;
}

void setup() {
    CONSOLE.begin(115200);
    while (!CONSOLE) {}
    CONSOLE.println("STEPSEQ-GATE v1");
    AudioMemory(16);
    wm.enable();
    wm.volume(0.6f);
    acid.cutoff(700.0f); acid.resonance(0.7f); acid.envMod(0.6f);
    acid.decay(0.15f); acid.level(0.7f);

    transport.tempo(120.0f);
    transport.loop(0.0f, 1.0f);          // 384 ticks == the pattern length
    transport.looping(true);

    // --- STEPS: a known pattern emits exactly the expected note-ons ---------
    // 4 gated steps out of 16. RUN_BLOCKS is 835.71 ticks (see above), which
    // crosses folded 0 twice (absolute 384 and 768), folded 96 twice, folded
    // 192 twice and folded 288 twice: 8 note-ons. Both sides are audio-clock
    // quantities -- the block count is counted, and the expectation is derived
    // from ticks -- so host speed cannot move it.
    seq.clear();
    seq.step(0,  33, true,  true,  false);   // accented
    seq.step(4,  33, true,  false, false);
    seq.step(8,  36, true,  false, false);
    seq.step(12, 33, true,  false, false);
    transport.stop(); resetLog(); transport.play();
    runBlocks(RUN_BLOCKS);
    int onsA  = countType(SEQ_NOTE_ON);
    int offsA = countType(SEQ_NOTE_OFF);
    CONSOLE.print("SQ: ons=");  CONSOLE.println(onsA);
    CONSOLE.print("SQ: offs="); CONSOLE.println(offsA);
    // 8 expected; allow +/-1 for where the run starts and stops relative to a
    // step boundary. Nothing wider: a step-derivation bug moves this by 4x.
    bool stepsOk = (onsA >= 7 && onsA <= 9) && (offsA >= 6 && offsA <= 9);
    CONSOLE.println(stepsOk ? "STEPS=PASS" : "STEPS=FAIL");

    // --- ACCENT: accented steps carry 127, unaccented 80 -------------------
    int v127 = 0, v80 = 0, vOther = 0;
    for (int i = 0; i < logCount; i++) {
        if (logBuf[i].type != SEQ_NOTE_ON) continue;
        if      (logBuf[i].velocity == 127) v127++;
        else if (logBuf[i].velocity == 80)  v80++;
        else                                vOther++;
    }
    CONSOLE.print("SQ: v127="); CONSOLE.print(v127);
    CONSOLE.print(" v80=");     CONSOLE.print(v80);
    CONSOLE.print(" vother=");  CONSOLE.println(vOther);
    // One accented step in four, so roughly a quarter of the note-ons.
    bool accentOk = v127 >= 1 && v80 >= 3 && vOther == 0;
    CONSOLE.println(accentOk ? "ACCENT=PASS" : "ACCENT=FAIL");

    // --- ORDER: a slide step emits the next NOTE_ON before the NOTE_OFF ----
    // This is the load-bearing one. It inspects emission ORDER directly, and
    // it is the property the whole slide feature rests on: the voice glides
    // only if the new note is pressed while the old one is still held.
    // Pattern: step 0 slides into step 4.
    seq.clear();
    seq.step(0, 33, true, false, true);      // slide
    seq.step(4, 45, true, false, false);
    transport.stop(); resetLog(); transport.play();
    runBlocks(RUN_BLOCKS);
    // ★ ANCHOR ON THE FIRST NOTE_ON OF 33, not on the first NOTE_ON of 45.
    // Step 0 does not fire until the first wrap (tick 0 is never emitted at
    // phase 0 -- see RUN_BLOCKS), so the run legitimately OPENS with step 4's
    // note 45 sounding on its own, with no slide flag and nothing held. That
    // first 45 says nothing about slide behaviour; the one that matters is the
    // 45 that follows a sounding 33. Anchoring here is what makes the three
    // indices below describe one slide, rather than three unrelated events.
    int idx33 = -1;
    for (int i = 0; i < logCount; i++)
        if (logBuf[i].type == SEQ_NOTE_ON && logBuf[i].note == 33) { idx33 = i; break; }
    int idx45 = -1;
    for (int i = idx33 + 1; idx33 >= 0 && i < logCount; i++)
        if (logBuf[i].type == SEQ_NOTE_ON && logBuf[i].note == 45) { idx45 = i; break; }
    bool slideFlag = (idx45 >= 0) && logBuf[idx45].slide;
    // The FIRST note-off for 33 after it sounds must come AFTER the note-on
    // for 45, not before. Taking the first one (rather than the first after
    // idx45) is deliberate: an implementation that released 33 early would put
    // a NOTE_OFF between idx33 and idx45, and this is what sees it.
    int idxOff33 = -1;
    for (int i = idx33 + 1; idx33 >= 0 && i < logCount; i++)
        if (logBuf[i].type == SEQ_NOTE_OFF && logBuf[i].note == 33) { idxOff33 = i; break; }
    CONSOLE.print("SQ: idx_on33=");  CONSOLE.println(idx33);
    CONSOLE.print("SQ: idx_on45=");  CONSOLE.println(idx45);
    CONSOLE.print("SQ: idx_off33="); CONSOLE.println(idxOff33);
    CONSOLE.print("SQ: slide_flag="); CONSOLE.println(slideFlag ? 1 : 0);
    bool orderOk = (idx33 >= 0) && (idx45 >= 0) && (idxOff33 > idx45) && slideFlag;
    CONSOLE.println(orderOk ? "ORDER=PASS" : "ORDER=FAIL");

    // --- REST: a gate=false step is silent, and the step AFTER it fires -----
    // An off-by-one in step indexing breaks precisely this and nothing else.
    seq.clear();
    seq.step(0, 33, true,  false, false);
    seq.step(4, 40, false, false, false);    // REST -- must not sound
    seq.step(8, 45, true,  false, false);    // must still fire
    transport.stop(); resetLog(); transport.play();
    runBlocks(RUN_BLOCKS);
    int n33 = 0, n40 = 0, n45 = 0;
    for (int i = 0; i < logCount; i++) {
        if (logBuf[i].type != SEQ_NOTE_ON) continue;
        if (logBuf[i].note == 33) n33++;
        if (logBuf[i].note == 40) n40++;
        if (logBuf[i].note == 45) n45++;
    }
    CONSOLE.print("SQ: n33="); CONSOLE.print(n33);
    CONSOLE.print(" n40=");    CONSOLE.print(n40);
    CONSOLE.print(" n45=");    CONSOLE.println(n45);
    bool restOk = (n40 == 0) && (n33 >= 1) && (n45 >= 1);
    CONSOLE.println(restOk ? "REST=PASS" : "REST=FAIL");

    // --- WRAP: the pattern restarts at step 0 on a loop wrap ---------------
    // Only step 0 is gated, so every note-on is one pattern apart -- and since
    // folded tick 0 is reached ONLY through the seam, each note-on here IS a
    // wrap. RUN_BLOCKS crosses the seam twice, so this must read 2; if the
    // wrap reset the fold wrongly it reads 1 or 3.
    seq.clear();
    seq.step(0, 33, true, false, false);
    transport.stop(); resetLog(); transport.play();
    runBlocks(RUN_BLOCKS);
    int wrapOns = countType(SEQ_NOTE_ON);
    CONSOLE.print("SQ: wrap_ons="); CONSOLE.println(wrapOns);
    bool wrapOk = (wrapOns >= 2 && wrapOns <= 3);
    CONSOLE.println(wrapOk ? "WRAP=PASS" : "WRAP=FAIL");

    // --- SHORTLOOP: a loop shorter than the pattern leaves the tail unused --
    // Pins the fold's defined behaviour: steps beyond the loop NEVER fire.
    // Without this a future change could turn "unused tail" into "compressed
    // pattern" and every other assertion here would still pass.
    // Loop = 0.5 bar = 192 ticks = steps 0..7 only.
    seq.clear();
    seq.step(0,  33, true, false, false);     // inside the loop
    seq.step(12, 50, true, false, false);     // OUTSIDE it -- must never sound
    transport.stop();
    transport.loop(0.0f, 0.5f);
    resetLog(); transport.play();
    runBlocks(RUN_BLOCKS);
    int inLoop = 0, outLoop = 0;
    for (int i = 0; i < logCount; i++) {
        if (logBuf[i].type != SEQ_NOTE_ON) continue;
        if (logBuf[i].note == 33) inLoop++;
        if (logBuf[i].note == 50) outLoop++;
    }
    CONSOLE.print("SQ: in_loop=");  CONSOLE.print(inLoop);
    CONSOLE.print(" out_loop=");    CONSOLE.println(outLoop);
    // 835.71 ticks over a 192-tick loop is 4 seams, so 4 note-ons; >= 3 leaves
    // room for the start race without accepting "the loop never wrapped".
    bool shortOk = (outLoop == 0) && (inLoop >= 3);
    CONSOLE.println(shortOk ? "SHORTLOOP=PASS" : "SHORTLOOP=FAIL");

    CONSOLE.println("STEPSEQ-DONE");

    // Leave a real acid pattern running for the hardware session.
    transport.stop();
    transport.loop(0.0f, 1.0f);
    seq.clear();
    seq.step(0,  33, true,  true,  false);
    seq.step(2,  33, true,  false, false);
    seq.step(3,  45, true,  false, true );    // slide into step 4
    seq.step(4,  36, true,  true,  false);
    seq.step(6,  33, true,  false, false);
    seq.step(8,  40, true,  false, true );    // slide into step 9
    seq.step(9,  33, true,  false, false);
    seq.step(11, 31, true,  true,  false);
    seq.step(14, 33, true,  false, false);
    transport.play();
}

void loop() {
    static uint32_t lastBeat = 0;
    drainSequencer();
    uint32_t now = millis();
    if (lastBeat == 0) lastBeat = now;       // seed: else heartbeat 1 predates any audio
    if (now - lastBeat >= 250) {
        lastBeat = now;
        if (peak.available()) {
            CONSOLE.print("SQ_ALIVE step="); CONSOLE.print(seq.currentStep());
            CONSOLE.print(" beat=");         CONSOLE.print(transport.beatInBar());
            CONSOLE.print(" peak=");         CONSOLE.println(peak.read(), 4);
        } else {
            CONSOLE.println("SQ_ALIVE peak=(no update)");
        }
    }
}
