/* acid_box - the audio+display integration capstone.
 * Spec: docs/superpowers/specs/2026-08-17-acid-box-capstone-design.md
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Audio core (previous commit) + layout-A UI and the glue between them.  The
 * glue is deliberately THIN: every callback reads the widget, maps it, and
 * writes the engine; no UI state mirrors engine state except the three
 * `shown*` caches, which exist only to stop LVGL repainting unchanged text.
 *
 * BOOT STATE IS STOPPED AND SILENT, AND THAT IS A CONTRACT, NOT AN OMISSION.
 * The transport is configured (tempo, loop, looping) but never played, so
 * AudioStepSequencer::currentStep() stays at its -1 "nothing has played yet"
 * sentinel, the note pump has nothing to drain and audio_probe_poll() returns
 * before it can print.  The ABSENCE of ACIDBOX_BAR lines is therefore the
 * assertion that the box came up quiet; a build that hums on power-up shows up
 * as bars appearing where the gate expects none, not as a subjective judgement
 * about a transcript.
 */
#include <Arduino.h>
#include <string.h>               // memset -- named explicitly, as the sibling
                                  // audio examples name <math.h>, rather than
                                  // relying on Arduino.h to drag it in.
#include <math.h>                 // powf/logf/roundf/lroundf for the knob maps
#include <stdio.h>                // snprintf -- INTEGER conversions only, below
#include <Audio.h>
// Audio.h pulls in every codec driver EXCEPT this one -- control_wm8960.h is
// in its include list and control_wm8962.h is not, so the WM8962 header must be
// named explicitly, exactly as acid_bass_test and audiooutput_i2s_test do.
#include "control_wm8962.h"
#include <Wire.h>                 // Wire2 = LPI2C5: codec AND touch controller
#include "Display.h"
#include "gt911.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "lvgl_gt911_indev.h"
#include "synthui_rotary_knob.h"
#include "synthui_step.h"

// rt1176-only example: LPUART1 console, which the imxrt1176 core names Serial1.
#define CONSOLE Serial1

/* --- audio graph ---------------------------------------------------------- *
 * ★ DECLARATION ORDER IS UPDATE ORDER and both the transport and the
 * sequencer depend on it (AudioStream.h appends at the tail; software_isr
 * walks head-first).  `transport` must precede `seq` -- the constructor
 * signature makes that structural rather than advisory -- and `acid` must
 * precede `rms` and `out` so the analyzer and the codec see the block the
 * voice just produced rather than the previous one. */
AudioTransport      transport;
AudioStepSequencer  seq(transport);
AudioSynthAcidBass  acid;
AudioAnalyzeRMS     rms;
AudioOutputI2S      out;
AudioControlWM8962  wm;
AudioConnection     cRms(acid, 0, rms, 0);
AudioConnection     cL(acid, 0, out, 0);
AudioConnection     cR(acid, 0, out, 1);

/* --- the preset: a classic 16-step acid line (A minor-ish), documented so
 * the first frame and the audio windows are deterministic.  note 0 = rest. */
struct PresetStep { uint8_t note; bool gate, accent, slide; };
static const PresetStep kPreset[16] = {
    { 33, true,  true,  false },   /* 0  A1 accent          */
    { 33, true,  false, false },   /* 1  A1                 */
    {  0, false, false, false },   /* 2  rest  <- the gate's edit target */
    { 45, true,  false, true  },   /* 3  A2 slide into 4    */
    { 36, true,  false, false },   /* 4  C2                 */
    {  0, false, false, false },   /* 5  rest               */
    { 33, true,  false, false },   /* 6  A1                 */
    { 40, true,  true,  false },   /* 7  E2 accent          */
    { 33, true,  false, false },   /* 8  A1                 */
    {  0, false, false, false },   /* 9  rest               */
    { 43, true,  false, true  },   /* 10 G2 slide into 11   */
    { 45, true,  false, false },   /* 11 A2                 */
    { 33, true,  true,  false },   /* 12 A1 accent          */
    { 31, true,  false, false },   /* 13 G1                 */
    {  0, false, false, false },   /* 14 rest               */
    { 33, true,  false, true  },   /* 15 A1 slide into 0    */
};

/* USER CONTEXT ONLY: seq.step() takes __disable_irq() guards internally
 * (seq_step.h), which is only safe from a context the audio ISR can preempt. */
static void load_preset(void)
{
    for (int i = 0; i < 16; i++)
        seq.step(i, kPreset[i].note, kPreset[i].gate,
                 kPreset[i].accent, kPreset[i].slide);
}

