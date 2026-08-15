# AudioTransport — tempo, position and loop for the Audio library

Date: 2026-08-15
Status: approved (design), pending implementation plan

## What and why

A transport/clock component for the Audio sibling repo (`~/Development/Audio`):
`AudioTransport`, owning tempo, song position and a musical loop. It is the
timing source a **step sequencer** will be built on next; the sequencer is a
separate project with its own spec, plan and implementation cycle.

The behaviour reference is the mulch project's `Transport`
(`~/Development/opengl-shader-streamer/src/core/Transport.h`), which is not
API-compatible with the Teensy audio library. This design ports the *intent*
and diverges where an embedded audio graph demands it.

### The divergence that motivates the whole design

mulch advances the transport with `advance(dt)` once per GL frame — the **wall
clock**. On this board the audio graph runs on its own clock and drifts against
wall time: measured this same day at ~0.81x wall clock under QEMU and 1.0x on
silicon (`examples/audio/acid_bass_test/transcript_hw_evkb.txt`). A sequencer
clocked off `millis()` would slide steadily against the audio it is sequencing.

So the transport advances on the **audio block clock**, and every assertion
about it is referenced to that clock too.

Decisions taken during brainstorming (all confirmed by the user):

- **Time base: the audio block clock**, via an `AudioStream` whose `update()`
  runs once per 128-sample block. Not wall clock, not `IntervalTimer` — the
  latter is precise but sits in a different crystal domain from the SAI bit
  clock, so it drifts against the audio anyway.
- **Position unit: an exact sample counter plus PPQN ticks** at 96 PPQN.
- **Features carried over from mulch**: loop with bounds in bars, time
  signature with bar/beat query, the full play/pause/stop/rewind/forward
  control set, and the external-clock flag.
- **Sequencer interface: a pull-based per-block tick span**, not callbacks and
  not raw state.

## 1. Architecture and time base

New `transport.h` / `transport.cpp`, included from `Audio.h`, MIT header
matching the library's convention.

`class AudioTransport : public AudioStream` with **0 inputs, 0 outputs**. It
produces no audio; it exists so `update_all()` calls its `update()` once per
block. This is the GraphClock pattern already used by `audio_h_test`.

**Two counters, separate because they answer different questions:**

| field | type | job |
|---|---|---|
| `samplesElapsed_` | `uint64_t` | monotonic audio time; advanced by exactly `AUDIO_BLOCK_SAMPLES` per update. Never wraps, never loops, no float. |
| `tickPhase_` | 32.32 fixed point | musical position in PPQN ticks; this is what loops, and what the sequencer reads. |

**Ticks are the master for musical position**, rather than a value derived from
the sample counter. The reason is concrete: loop bounds are musical.
`ticksPerBar = PPQN * beatsPerBar` is tempo-independent, so a 4-bar loop stays
exactly 4 bars across any tempo change and the wrap is exact integer
arithmetic with no seam error. Deriving ticks from an absolute sample count
would make a mid-song tempo change retroactively move the playhead.

Fixed-point accumulation costs ~2.3e-10 tick per block of rounding — about
2e-4 tick over a million blocks. Inaudible over any session.

PPQN is **96**: the sequencer's natural resolution and an exact multiple of
MIDI clock's 24, so `externalPulse()` is a whole number of ticks.

## 2. API

```cpp
// tempo & state
void tempo(float bpm);            // clamped 20..999
void beatsPerBar(uint8_t n);      // 4/4 default
void play(); void pause(); void stop();   // stop() rewinds to loop start (or 0)
void rewindBar(); void forwardBar();
bool playing() const;

// loop -- bounds in BARS, so they survive tempo changes
void loop(float startBar, float endBar);
void looping(bool on);

// query
uint64_t samples() const;         // exact, monotonic
float seconds() const;            // derived from samples
float beats() const; float bars() const;
int barNumber() const;            // 1-based, for display
int beatInBar() const;            // 1-based

// THE SEQUENCER INTERFACE -- ticks that began during the block just processed
int      tickCount() const;
uint32_t tickAt(int i) const;         // musical tick index
uint16_t tickOffsetAt(int i) const;   // 0..127, sample offset within the block

// external clock (MIDI sync scaffolding)
void externalClock(bool on);      // update() stops advancing
void externalPulse();             // one MIDI clock pulse = PPQN/24 = 4 ticks
```

