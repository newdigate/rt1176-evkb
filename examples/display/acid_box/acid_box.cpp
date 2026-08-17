/* acid_box - the audio+display integration capstone.
 * Spec: docs/superpowers/specs/2026-08-17-acid-box-capstone-design.md
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * THIS COMMIT IS THE AUDIO HALF ONLY.  `build_ui()` is a dark-ground stub so
 * the file compiles, the first frame is deterministic and the panel chain is
 * exercised; the eight knobs, the step grid and the touch bindings land next.
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
#include <Audio.h>
// Audio.h pulls in every codec driver EXCEPT this one -- control_wm8960.h is
// in its include list and control_wm8962.h is not, so the WM8962 header must be
// named explicitly, exactly as acid_bass_test and audiooutput_i2s_test do.
#include "control_wm8962.h"
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
// Not used by the stub below, but compiled here deliberately: SynthUI's headers
// have never before been preprocessed in the same translation unit as Audio.h,
// and a collision between the two would be a Task-5 surprise discovered after
// the audio work was already declared done.
#include "synthui_knob.h"
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
    }
    if (s != lastSeenStep) {
        /* 15 -> 0 is the loop seam.  Anchoring on the seam rather than on
         * "s == 0" means a bar is only reported once the whole 16-step table
         * has been filled, so no line can carry a half-measured window. */
        if (s == 0 && lastSeenStep == 15) {
            barsDone++;
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
 * STUB FOR THIS COMMIT ONLY -- replaced wholesale by the knob/grid scene.
 * It still paints an OPAQUE ground over the whole screen, because that is what
 * makes every pixel of the frame defined and therefore makes ACIDBOX_UI_SUM a
 * checksum of something rather than of whatever the allocator left behind. */
static lv_obj_t *build_ui(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    return scr;
}

void setup()
{
    CONSOLE.begin(115200);
    delay(200);
    CONSOLE.println("ACIDBOX_BEGIN");

    AudioMemory(24);
    const bool codec = wm.enable();
    wm.volume(0.6f);
    CONSOLE.println(codec ? "CODEC_OK" : "CODEC_FAIL");

    const bool panel = Display.begin();
    CONSOLE.println(panel ? "PANEL_OK" : "PANEL_FAIL");
    if (!panel) {
        /* No lv_init() happened, so loop()'s lv_timer_handler() returns
         * immediately -- the same contract the sibling display examples use.
         * The transport is also never configured, so audio_probe_poll() stays
         * on its -1 early return and the run is silent in both senses. */
        CONSOLE.println("ACIDBOX_DONE");
        return;
    }

    lvgl_rt1176_begin();
    lvgl_mipi_panel_create(Display);

    load_preset();
    default_patch();
    transport.tempo(128.0f);
    transport.loop(0.0f, 1.0f);        /* one bar == the 384-tick pattern */
    transport.looping(true);
    pump.priority(224);                /* BEFORE begin(): see pump_isr's note.
                                        * priority() applied afterwards would
                                        * leave a window running at 128. */
    pump.begin(pump_isr, 1000);        /* MICROseconds, Teensy convention: 1 kHz */

    /* The stub scene is the FIRST refresh, so the sum below covers a
     * whole-screen paint rather than whatever a partial repaint touched. */
    lv_screen_load(build_ui());
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000)
        lvgl_rt1176_loop();
    lvgl_sum_reset();
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);
    CONSOLE.printf("ACIDBOX_UI_SUM=0x%08lX\n", (unsigned long)lvgl_sum_value());
    CONSOLE.printf("PLAYING=%d\n", transport.playing() ? 1 : 0);
    CONSOLE.println("ACIDBOX_DONE");
}

void loop()
{
    lvgl_rt1176_loop();
    audio_probe_poll();
}