/* --- default patch: the boot angles in §4 of the spec map to these -------- */
static void default_patch(void)
{
    acid.waveform(WAVEFORM_SAWTOOTH);
    acid.cutoff(800.0f);
    acid.resonance(0.55f);
    acid.envMod(0.6f);
    acid.decay(0.28f);
    acid.accent(0.7f);
    acid.distortion(0.15f);
    acid.subLevel(0.2f);
    acid.slideTime(0.06f);
    acid.level(0.5f);              /* fixed; no knob (spec §4) */
}

/* --- DIAGNOSTIC SCAFFOLDING, build-diag/ ONLY ---------------------------- *
 * Compiled in only under -DACIDBOX_DIAG=1, which the shipped build/ (and so the
 * gate, and so the golden ELF) never sets.  It exists to answer one question
 * over SWD while the MCU-Link VCOM is dead: WHERE IN setup() IS THE FIRMWARE AT
 * A GIVEN MILLISECOND.  A debugger halt reports a frozen systick that is
 * INDISTINGUISHABLE from a firmware freeze, so the only way to tell "died at t"
 * from "was halted at t" is to know what setup() is legitimately doing at t.
 * Everything here is volatile and read by symbol; nothing prints, because the
 * VCOM is a separate bench fault. */
#ifdef ACIDBOX_DIAG
volatile uint32_t g_diag_t[16];        /* setup() checkpoint timestamps, ms */
volatile uint32_t g_diag_n;            /* how many checkpoints have been passed */
volatile float    g_diag_rms[16];      /* per-step peak RMS, NEVER cleared */
volatile uint32_t g_diag_bars;         /* bars completed since the diag play() */
volatile uint32_t g_diag_played;       /* millis() at the synthetic play()      */
static inline void diag_mark(void)
{ if (g_diag_n < 16) g_diag_t[g_diag_n++] = millis(); }
#else
#define diag_mark() ((void)0)
#endif

/* --- note-event pump: PIT context, immune to UI frame time ---------------- *
 *
 * WHY AN ISR AT ALL: the drain must not be delayed by an LVGL frame, and a
 * full-screen software render on the RK055 is tens of milliseconds -- far
 * longer than the 2.9 ms window in which the sequencer's event queue is valid.
 * Draining from loop() would lose whole steps whenever the UI redrew.
 *
 * ★ PRIORITY 224, BELOW THE AUDIO SOFTWARE ISR'S 208, AND THAT IS THE WHOLE
 * DESIGN.  IntervalTimer defaults to 128, which is HIGHER priority than
 * AudioStream's software_isr (AudioStream.cpp sets IRQ_SOFTWARE to 208), so a
 * default-priority pump can land in the middle of one update_all() pass --
 * specifically between transport.update() and seq.update(), which run
 * adjacently in construction order.  In that window samples() has already
 * advanced while the queue still holds the PREVIOUS block, so the "new block"
 * test below would fire, re-apply stale events, and then mark the new block
 * drained: the real note-ons of that block would be silently dropped.  The
 * window is a microsecond wide and the pump fires 1000 times a second, so it
 * is rare, nondeterministic, and exactly the class of defect this tree refuses
 * to ship.  Running BELOW the audio ISR removes it outright -- software_isr can
 * never be preempted by this handler, so this handler can never observe a
 * half-finished pass.
 */
IntervalTimer pump;
static void pump_isr(void)
{
    /* ★ ONCE PER AUDIO BLOCK, NOT ONCE PER TIMER TICK.  seq_step.h is explicit
     * that reading the queue does not consume it: update() clears it at the top
     * of each block and refills it, so it keeps reporting the same events until
     * the next block arrives.  At 1 kHz against a 344.5 Hz block rate every
     * event would otherwise be applied about three times, retriggering the
     * voice's envelope for the whole 2.9 ms of the block.  The transport's own
     * sample counter is the audio-clock way to say "a new block has arrived",
     * and it is the same guard drainSequencer() uses in step_seq_test. */
    static uint64_t lastDrained = 0;
    SeqEvent ev[AudioStepSequencer::MAX_EVENTS];
    int n = 0;

    /* ★ ONE CRITICAL SECTION COVERING BOTH READS.  software_isr (208) preempts
     * this handler (224), so reading samples() and the queue as two separate
     * steps could straddle a block boundary: the counter from the old block and
     * the events from the new one, which double-applies now and drops later.
     * Taking both under one __disable_irq() makes the pair atomic. */
    __disable_irq();
    const uint64_t now = transport.samples();
    if (now != lastDrained) {
        lastDrained = now;
        n = seq.eventCount();
        if (n > AudioStepSequencer::MAX_EVENTS) n = AudioStepSequencer::MAX_EVENTS;
        for (int i = 0; i < n; i++) ev[i] = seq.eventAt(i);
    }
    __enable_irq();

    /* Applied OUTSIDE the critical section on purpose: noteOn/noteOff take
     * their own __disable_irq() guards (synth_acidbass.h), and holding
     * interrupts off across up to eight voice updates would delay the audio ISR
     * this handler exists to serve. */
    for (int i = 0; i < n; i++) {
        if (ev[i].type == SEQ_NOTE_ON) acid.noteOn(ev[i].note, ev[i].velocity, ev[i].slide);
        else                           acid.noteOff(ev[i].note);
    }
}

