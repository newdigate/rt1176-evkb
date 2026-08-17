# Acid Box — the audio+display integration capstone

Date: 2026-08-17. Status: approved in session, pending implementation.

## 1. Goal

A playable acid groovebox on the EVKB: the 303 voice, `AudioTransport` and
`AudioStepSequencer` (the trio finished 2026-08-15) wired to a touch UI of
SynthUI knobs and a new step-cell widget on the RK055, audio out the WM8962
headphone jack. This is the first example where the audio and display halves
of this tree meet under real conditions, and its point is the integration:
**touch → pattern → sound, verified end to end**.

It deliberately does NOT depend on the failed VGLite fps criterion: the
software renderer repaints one knob in ~22 ms, and a real instrument animates
one knob (the touched one) plus a two-cell cursor move — inside the 33 ms
budget. The GPU path stays unused.

## 2. Decisions (settled in brainstorm, 2026-08-17)

| axis | decision |
|---|---|
| shape | editable groovebox: touch-edited 16-step pattern while it plays |
| audio out | I2S / WM8962 headphone jack (self-contained instrument) |
| knobs | eight, 2×4: cutoff, resonance, envMod, decay, accent, distortion, subLevel, slideTime |
| layout | "Face": transport bar top, knobs, selected-step editor strip, 2×8 step lane at the bottom (thumb zone) |
| knob input | vertical drag, ~200 px = full sweep |
| widget home | knob input layer + `synthui_step` in SynthUI; buttons/readouts are stock LVGL styled in the app |
| boot state | preset acid line loaded, transport STOPPED (deterministic first frame + silence) |
| glue | thin: direct calls + polled cursor (approach 1) |

The 2×8 step lane (not 1×16) is a touch-target decision: 90 px cells are
~7.8 mm at this panel's ~295 DPI; a 1×16 row's 45 px cells (~3.9 mm) are
below reliable finger size. Tempo is a −/+ readout, not a ninth knob.

## 3. Architecture

`examples/display/acid_box` (the display category hosts cross-cutting demos —
`camera_preview_synth` precedent). RT1176 only, software LVGL renderer,
single core; the CM4 stays out (one voice does not need it).

### 3.1 SynthUI additions (sibling repo, pilot discipline)

- **Knob input layer** on `synthui_knob`: `LV_EVENT_PRESSING` accumulates
  vertical drag; 200 px maps to the full sweep; clamped at the range stops;
  detent mode snaps per `detent_step`; emits `LV_EVENT_VALUE_CHANGED`.
  ★ It adds NO drawing. The pilot's five goldens must stay bit-identical,
  asserted as a regression check, not assumed.
- **`synthui_step`**, the library's second widget: square cell drawing gate
  fill, accent dot, slide bar, cursor ring, selected outline (per the DC
  reference art); emits clicked. Verified by its own example
  (`examples/display/synthui_step_test`) with per-state goldens and the knob
  pilot's triple agreement: QEMU, host-clang, silicon.

### 3.2 Audio graph (existing library code, `step_seq_test` wiring)

`AudioTransport` (PPQN 96) → `AudioStepSequencer(transport)`;
`AudioSynthAcidBass` → `AudioOutputI2S` (+ WM8962 codec bring-up as in
`audiooutput_i2s_test`). `level()` fixed at a safe headphone value; no knob.

### 3.3 Glue

- **Note-event pump: `IntervalTimer` at 1 kHz** drains
  `seq.eventCount()/eventAt()` → `acid.noteOn/noteOff`. This is the one
  timing decision that matters: draining in `loop()` would let a ~22 ms
  knob repaint delay a noteOn audibly. Both ends take their own
  `__disable_irq` guards (verified in `seq_step.cpp` and
  `synth_acidbass.cpp`), so PIT-context dispatch is inside their contract.
- **UI → engine, direct:** knob `VALUE_CHANGED` → mapping (§4) → voice
  setter (single-word float writes, atomic on CM7). Step tap and every
  editor-strip edit write back as ONE
  `seq.step(i, note, gate, accent, slide)` call — the header marks these
  USER CONTEXT ONLY and internally guarded, and the LVGL loop is user
  context. Transport buttons → `play()/pause()/stop()`; tempo −/+ →
  `transport.tempo()`.
- **Engine → UI, polled:** an `lv_timer` at 33 ms reads `currentStep()`
  (cursor ring: exactly two cell invalidations per move; at 128 BPM
  sixteenths are ~117 ms, so a 33 ms poll never skips), `playing()` (▶/⏸
  label), and tempo (readout).

### 3.4 Boot sequence

