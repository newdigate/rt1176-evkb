# AudioStepSequencer — a 303-style step sequencer for the Audio library

Date: 2026-08-15
Status: approved (design), pending implementation plan

## What and why

A 16-step sequencer for the Audio sibling repo (`~/Development/Audio`):
`AudioStepSequencer`, driven by `AudioTransport`'s per-block tick span and
emitting note events for `AudioSynthAcidBass`.

This is the third component in the series. `AudioSynthAcidBass` (the voice) and
`AudioTransport` (the clock) are both complete, hardware-verified and pushed;
this consumes the clock to play the voice.

The behaviour reference is the mulch project's `StepSequencerNode`
(`~/Development/opengl-shader-streamer/src/modules/StepSequencerNode.h`), with
one substantial divergence: mulch's is a **drum** sequencer — sixteen boolean
toggles firing one fixed note — while the voice here is a monophonic TB-303
whose musical interest is per-step pitch, accent and slide. The step model
follows the 303, not the drum machine.

Decisions taken during brainstorming (all confirmed by the user):

- **Full 303 step**: note, gate, accent, slide — exactly the four things a
  TB-303 stores, and exactly what `AudioSynthAcidBass::noteOn(note, velocity,
  slide)` can express.
- **Pull-based event queue**, mirroring the transport's tick span, rather than
  binding directly to the voice or invoking callbacks.
- **Carry the sample offset, apply at block rate.** Events report where in the
  block each step falls; the drainer applies them at block granularity.
- **Fixed 16th notes, 16 steps.**

## 1. Architecture and binding

New `seq_step.h` / `seq_step.cpp`, included from `Audio.h`, MIT header matching
the library's convention.

`class AudioStepSequencer : public AudioStream` with **0 inputs, 0 outputs** —
the same shape as `AudioTransport`, so `update_all()` runs its `update()` once
per 128-sample block.

It takes the transport **by reference in its constructor**:

```cpp
AudioTransport     transport;
AudioStepSequencer seq(transport);   // cannot be declared before the transport
```

★ **This is the point of the design, not an incidental choice.** The transport
requires consumers to be declared *after* it — update order is construction
order, so a node declared earlier sees the previous block's tick span, a silent
2.9 ms staleness. `transport.h` can only warn about that in prose. Binding by
constructor reference makes it **structurally unrepresentable**: you cannot pass
a reference to an object that has not been constructed.

★ **`active = true` in the constructor is required**, for the same reason it is
in `AudioTransport`: `AudioStream`'s constructor sets `active = false`
(`AudioStream.h:140`), only `AudioConnection::connect()` sets it true
(`AudioStream.cpp:222,225`), and `software_isr` skips inactive nodes (`:323`).
This node has no audio connections, so without that line its `update()` never
runs and every assertion fails at once.

## 2. Pattern and event model

```cpp
struct AcidStep {          // the four things a TB-303 stores
    uint8_t note;          // 0..127
    bool    gate;          // false = rest
    bool    accent;
    bool    slide;         // tie into the next step
};

struct SeqEvent {
    uint8_t  type;         // SEQ_NOTE_ON | SEQ_NOTE_OFF
    uint8_t  note;
    uint8_t  velocity;
    bool     slide;        // pass through to noteOn's third argument
    uint16_t sampleOffset; // 0..127, from the transport's tick offset
};
```

- **16 steps**, one step = `AudioTransport::PPQN / 4` = **24 ticks**, so a
  pattern is **384 ticks — exactly one bar at 4/4**, which is the transport's
  default and the configuration the gate and the hardware run both use.

★ **The pattern is a fixed 384 ticks; the loop is whatever `loopTicks()` says.
They are not the same quantity, and the spec must not pretend they are.** Three
cases follow, all defined by the fold rather than special-cased:

- **Loop = 384 ticks (1 bar at 4/4).** The common case: pattern and loop
  coincide, all 16 steps play, and the pattern restarts exactly at the seam.
- **Loop shorter than 384, or not a multiple of 24.** The fold restarts the
  pattern at the loop start, so only the first `loopTicks()/24` steps are ever
  reached and a final partial step is cut off mid-gate. Defined and harmless,
  but it is *not* "the pattern playing faster" — it is the tail being unused.
  At 3/4 a one-bar loop is 288 ticks, so steps 12–15 never fire.
- **Loop longer than 384.** `% 16` wraps the step index, so the pattern simply
  repeats within the loop.

A `beatsPerBar()` other than 4 therefore changes which steps are reachable but
never desynchronises them, because the fold is in ticks and never consults the
time signature. This is a consequence worth stating rather than a limitation
worth fixing: making the step count follow the signature would be a different
component.

- **Not looping.** `loopTicks()` still reports the configured length even when
  `looping(false)`, so the fold keeps the pattern repeating against a
  free-running playhead. That is the wanted behaviour, and it means the
  sequencer needs no separate free-running mode — unlike mulch's, which carries
  two independent timing paths.
- The step index comes from the **folded** tick:
  `(tickAt(i) - loopStartTick()) % loopTicks()`, then `/ 24 % 16`. This is the
  idiom `transport.h` documents, and it means the pattern restarts at the loop
  start rather than drifting against it. An unfolded index would yield five
  quarters per bar across the seam — the audible flam already found and fixed
  in `acid_bass_test`.

### The event queue

Mirrors the transport's tick span exactly, deliberately:

```cpp
int             eventCount() const;
const SeqEvent& eventAt(int i) const;
bool            eventOverflow() const;
```