/* --- per-step RMS windows, referenced to the SEQUENCER's own position ----- *
 *
 * Float DSP is asserted by measured windows with margin, never by bit-goldens
 * (the acid_bass_test idiom) -- and the window boundaries come from the
 * sequencer's step index rather than from millis(), so the table below is a
 * property of the audio clock and cannot move with host speed.  All state here
 * is touched from loop() only.
 *
 * ★ BAR 1 IS NOT A VALID WINDOW FOR STEP 0 -- assert from bar 2 onwards.
 * The transport never emits tick 0 at phase 0 (transport.cpp records boundaries
 * strictly inside (from, to]), so step 0 first fires at the loop seam and its
 * bar-1 window opens part-way through the note.  Measured under a throwaway
 * play(): step 0 reads 0.1843 in bar 1 and 0.42-0.43 in every bar after it,
 * while the accented steps 0/7/12 sit at ~0.42 against ~0.37-0.39 for the plain
 * gated steps and the four rests (2, 5, 9, 14) read ~0.0002.  Those are the
 * numbers the gate's windows should be built from, and the bar-1 outlier is a
 * property of the transport, not a startup transient that settles. */
static float    stepPeakRms[16];
static int      lastSeenStep = -1;
static uint32_t barsDone = 0;
static void audio_probe_poll(void)
{
    /* -1 until the first step fires.  While the transport is stopped this is
     * the only statement that runs, which is what makes the boot state silent
     * on the wire as well as in the headphones. */
    const int s = seq.currentStep();
    if (s < 0) return;
    if (rms.available()) {
        const float v = rms.read();
        if (v > stepPeakRms[s]) stepPeakRms[s] = v;
#ifdef ACIDBOX_DIAG
        /* Same peak, but NEVER cleared at the bar seam, so an SWD read that
         * lands mid-bar still sees a whole measured table. */
        if (v > g_diag_rms[s]) g_diag_rms[s] = v;
#endif
    }
    if (s != lastSeenStep) {
        /* 15 -> 0 is the loop seam.  Anchoring on the seam rather than on
         * "s == 0" means a bar is only reported once the whole 16-step table
         * has been filled, so no line can carry a half-measured window. */
        if (s == 0 && lastSeenStep == 15) {
            barsDone++;
#ifdef ACIDBOX_DIAG
            g_diag_bars = barsDone;
#endif
            CONSOLE.printf("ACIDBOX_BAR=%lu RMS=[", (unsigned long)barsDone);
            /* ★ print(float, digits), NOT printf("%.4f"), AND THAT IS NOT A
             * STYLE CHOICE. Print::printf goes through newlib's vsnprintf, and
             * this tree links the INTEGER-ONLY formatter: in the ELF
             * _svfprintf_r and _svfiprintf_r resolve to the same address and
             * _dtoa_r is absent entirely. A "%.4f" therefore emits a NUL byte
             * rather than digits -- measured here, not assumed, and it is why
             * no other example in this tree formats a float with printf.
             * printFloat() is the core's own converter and needs no libc.
             *
             * ★ Note how nearly this hid: the boot state is STOPPED, so this
             * whole branch is unreachable in the shipped configuration. It only
             * surfaced under a throwaway transport.play(). Checking that the
             * ELF merely CONTAINS a symbol named _svfprintf_r would have
             * "confirmed" the opposite -- the name is an alias. */
            for (int i = 0; i < 16; i++) {
                if (i) CONSOLE.print(',');
                CONSOLE.print(stepPeakRms[i], 4);
            }
            CONSOLE.println("]");
            memset(stepPeakRms, 0, sizeof(stepPeakRms));
        }
        lastSeenStep = s;
    }
}