Panel + LVGL up → build layout → program the preset pattern (16 steps of
data in the sketch — a classic acid line with accents and slides) → set the
default patch (documented angles, §4) → print the token banner → render one
frame → checksum. Transport stopped; silence until ▶.

## 4. Interaction and parameter mapping

Eight sound knobs are `MODE_BOUNDED`, −140°..+140°, stop rays visible.
Mapping is perceptual where it matters:

| knob | map | range |
|---|---|---|
| cutoff | exponential | 20 Hz – 12 kHz |
| resonance | linear | 0 – 1 |
| envMod | linear | 0 – 1 |
| decay | exponential | 30 ms – 2 s |
| accent | linear | 0 – 1 |
| distortion | linear | 0 – 1 |
| subLevel | linear | 0 – 1 |
| slideTime | exponential | 10 – 300 ms |

Boot angles come from a documented default patch so the first frame is
deterministic.

**Step lane:** tap toggles the cell's gate AND selects it (outline follows
the last touch). The editor strip operates on the selected step: pitch knob
in `MODE_DETENTS`, one detent per semitone, C1–C3 (25 detents), note-name
readout beside it; ACC and SLD toggles. The strip deliberately carries NO
gate button — gate toggling lives on the cell itself (the layout mockup's
draft strip drew one; the approved interaction design dropped it as
redundant). Every edit is one atomic `seq.step()` transaction.

**Transport:** ▶ toggles play/pause (label flips), ■ stops (the transport's
`stop()` already rewinds). Tempo −/+ steps ±1 BPM with stock LVGL
long-press auto-repeat. SAW/SQR toggles `waveform()` and its label.

## 5. Verification — the two-gate rule

### 5.1 QEMU gate (`rt1176:display/acid_box`), four assertions in one run

1. **Boot frame golden** — the stopped-state frame, FNV-checksummed;
   recorded only after eyes on the pixels.
2. **Touch-injected control** — inject a tap on ▶, assert a `PLAYING=1`
   token; tap a step cell, assert the pattern-data token flips
   (`lvgl_rk055_touch_test` precedent).
3. **Audio, audio-clock referenced** — run the preset pattern for 2 bars
   against the audio clock and checksum the rendered audio (the
   `acid_bass_test` tap pattern), so QEMU and silicon agree bit-for-bit.
4. **The integration assertion:** after the injected step edit, render
   another bar and assert the audio checksum CHANGES to a second recorded
   golden — touch → pattern → sound with both expected values known. This
   is the capstone's reason to exist; a gate without it is decoration.

★ Plan-time check, stated rather than fudged: knob DRAG injection needs
press-move-release sequences from the qemu2 GT911 model. If the model only
injects taps today, the QEMU gate covers taps (steps + transport) and the
drag interaction is verified on silicon only — documented in the gate
header either way.

Vacuity discipline as always: sentinel + one-nibble mutation proofs for
every golden, and the missing-capture path must fail by name.

### 5.2 Hardware

The by-ear ritual: preset line through headphones; cutoff swept under a
finger while it plays; a step toggled and heard entering/leaving the loop;
accent/slide audible. UI verified by SWD framebuffer dumps
(`tools/rt1170-screenshot.py`). Transcript records all of it.
**Bench precondition: the MCU-Link VCOM needs its physical replug first**
(hardware tokens read over serial; it has been dead since 2026-08-17).

### 5.3 Bookkeeping

Sweep 95 → **97** (`display/acid_box`, `display/synthui_step_test`); two
GATES entries in the licence audit, each mutation-tested; KNOWN-BROKEN-GATES
notes for what each QEMU gate does and does not prove; `examples/README.md`
rows; CLAUDE.md baseline re-measured on merge; SynthUI pin bumped after its
additions are pushed.

## 6. Risks

- **Knob input regressing the pilot goldens** — it adds no drawing;
  asserted by re-running `synthui_knob_test` goldens, not assumed.
- **UI long-frames** can only delay the cosmetic cursor; audio renders at
  ISR priority and note dispatch at PIT priority.
- **SEMC sustained-refresh wedge** (unresolved, documented in
  `vglite_lvgl_test`'s transcript): this app's animation is small-damage
  only (two cells + one knob), outside the wedge's observed envelope —
  but eyes stay open on silicon.
- **Headphone level**: fixed conservative `level()`; no knob can turn it
  harmful.

## 7. Non-goals

- VGLite/GPU rendering (measured slower for this scene class; Phase 2's
  verdict stands).
- Multiple patterns, pattern save/load, SD — the "full patterner" shape,
  deliberately deferred.
- USB audio out (I2S chosen; the UAC series already proved USB elsewhere).
- MIDI in/out, CM4 offload, more SynthUI widgets than the step cell.
- Tempo as a knob; per-step velocity editing beyond accent.