### Behaviour contract

- `tickCount()` is normally 0, 1 or 2 — at 96 PPQN and 300 BPM a block spans
  1.39 ticks. The internal array is sized **8** and the count **saturates**
  there, which only matters above ~1700 BPM. Saturation is documented and
  reported, never a silent truncation.
- A **loop wrap inside a block** makes the tick indices jump backward at the
  seam. The pull-based API represents that naturally; consumers need no
  special case.
- The seam **carries the overshoot** (mulch's behaviour), so no tick is lost
  or duplicated across it.
- `stop()` rewinds to the loop start when looping, else to zero.
- Loop bounds are validated: `endBar > startBar`, else the call is ignored.
- `externalClock(true)` makes `update()` stop advancing `tickPhase_`;
  `samplesElapsed_` keeps counting, because audio is still being produced.

### Concurrency

`play`/`pause`/`stop`/`tempo`/`loop`/`beatsPerBar` run in user context while
`update()` runs at audio-ISR priority, and they mutate multi-word state
together, so each wraps its body in `__disable_irq()` / `__enable_irq()` — the
in-tree pattern (`AudioSynthAcidBass`, `AudioEffectEnvelope`,
`AudioSynthSimpleDrum`). They are **user-context only**, documented as such.

### ★ Update ordering — to be measured, not assumed

The sequencer reads a span the transport cached during its own `update()`. If
`update_all()` walks the sequencer first, the sequencer sees the previous
block's span: a constant 2.9 ms latency, **not** drift. Benign either way, but
the implementation must **measure** which order the update list actually runs
in and document the finding. Do not assume construction order implies update
order.

## 3. Verification — the two-gate rule

New example `examples/audio/transport_test/` (rt1176 only, no `boards`
sidecar).

### QEMU gate

Every assertion is referenced to the **audio clock**, per the lesson the
acid-bass gates cost a full round of review: a check that compares an
audio-clock quantity against a wall-clock delay flakes under load. Both sides
of each comparison here are audio-clock quantities, so the assertions hold at
any host speed.

- `TEMPO PASS` — over an exact, counted number of blocks, accumulated ticks
  match the analytic expectation within a tight band.
- `LOOP PASS` — position wraps at the correct tick, and the seam neither loses
  nor duplicates a tick (ticks across the wrap equal the sum of the parts).
  **This is the un-fakeable one**: an off-by-one at the seam is precisely the
  defect this component would otherwise ship with.
- `TEMPOCHANGE PASS` — changing tempo mid-run leaves the loop length in bars
  unchanged. This is the property that motivated ticks-as-master, so it is
  asserted rather than assumed.
- `TRANSPORT PASS` — paused advances nothing; `stop()` rewinds.

Committed `transcript_qemu.txt` so `gate-vacuity.test.sh` has a fixture.

### Hardware

The example wires the transport to `AudioSynthAcidBass` with a minimal
four-on-the-floor, so **tempo and the loop seam are audible**. That gives real
silicon evidence without waiting for the sequencer, and is the natural
precursor to it. Evidence: `transcript_hw_evkb.txt` plus the user's listening
verdict.

### Bookkeeping

1. Audio-repo commit first (local-first resolution covers development).
2. Push Audio, then bump its pin in `evkb.cmake`.
3. Add the example's `GATES` entry to `tools/license-audit.sh`; run the audit.
4. Re-measure the sweep line in `CLAUDE.md` by **running** the sweep. 0 SKIP
   required. Note that as of 2026-08-15 that line is explicitly marked
   not-re-measured, because a concurrent qemu2 ASan `configure` removed the
   QEMU binary; the sweep must be re-run once that build is restored.

## Out of scope (YAGNI)

- No step sequencer — that is the next project, with its own spec.
- No swing/shuffle.
- No time-signature changes mid-song.
- No tempo ramps or automation.
- No MIDI clock *transmission* — only the `externalPulse()` input hook.
- No song/arrangement structure beyond the single loop.

## Error handling summary

- `tempo()` clamped to 20..999 BPM; a zero or negative tempo can never reach
  the tick-increment arithmetic.
- `loop()` ignores an invalid range (`endBar <= startBar`).
- `beatsPerBar()` clamped to 1..32.
- `tickAt()` / `tickOffsetAt()` return 0 for an out-of-range index rather than
  reading past the array.
- Tick array saturation at 8 per block is reported, not silent.