/* --- UI ------------------------------------------------------------------- *
 *
 * Layout A (spec §3.2) on the RK055's 720x1280 portrait frame: a transport bar,
 * a 2x4 grid of sound knobs, the selected-step editor strip, and the 2x8 step
 * lane.  Everything is placed with absolute coordinates, and those coordinates
 * are the GATE's geometry as well as the picture's -- the touch script's tap
 * percentages are derived from the constants below, so moving a widget moves a
 * tap point.  Re-derive the three percentages in run_qemu.sh if any of the
 * placement arithmetic here changes.
 *
 * The screen still paints an OPAQUE ground, for the reason the stub did: it is
 * what makes every pixel of the frame defined, and therefore makes
 * ACIDBOX_UI_SUM a checksum of the scene rather than of whatever the allocator
 * left behind. */
static lv_obj_t *stepCell[16];
static lv_obj_t *playBtnLabel, *bpmLabel, *noteLabel, *accBtn, *sldBtn, *waveBtnLabel;
static lv_obj_t *pitchKnob;
static int selectedStep = 0;

/* knob -> parameter maps (spec §4).  Every knob is created with an EXPLICIT
 * -140..+140 range (the rotary widget's DC default is ±150), so the whole
 * sweep is 280 degrees and t lands in [0,1]; the input layer clamps the drag
 * to that range, so knob01() needs no clamp of its own. */
static inline float knob01(lv_obj_t *k)
{ return (synthui_rotary_knob_get_angle(k) + 140.0f) / 280.0f; }
static inline float expmap(float t, float lo, float hi)
{ return lo * powf(hi / lo, t); }

/* Pitch map: 25 semitones C1(24)..C3(48), one detent each, so detent_step is
 * 280/24 -- 24 intervals between 25 stops, not 25.  The knob snaps onto the
 * lattice anchored at min_deg (synthui_knob_math.h), which is exactly the
 * lattice these two functions assume. */
static inline uint8_t angleToNote(float deg)
{ return (uint8_t)(24 + (int)roundf((deg + 140.0f) / (280.0f / 24.0f))); }
static inline float noteToAngle(uint8_t note)
{
    /* Clamped because seq.step() accepts the full 0..127 MIDI range while this
     * knob only spans C1..C3: an out-of-range note would otherwise park the
     * pointer outside the drawn end stops, drawing a position the user cannot
     * reach and this map cannot round-trip. */
    if (note < 24) note = 24;
    if (note > 48) note = 48;
    return -140.0f + (float)(note - 24) * (280.0f / 24.0f);
}

/* One callback per sound knob.  CUTOFF, DECAY and SLIDE T are exponential
 * because they are frequency and time; the five 0..1 amounts are linear. */
static void cbRes(lv_event_t *e){ acid.resonance (knob01((lv_obj_t *)lv_event_get_target(e))); }
static void cbEnv(lv_event_t *e){ acid.envMod    (knob01((lv_obj_t *)lv_event_get_target(e))); }
static void cbAcc(lv_event_t *e){ acid.accent    (knob01((lv_obj_t *)lv_event_get_target(e))); }
static void cbDst(lv_event_t *e){ acid.distortion(knob01((lv_obj_t *)lv_event_get_target(e))); }
static void cbSub(lv_event_t *e){ acid.subLevel  (knob01((lv_obj_t *)lv_event_get_target(e))); }
static void cbDec(lv_event_t *e){ acid.decay    (expmap(knob01((lv_obj_t *)lv_event_get_target(e)), 0.03f, 2.0f)); }
static void cbSld(lv_event_t *e){ acid.slideTime(expmap(knob01((lv_obj_t *)lv_event_get_target(e)), 0.01f, 0.3f)); }

/* Cutoff also prints, because the gate's drag assertion needs a value it can
 * order.  ★ print(float, digits), NOT printf("%.1f") -- see the identical note
 * over the RMS table above: this tree links newlib's INTEGER-ONLY formatter and
 * a %f conversion emits a NUL byte instead of digits, which would truncate the
 * line and leave the gate parsing "CUTOFF=" with nothing after it. */
static void cbCut(lv_event_t *e)
{
    const float hz = expmap(knob01((lv_obj_t *)lv_event_get_target(e)), 20.0f, 12000.0f);
    acid.cutoff(hz);
    CONSOLE.print("CUTOFF=");
    CONSOLE.println(hz, 1);
}