Valid only for the block just processed; stable inside `update_all()`'s walk; a
user-context reader must snapshot under `__disable_irq()`. Same contract and
the same wording as the transport, so a reader learns it once.

Capacity **8 events**, saturating with `eventOverflow()` set rather than
truncating silently. Sizing: at 999 BPM a block spans 4.64 ticks against a
24-tick step, so one step boundary per block is the norm and each step emits at
most two events. Short loops (the transport's `MIN_LOOP_TICKS` is 8) can put
several step boundaries in one block, which is what the headroom is for.

### Event ordering is the sequencer's job

For a **slide** step, the next step's note-on is emitted **before** the
previous note's note-off, so a drainer that applies events blindly in order
produces correct legato. That ordering is what makes `AudioSynthAcidBass` glide
rather than retrigger, and putting it here means no consumer has to know it —
the `acid_bass_test` example originally got exactly this wrong.

## 3. Gate length and emission

- Non-slide steps release partway through the step. `gateLength()` is a
  fraction, default **0.5**, so a note-off is emitted at step start + 12 ticks.
- **Slide steps emit no note-off.** The tie is what makes the pitch glide.
- Velocities: `accentVelocity()` default **127**, `normalVelocity()` default
  **80**. Accent is expressed through velocity because that is how the voice
  consumes it (`accentAmt = accent_ × velocity/127`).

The sequencer therefore inspects **every** tick the transport reports, not only
step boundaries, and emits on two kinds: step boundaries (multiples of 24) and
gate-off ticks. Both are exact, because the transport reports every tick with
its sample offset.

`transport.wrapped()` resets the held-note state, so a loop restart cannot
leave a note hanging.

### API

```cpp
void step(int i, uint8_t note, bool gate, bool accent, bool slide);
AcidStep step(int i) const;
void clear();                      // all steps rest
void gateLength(float fraction);   // clamped 0.1..0.9
void accentVelocity(uint8_t v);
void normalVelocity(uint8_t v);
int  currentStep() const;          // last step index triggered; -1 before the
                                   // first one, so "nothing has played yet" is
                                   // distinguishable from "step 0 played"
```

Pattern mutators run in user context and take `__disable_irq` guards, matching
`AudioTransport` and `AudioSynthAcidBass`.

## 4. Verification — the two-gate rule

New example `examples/audio/step_seq_test/` (rt1176 only, no `boards` sidecar).

### QEMU gate

Every assertion referenced to the **audio clock** — both sides of every
comparison are audio-clock quantities, so none of them moves with host speed.
This is the discipline that made `transport_test` produce identical numbers on
QEMU and silicon, and whose absence flaked four `acid_bass_test` assertions.

- `STEPS PASS` — over a counted number of blocks with a known pattern, the
  note-on count equals the analytic expectation.
- `ORDER PASS` — for a slide step, the note-on precedes the note-off in the
  event stream. **Un-fakeable and load-bearing**: it inspects emission order
  directly, and it is the property the entire slide feature rests on.
- `ACCENT PASS` — accented steps emit velocity 127, unaccented 80.
- `REST PASS` — a `gate=false` step emits no note-on, **and the step after a
  rest still fires**. An off-by-one in step indexing breaks exactly this and
  nothing else.
- `WRAP PASS` — the pattern restarts at step 0 on a loop wrap, with no hung
  note.
- `SHORTLOOP PASS` — with a loop shorter than the pattern, the steps beyond the
  loop never fire and the reachable ones still land on their correct tick.
  This pins the fold's defined behaviour from §2 so a future change to step
  derivation cannot quietly turn "unused tail" into "compressed pattern".

Committed `transcript_qemu.txt` as a `gate-vacuity.test.sh` fixture. Do not pin
free-running absolute counters in it — assert differences, per the lesson from
`transport_test`.

### Hardware

A real 16-step acid pattern driving `AudioSynthAcidBass` through the queue, so
accents, slides and rests are all audible. Evidence: `transcript_hw_evkb.txt`
plus the user's listening verdict, confirming in particular that **slides bend
and rests are silent** — the two things the event stream can only assert
structurally.

### Bookkeeping

1. Audio-repo commit first (local-first resolution covers development).
2. Push Audio, then bump its pin in `evkb.cmake`.
3. Add the example's `GATES` entry to `tools/license-audit.sh`; run the audit.
   The drift check will catch a missing entry on its own.
4. Re-measure the sweep line in `CLAUDE.md` by **running** the sweep (91 → 92).
   0 SKIP required.

## Out of scope (YAGNI)

- No swing or shuffle.
- No configurable step division or pattern length.
- No pattern chaining, bank switching or song mode.
- No per-step velocity — accent already scales it, and two controls for one
  audible effect is a confusion, not a feature.
- No MIDI input or output.
- No sample-accurate note application — events carry `sampleOffset`, but
  applying it would require reopening the verified, pushed and pinned voice.

## Error handling summary

- `step(i, …)` ignores an out-of-range index; `step(i) const` returns a rest.
- `gateLength()` clamped 0.1..0.9, so a gate can never be zero-length or
  swallow the following step.
- `eventAt()` returns a zeroed event for an out-of-range index.
- Queue saturation at 8 sets `eventOverflow()`; never silent.
- A transport with `loopTicks() == 0` (not reachable through the public API,
  which enforces `MIN_LOOP_TICKS`) must not divide by zero — guard anyway.
- `externalClock` on the transport makes its tick interface inert, so the
  sequencer emits nothing; documented rather than worked around.
