# AudioSynthAcidBass — TB-303-style acid bass voice for the Audio library

Date: 2026-08-15
Status: approved (design), pending implementation plan

## What and why

A new source node for the Audio sibling repo (`~/Development/Audio`):
`AudioSynthAcidBass`, a monophonic Roland TB-303-style "acid bass" synth
voice. The behavior model is the mulch project's `AcidVoice`
(`~/Development/opengl-shader-streamer/src/audio/AcidVoice.{h,cpp}`), which is
the reference for how the voice must *sound and behave* but is not
API-compatible with the Teensy audio library — this design ports it into a
proper `AudioStream` object.

Decisions taken during brainstorming (all confirmed by the user):

- **Single voice object** — osc + sub-osc + ladder filter + envelopes +
  accent + slide all inside one `update()`. The 303 character depends on
  per-sample coupling (env→cutoff sweep, accent hitting filter and amp,
  slide), which block-rate graph connections cannot express.
- **Full mulch control set** — including sub-oscillator, filter-FM,
  key-tracking, and post-VCA tanh distortion.
- **Filter: port the mulch ladder** — the compact Stilson/Smith 4-pole with
  the tanh feedback tap, unchanged. It is the reference sound, ~10
  flops/sample, and stays private to the voice. Not the in-tree Huovilainen
  `filter_ladder` core (entangled with its oversampling wrapper, ~4× CPU) and
  not a new diode-ladder model (most authentic, least payoff at bass
  register).
- **Verification: full two-gate** — new evkb example with a QEMU token gate
  asserting self-measured DSP facts, then a hardware ear/bench session.
- **Board scope: rt1176 only** — no `boards` sidecar. The component itself is
  board-agnostic C++; an rt1062 example is a possible follow-up phase.

## 1. Component & API (Audio repo)

New files `synth_acidbass.h` / `synth_acidbass.cpp`, included from `Audio.h`,
MIT header matching the library's convention.

`class AudioSynthAcidBass : public AudioStream` with **0 inputs, 1 mono
output** — a pure source node.

```cpp
void noteOn(uint8_t note, uint8_t velocity, bool slide = false);
void noteOff(uint8_t note);
void waveform(short type);        // WAVEFORM_SAWTOOTH or WAVEFORM_SQUARE
void cutoff(float hz);            // clamped 20..12000
void resonance(float r);          // 0..1
void envMod(float amt);           // 0..1
void decay(float seconds);        // filter-env decay time constant
void accent(float amt);           // 0..1, scaled by note velocity
void subLevel(float amt);         // square sub-osc, -1 octave
void slideTime(float seconds);
void filterFM(float amt);         // bounded output->cutoff feedback
void keyTrack(float amt);         // cutoff follows note, 0..1
void distortion(float amt);       // post-VCA tanh drive
void level(float amt);            // output trim
float currentFreq() const;        // test hook (glide assertions)
float filtEnv() const;            // test hook (envelope assertions)
```

`waveform()` accepts the library's `WAVEFORM_SAWTOOTH` / `WAVEFORM_SQUARE`
constants; any other value is treated as sawtooth.

Behavior contract carried over verbatim from `AcidVoice`:

- Mono **last-note priority**; releasing the sounding note falls back to the
  most recent still-held note (pitch jumps, no retrigger).
- **Legato slide** (`slide=true` while a note is sounding) glides pitch with
  the `slideTime` one-pole and does **not** retrigger the filter envelope.
  A non-slide note-on jumps pitch and sets the filter env to 1.
- **Accent** (`accent_ * velocity/127`) adds to both the env-mod depth and
  the VCA target (`1 + 0.5*accentAmt`).
- Note-off with nothing held releases the amp envelope (~8 ms one-pole) to
  silence; the filter env free-decays.
- Note numbers clamped to 0..127 in both `noteOn` and `noteOff`.

## 2. DSP internals & concurrency

Per-sample float loop ported from `AcidVoice.cpp` + `LadderFilter` with three
changes:

1. **Sample rate** — `AUDIO_SAMPLE_RATE_EXACT` (44100.0f) replaces the
   hard-coded 48000. `updateCoefs()` reruns on any time-constant setter, and
   the constructor establishes valid coefficients before any setter call.
2. **Block I/O** — `update()`: `allocate()` → render `AUDIO_BLOCK_SAMPLES`
   (128) float samples → `saturate((int32_t)(s * 32767.0f))` into
   `block->data` → `transmit()` / `release()`. On allocation failure, return
   without advancing state (standard library pattern; silence under memory
   pressure, no state corruption).
3. **No heap, ISR-safe note state** — `std::vector<int> held_` becomes a
   fixed 8-deep array (overflow drops the oldest entry). `noteOn`/`noteOff`
   run in user context while `update()` runs at audio-ISR priority and they
   mutate multi-word state (`curFreq_`, `targetFreq_`, `gliding_`,
   `filtEnv_`, `gateOn_`, held list) together, so both wrap their bodies in
   `__disable_irq()` / `__enable_irq()` — the `AudioEffectEnvelope` pattern.
   Float parameter setters stay bare: single aligned 32-bit stores are atomic
   on Cortex-M7.

Unchanged from the reference (deliberately):

- Stilson/Smith ladder with `tanh` on the feedback tap — BIBO-stable at
  `res = 1`, gentle transistor character.
- `tanh`-bounded VCA output feeding the filter-FM path (stable feedback).
- Naive (non-band-limited) saw/square oscillators. At bass register through
  the ladder this is the reference sound; if hardware listening reveals
  objectionable aliasing on high notes, that becomes a *documented* follow-up,
  not silent scope growth.

CPU estimate: ~2 `tanhf` + ~2 `powf`-equivalents per sample at 44.1 kHz —
well under 1% of a 996 MHz M7. No lookup tables unless measurement says
otherwise.

## 3. Example, gates, and repo bookkeeping (evkb repo)

New example `examples/audio/acid_bass_test/` (rt1176 only): a self-playing
16-step acid pattern (note + accent/slide flags per step) through
`AudioOutputI2S` + WM8962, structured like `audiooutput_i2s_test`.

**QEMU gate** (`run_qemu.sh` via `tools/gate-lib.sh`): the sketch
self-measures DSP facts and prints tokens; the gate asserts them:

- `ACCENT PASS` — block RMS (via `AudioAnalyzeRMS`) of an accented note
  strictly greater than the same note unaccented.
- `SLIDE PASS` — `currentFreq()` sampled during a slide moves monotonically
  toward and lands within 1 Hz of the target note's frequency; a non-slide
  note-on jumps immediately.
- `DECAY PASS` — `filtEnv()` strictly decreases across a held note, and
  post-noteOff RMS falls below a silence threshold.
- Heartbeat/counter token per the vacuity-test discipline; commit
  `transcript_qemu.txt` so `gate-vacuity.test.sh` has its fixture.

**Hardware gate**: flash the same elf, ear-verify the acid line (accent pops,
slides bend, resonance squelches), capture `transcript_hw_evkb.txt` — the
Phase 5a evidence pattern.

**Bookkeeping** (order matters):

1. Audio-repo commit lands first (local-first resolution covers development).
2. After pushing the Audio repo, bump its pin in `evkb.cmake`.
3. Add the example's GATES entry to `tools/license-audit.sh` and run the
   audit (new files are MIT).
4. Re-measure the sweep line in `CLAUDE.md` (expected 89 → 90) by **running
   `tools/run-all-qemu-gates.sh`**, not by counting files. 0 SKIP required.

## Out of scope (YAGNI)

- No built-in sequencer object (the example's pattern player is example code).
- No polyphony.
- No rt1062 example in this phase.
- No band-limited oscillators (see above).
- No MIDI-input wiring (the API is callable from any MIDI handler; that is
  the caller's business).

## Error handling summary

- All float params clamped in setters (mulch clamps carried over).
- Note numbers clamped 0..127.
- `allocate()` failure → skip block, no state advance.
- Held-note overflow (>8) drops the oldest note.
- Filter numerically bounded by design (tanh feedback), including under
  filter-FM and full resonance.