static const char *noteName(uint8_t n)
{
    static const char *N[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static char buf[8];
    /* %s and %d only: integer conversions are all the linked formatter has. */
    snprintf(buf, sizeof buf, "%s%d", N[n % 12], (int)(n / 12) - 1);
    return buf;
}

/* Write the selected step's FULL state back as ONE seq.step() call and refresh
 * its cell and the editor readouts.  The single write is the atomic transaction
 * of spec §3.3: seq.step() takes its own __disable_irq() guard internally, so a
 * four-field store is indivisible against the audio ISR, while four separate
 * read-modify-writes would let the sequencer observe a half-edited step. */
static void commit_selected(uint8_t note, bool gate, bool accent, bool slide)
{
    seq.step(selectedStep, note, gate, accent, slide);
    synthui_step_set(stepCell[selectedStep], gate, accent, slide);
    lv_label_set_text(noteLabel, gate ? noteName(note) : "--");
    lv_obj_set_style_bg_color(accBtn, accent ? lv_color_hex(0x5b62b8) : lv_color_hex(0x232b3a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sldBtn, slide  ? lv_color_hex(0x5b62b8) : lv_color_hex(0x232b3a), LV_PART_MAIN);
    CONSOLE.printf("STEP[%d]=note%u gate%d acc%d sld%d\n",
                   selectedStep, (unsigned)note,
                   gate ? 1 : 0, accent ? 1 : 0, slide ? 1 : 0);
}

/* Pure VIEW change: moves the editor strip onto step i without touching the
 * pattern, so it emits no STEP token.  A rest has note 0 in the preset, which
 * is not a pitch the knob can show -- park it on A1 so the first pitch edit
 * after un-resting starts somewhere musical. */
static void select_step(int i)
{
    synthui_step_set_selected(stepCell[selectedStep], false);
    selectedStep = i;
    synthui_step_set_selected(stepCell[i], true);
    const AcidStep st = seq.step(i);
    synthui_rotary_knob_set_angle(pitchKnob, noteToAngle(st.note ? st.note : 33));
    lv_label_set_text(noteLabel, st.gate ? noteName(st.note) : "--");
    lv_obj_set_style_bg_color(accBtn, st.accent ? lv_color_hex(0x5b62b8) : lv_color_hex(0x232b3a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sldBtn, st.slide  ? lv_color_hex(0x5b62b8) : lv_color_hex(0x232b3a), LV_PART_MAIN);
}

/* A tap does BOTH: it selects the cell for editing and toggles its gate.  One
 * gesture, because on a 16-cell lane a select-then-toggle pair would double
 * every pattern edit. */
static void cbStepTap(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    select_step(i);
    const AcidStep st = seq.step(i);
    commit_selected(st.note ? st.note : 33, !st.gate, st.accent, st.slide);
}
static void cbPitch(lv_event_t *e)
{
    const uint8_t note = angleToNote(synthui_rotary_knob_get_angle((lv_obj_t *)lv_event_get_target(e)));
    const AcidStep st = seq.step(selectedStep);
    commit_selected(note, st.gate, st.accent, st.slide);
}
static void cbAccBtn(lv_event_t *e)
{ (void)e; const AcidStep st = seq.step(selectedStep); commit_selected(st.note, st.gate, !st.accent, st.slide); }
static void cbSldBtn(lv_event_t *e)
{ (void)e; const AcidStep st = seq.step(selectedStep); commit_selected(st.note, st.gate, st.accent, !st.slide); }

static void cbPlay(lv_event_t *e)
{
    (void)e;
    if (transport.playing()) transport.pause(); else transport.play();
    CONSOLE.printf("PLAYING=%d\n", transport.playing() ? 1 : 0);
}
static void cbStop(lv_event_t *e)
{ (void)e; transport.stop(); CONSOLE.println("PLAYING=0"); }
static void cbTempoUp(lv_event_t *e)
{ (void)e; transport.tempo(transport.tempo() + 1.0f); }
static void cbTempoDn(lv_event_t *e)
{ (void)e; transport.tempo(transport.tempo() - 1.0f); }
static void cbWave(lv_event_t *e)
{
    (void)e;
    /* Mirrors default_patch()'s WAVEFORM_SAWTOOTH; the voice keeps the truth,
     * this only remembers which way to flip next. */
    static bool square = false;
    square = !square;
    acid.waveform(square ? WAVEFORM_SQUARE : WAVEFORM_SAWTOOTH);
    lv_label_set_text(waveBtnLabel, square ? "SQR" : "SAW");
}

/* 33 ms poller: cursor ring, play label, bpm readout (spec §3.3).
 *
 * ★ EVERY WRITE IS GUARDED BY A CHANGE TEST, AND NOT AS AN OPTIMISATION.
 * lv_label_set_text() reallocates and invalidates unconditionally, so an
 * unguarded poller would dirty two labels 30 times a second forever -- a
 * permanent full-rate repaint on a panel whose software render costs tens of
 * milliseconds.  The `shown*` caches start at values no reading can equal, so
 * the first call always paints. */
static int shownCursor  = -1;
static int shownPlaying = -1;
static int shownBpm10   = -1;
static void ui_poll(lv_timer_t *t)
{
    (void)t;
    /* Stopped means NO cursor, not "the cursor where it stopped": currentStep()
     * keeps its last index after pause(), and a ring left burning on a paused
     * box reads as a playhead that has stalled. */
    const bool play = transport.playing();
    const int  s    = play ? seq.currentStep() : -1;
    if (s != shownCursor) {
        if (shownCursor >= 0) synthui_step_set_cursor(stepCell[shownCursor], false);
        if (s >= 0)           synthui_step_set_cursor(stepCell[s], true);
        shownCursor = s;
    }
    if ((int)play != shownPlaying) {
        shownPlaying = (int)play;
        lv_label_set_text(playBtnLabel, play ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    /* One decimal, assembled from INTEGERS: see cbCut's note on %f.  tempo() is
     * clamped to 20..999 by the transport, so both halves stay non-negative. */
    const int bpm10 = (int)lroundf(transport.tempo() * 10.0f);
    if (bpm10 != shownBpm10) {
        shownBpm10 = bpm10;
        char b[16];
        snprintf(b, sizeof b, "%d.%d", bpm10 / 10, bpm10 % 10);
        lv_label_set_text(bpmLabel, b);
    }
}

static lv_obj_t *mkbtn(lv_obj_t *par, const char *txt, lv_event_cb_t cb,
                       lv_obj_t **labelOut)
{
    lv_obj_t *b = lv_button_create(par);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x232b3a), LV_PART_MAIN);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (labelOut) *labelOut = l;
    return b;
}
static lv_obj_t *mkknob(lv_obj_t *scr, int col, int row, const char *name,
                        float boot01, lv_event_cb_t cb)
{
    lv_obj_t *k = synthui_rotary_knob_create(scr);
    lv_obj_set_size(k, 150, 150);
    lv_obj_set_pos(k, 15 + col * 175, 90 + row * 185);
    synthui_rotary_knob_set_mode(k, SYNTHUI_ROTARY_MODE_BOUNDED);
    /* The DC default range is ±150; every angle<->param map in this file
     * hardcodes ±140, so the range is stated here instead of inherited. */
    synthui_rotary_knob_set_range(k, -140.0f, 140.0f);
    synthui_rotary_knob_set_angle(k, boot01 * 280.0f - 140.0f);
    lv_obj_add_event_cb(k, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, name);
    lv_obj_set_style_text_color(l, lv_color_hex(0x9aa0b8), LV_PART_MAIN);
    lv_obj_set_pos(l, 15 + col * 175 + 50, 90 + row * 185 + 152);
    return k;
}

static lv_obj_t *build_ui(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    /* ★ CROSS-AXIS SCROLL CHAINING, decided here (plan's Task-5 note).  The
     * knob clears SCROLL_CHAIN_VER permanently but leaves SCROLL_CHAIN_HOR on,
     * so a vertical drag that wanders far enough sideways can flip
     * lv_indev_find_scroll_obj() to the horizontal axis and let a scrollable
     * ancestor steal the press mid-turn (PRESS_LOST).  Nothing here is
     * horizontally scrollable, but "nothing scrolls today" is a property of the
     * layout and would evaporate the first time a widget is placed past the
     * right edge.  Clearing the flag on the screen makes it structural.  If a
     * knob is ever put inside a genuinely horizontally scrolling panel, port
     * lv_slider's pattern -- remove the CROSS-axis flag only once the drag is
     * established, restore it on RELEASED/PRESS_LOST -- rather than killing a
     * legitimate swipe gesture outright. */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* transport bar */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ACID BOX");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(title, 15, 24);
    lv_obj_set_pos(mkbtn(scr, "-", cbTempoDn, NULL), 300, 16);
    bpmLabel = lv_label_create(scr);
    lv_obj_set_style_text_color(bpmLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(bpmLabel, 356, 24);
    lv_obj_set_pos(mkbtn(scr, "+", cbTempoUp, NULL), 420, 16);
    lv_obj_t *play = mkbtn(scr, LV_SYMBOL_PLAY, cbPlay, &playBtnLabel);
    lv_obj_set_pos(play, 540, 16);          /* centre (575,40) = the gate's ▶ */
    lv_obj_set_size(play, 70, 48);
    lv_obj_t *stop = mkbtn(scr, LV_SYMBOL_STOP, cbStop, NULL);
    lv_obj_set_pos(stop, 626, 16);
    lv_obj_set_size(stop, 70, 48);

    /* Sound knobs.  Boot angles are the INVERSE of each map applied to
     * default_patch()'s values, so the first frame shows the patch the engine
     * is actually holding -- a knob drawn at a position its parameter is not at
     * would make the very first drag jump. */
    mkknob(scr, 0, 0, "CUTOFF",  logf(800.0f / 20.0f) / logf(12000.0f / 20.0f), cbCut);
    mkknob(scr, 1, 0, "RESO",    0.55f, cbRes);
    mkknob(scr, 2, 0, "ENV MOD", 0.60f, cbEnv);
    mkknob(scr, 3, 0, "DECAY",   logf(0.28f / 0.03f) / logf(2.0f / 0.03f), cbDec);
    mkknob(scr, 0, 1, "ACCENT",  0.70f, cbAcc);
    mkknob(scr, 1, 1, "DIST",    0.15f, cbDst);
    mkknob(scr, 2, 1, "SUB",     0.20f, cbSub);
    mkknob(scr, 3, 1, "SLIDE T", logf(0.06f / 0.01f) / logf(0.3f / 0.01f), cbSld);

    /* editor strip: pitch detent knob + note name + ACC/SLD toggles + SAW/SQR */
    pitchKnob = synthui_rotary_knob_create(scr);
    lv_obj_set_size(pitchKnob, 120, 120);
    lv_obj_set_pos(pitchKnob, 15, 470);
    /* detents are input behavior on the rotary widget (no visual mode):
     * bounded well + 24 semitone stops on the ±140 lattice the pitch maps
     * above assume. */
    synthui_rotary_knob_set_mode(pitchKnob, SYNTHUI_ROTARY_MODE_BOUNDED);
    synthui_rotary_knob_set_range(pitchKnob, -140.0f, 140.0f);
    synthui_rotary_knob_set_detent_step(pitchKnob, 280.0f / 24.0f);
    lv_obj_add_event_cb(pitchKnob, cbPitch, LV_EVENT_VALUE_CHANGED, NULL);
    noteLabel = lv_label_create(scr);
    lv_obj_set_style_text_color(noteLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(noteLabel, 150, 515);
    accBtn = mkbtn(scr, "ACC", cbAccBtn, NULL);
    lv_obj_set_pos(accBtn, 230, 505);
    sldBtn = mkbtn(scr, "SLD", cbSldBtn, NULL);
    lv_obj_set_pos(sldBtn, 340, 505);
    lv_obj_t *wave = mkbtn(scr, "SAW", cbWave, &waveBtnLabel);
    lv_obj_set_pos(wave, 560, 505);

    /* step lane: 2x8 of 82 px cells at 88 pitch, y = 640/736.  Cell 2's centre
     * is (225,681) -- the gate's edit target. */
    for (int i = 0; i < 16; i++) {
        lv_obj_t *c = synthui_step_create(scr);
        lv_obj_set_size(c, 82, 82);
        lv_obj_set_pos(c, 8 + (i % 8) * 88, 640 + (i / 8) * 96);
        synthui_step_set(c, kPreset[i].gate, kPreset[i].accent, kPreset[i].slide);
        lv_obj_add_event_cb(c, cbStepTap, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        stepCell[i] = c;
    }
    select_step(0);

    /* ★ RUN THE POLLER ONCE BEFORE ARMING THE TIMER, or the boot golden is a
     * race.  lv_timer_create() schedules the first callback one FULL period
     * away, while the caller renders as soon as the first lv_timer_handler()
     * returns; whether the 33 ms tick beat the first flush would then decide
     * whether ACIDBOX_UI_SUM covers a bpm readout saying "128.0" or the
     * lv_label default "Text".  Priming it here makes the first frame a
     * function of engine state and nothing else. */
    ui_poll(NULL);
    lv_timer_create(ui_poll, 33, NULL);
    return scr;
}

/* --- touch ---------------------------------------------------------------- *
 * D9 = GPIO_AD_01 = touch reset, D6 = GPIO_AD_00 = touch interrupt, both owned
 * by GT911::begin(); the INT line is never attached to, exactly as
 * lvgl_rk055_touch_test leaves it. */
static constexpr uint8_t TOUCH_RST_PIN = 9;
static constexpr uint8_t TOUCH_INT_PIN = 6;
static GT911 touch(Wire2, TOUCH_RST_PIN, TOUCH_INT_PIN);

void setup()
{
    CONSOLE.begin(115200);
    delay(200);
    CONSOLE.println("ACIDBOX_BEGIN");
    diag_mark();               /* after CONSOLE.begin + delay(200) */

    AudioMemory(24);
    diag_mark();               /* after AudioMemory(24) */
    const bool codec = wm.enable();
    wm.volume(0.6f);
    CONSOLE.println(codec ? "CODEC_OK" : "CODEC_FAIL");
    diag_mark();               /* after wm.enable() + volume */

    const bool panel = Display.begin();
    CONSOLE.println(panel ? "PANEL_OK" : "PANEL_FAIL");
    diag_mark();               /* after Display.begin() */
    if (!panel) {
        /* No lv_init() happened, so loop()'s lv_timer_handler() returns
         * immediately -- the same contract the sibling display examples use.
         * The transport is also never configured, so audio_probe_poll() stays
         * on its -1 early return and the run is silent in both senses. */
        CONSOLE.println("ACIDBOX_DONE");
        return;
    }

    lvgl_rt1176_begin();
    diag_mark();               /* after lvgl_rt1176_begin() */
    lv_display_t *disp = lvgl_mipi_panel_create(Display);
    diag_mark();               /* after lvgl_mipi_panel_create() */

    load_preset();
    default_patch();
    transport.tempo(128.0f);
    transport.loop(0.0f, 1.0f);        /* one bar == the 384-tick pattern */
    transport.looping(true);
    diag_mark();               /* after preset + patch + transport cfg */
    pump.priority(224);                /* BEFORE begin(): see pump_isr's note.
                                        * priority() applied afterwards would
                                        * leave a window running at 128. */
    pump.begin(pump_isr, 1000);        /* MICROseconds, Teensy convention: 1 kHz */
    diag_mark();               /* after pump.begin() */

    /* The scene is the FIRST refresh, so the sum below covers a whole-screen
     * paint rather than whatever a partial repaint touched. */
    lv_screen_load(build_ui());
    diag_mark();               /* after build_ui() + screen load */
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000)
        lvgl_rt1176_loop();
    lvgl_sum_reset();
    diag_mark();               /* after the frame_done wait loop */
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);
    diag_mark();               /* after the 3.6 MB checksum   == :591 pre-diag */
    CONSOLE.printf("ACIDBOX_UI_SUM=0x%08lX\n", (unsigned long)lvgl_sum_value());
    CONSOLE.printf("PLAYING=%d\n", transport.playing() ? 1 : 0);
    diag_mark();               /* after the two console printfs */

    /* Touch bring-up AFTER the golden, which is what keeps the golden a
     * statement about the scene alone: no indev exists yet, so no contact can
     * have moved a widget before the checksum was taken.  Wire2 is already open
     * (wm.enable() begins it -- it is the codec's bus too); re-begin()ing it is
     * lvgl_rk055_touch_test's proven sequence and states the ownership rather
     * than inheriting it by luck. */
    Wire2.begin();
    Wire2.setClock(400000);
    diag_mark();               /* after Wire2.begin()/setClock() == :602 pre-diag */
    const bool touchOk = touch.begin();
    diag_mark();               /* after touch.begin()          == :603 pre-diag */
    if (touchOk) {
        CONSOLE.println("I2C_OK");
        /* From here LVGL polls the part every 10 ms; in QEMU the model replays
         * a script, on the bench a finger drives it. */
        lvgl_gt911_indev_create(disp, touch);
    } else {
        /* Not fatal: the box keeps playing and drawing, it just cannot be
         * touched.  The gate's verdict is the ABSENCE of I2C_OK, so a silent
         * fallthrough is still caught -- but the diagnostics say which of the
         * three failure modes it was without a second run. */
        CONSOLE.printf("TOUCH_FAIL err=%s i2c=%u id=0x%08lX\n",
                       GT911::errorName(touch.lastError()),
                       (unsigned)touch.lastI2cStatus(),
                       (unsigned long)touch.lastDeviceId());
    }
    diag_mark();               /* setup() COMPLETE */
    CONSOLE.println("ACIDBOX_DONE");
}

void loop()
{
#ifdef ACIDBOX_DIAG
    /* Synthetic play, diagnostic build only.  The shipped contract is BOOT
     * SILENT and this must not be allowed to soften that claim -- so it lives
     * behind the definition build/ never sets, and it starts the transport
     * FOUR SECONDS IN, purely so the audio path can be MEASURED over SWD
     * (g_diag_rms) while the VCOM carries nothing. */
    if (!g_diag_played && millis() >= 4000) {
        g_diag_played = millis();
        transport.play();
    }
#endif
    lvgl_rt1176_loop();
    audio_probe_poll();
}
