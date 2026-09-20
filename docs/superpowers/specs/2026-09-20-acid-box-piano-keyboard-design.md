# acid_box note entry on a 13-key PianoKey keyboard — design

Date: 2026-09-20
Status: approved (brainstorm 2026-09-20, layout and octave-row mockups reviewed
option by option)
Tracking: no Linear issue yet.
Follows: `2026-09-15-acid-box-synthui-editor-design.md` (NEW-51, the step
editor this re-fits) and `2026-09-17-acid-box-synthui-top-bar-design.md`
(NEW-54, whose `mkpanelbtn`/`PANEL_MOMENTARY` and seven-segment sizing notes
this reuses).
Related: `2026-09-14-acid-box-landscape-design.md` (the present pipeline and the
touch-script geometry this keeps), `2026-09-11-acid-box-itcm-headroom-design.md`,
NEW-28 PianoKey, NEW-27 PanelButton, NEW-29 SevenSegment.

## 1. Goal & scope

Replace the editor's pitch detent knob with a playable one-octave keyboard:

| Control | Today | After |
|---|---|---|
| note entry | `synthui_rotary_knob`, 25 detents C1..C3 | 13 × `synthui_piano_key`, C[o]..C[o+1] |
| octave | implicit in the knob's sweep | view octave 1..3, − / + `synthui_panel_button`, one-digit `synthui_seven_segment` |
| note name | `lv_label` beside the knob | the same label, moved into the freed knob slot |
| sound on edit | none | the pressed key sounds while the transport is not playing |

The reachable range GROWS from C1..C3 (24..48) to C1..C4 (24..60).

**No SynthUI change.** Every widget is used through its existing API: no
SynthUI push, no `evkb.cmake` pin bump, no fresh-user cycle, no rebuild of the
other SynthUI-linking dirs.

**Out of scope, deliberately:** auto-advance after a key press; glissando; any
indicator for a note that is off the visible keyboard; greying the octave
buttons at their limits; a seven-segment note name (the widget has no `G` and
no sharp).

## 2. Widget facts that shaped the design

Read from the widget sources, not assumed from their names.

1. **PianoKey has `lit`, and it is an LED, not a key tint.**
   `synthui_piano_key_set_lit()` switches a small LED (`0x5C1C17` →
   `0xFF2A20` plus a bloom ring at 1.75 r) that sits above the ridged pad.
   There is no colour setter. `set_lit` damages ONLY the bloom box
   (`compute_led_dirty_area`), about 33×33 px on our white key.
2. **PianoKey never looks at `LV_STATE_PRESSED`.** Its class handles
   `LV_EVENT_DRAW_MAIN` and nothing else; the pressed look exists only through
   `set_pressed()`, which invalidates the WHOLE key. The consumer drives it —
   the same shape as `PANEL_MOMENTARY` driving a panel button's `on`.
3. **Its geometry is normalised to WIDTH and was drawn for a tall key**
   (class default 46×158, `vh` = 343). Ours are squat (white 105×130, `vh` =
   124), so two properties must be set, not defaulted:
   * `zone_top` — the white key's LED/pad zone must clear the black keys'
     78 px: 0.62 puts the LED centre 88 px down. Black keys keep 0.10.
   * `pad_height` — the constructor default is **48 px**, which on a 130 px
     key runs the pad to 99 + 48 = 147 px, 17 px past the key's bottom edge.
     `pad_height = 0` selects the widget's own auto-fit (`vh·0.94 − pad_y`,
     ≈ 23 px here).
4. **The seven-segment's width follows its text and it does not centre**
   (NEW-54 §2). A one-digit readout therefore gets its OWN derived box rather
   than the step readout's 84 px: one cell at h = 56 is
   (44 + 112·tan 6°) × 56/112 = 27.9 px → a 30 px box, centred between the two
   buttons. The widget CLIPS to its coords and a golden would pin a clipped
   digit, so the frame dump of §7 checks it.
5. **A panel button has one lightable state, `on`, already spent on the
   momentary press flash** — which is why the octave buttons carry no
   "note is that way" indicator (decision 3.4).

## 3. Decisions (each chosen in the brainstorm)

1. **Layout B.** The keyboard fills the band the geometry block marks
   "RESERVED: empty on purpose" (y 379..519); the octave row sits beside it in
   the editor column on the STEP row's exact x's. Nothing existing moves, so
   the gate's three pinned touches (PLAY 1088,43 / cell 2 281,143 / the CUTOFF
   drag x 89, y 583..647) stay valid. Rejected: keys in the old knob slot +
   editor column — 384 px gives 46 px white keys (4.4 mm on this ~10.5 px/mm
   panel) and 2.9 mm black keys.
2. **Octave buttons are VIEW ONLY.** They re-map the keyboard and never touch
   the pattern; the digit means "where the keys point".
3. **Audition only while the transport is not playing.** While it plays, keys
   only edit — the mono voice is never contended.
4. **A key press PRESERVES the gate**, as the knob did. A rest stays a rest
   until its lane cell is tapped.
5. **Octave buttons are panel buttons** (DOWN/UP glyphs, `PANEL_MOMENTARY`),
   consistent with tempo −/+ and step `<`/`>`.
6. **The readout is the bare octave number**, `1`..`3`.

## 4. Behaviour

**Mapping.** `base = 12·(viewOct + 1)`; key `k` (0..12) is note `base + k`.
`viewOct` ∈ 1..3. Boot: `viewOct = 1`.

**Key press** — on `LV_EVENT_PRESSED`, so edit and audition are one event:
* `commit_selected(base + k, st.gate, st.accent, st.slide)` — the existing
  single atomic `seq.step()` write, gate preserved.
* `set_pressed(key, true)`.
* if `!transport.playing()`: `acid.noteOn(note, st.accent ? 127 : 80, false)`
  (the sequencer's own default velocities; it has no getters) and
  `auditionNote = note`. `noteOn`/`noteOff` take their own `__disable_irq()`
  guards and keep an 8-deep held stack (`synth_acidbass.h`), so a user-context
  call is safe against the PIT note pump.

**Key release** — on `RELEASED`, `PRESS_LOST` and `INDEV_RESET`:
`set_pressed(key, false)`; if `auditionNote >= 0`, `acid.noteOff(auditionNote)`
and clear it. The REMEMBERED note is released, never a recomputed one, so no
path can orphan a `noteOff`. The poller's existing stopped→playing edge also
clears a live audition (one line; defensive — one pointer cannot hold a key and
press PLAY today).

`PRESS_LOCK` stays on: sliding across keys does nothing.

**Lit key.** At most one key is lit: `kb_key_for_note(display_note, viewOct)`
where `display_note = st.note ? st.note : 33` (today's "park a note-0 rest on
A1" rule). It shows the STORED note, rests included — the consequence of
decision 4; the label still reads `--` on a rest. `base + 12` lights the TOP
C; the same note is the BOTTOM C of the next view.

**Step select.** Keep `viewOct` if the display note is on the current 13 keys,
else snap to `clamp(note/12 − 1, 1, 3)`. Still a pure view change: no `STEP`
token.

**Octave − / +.** `viewOct ± 1`, clamped silently at 1 and 3 (as tempo clamps).
Updates the lit key and the digit. No pattern write.

## 5. Geometry (1280×720 logical; nothing existing moves)

| Element | Rect |
|---|---|
| white keys ×8 (C D E F G A B C) | 105×130 at `x = 16 + w·107`, y 384..513 — 16..870, under the lane's 8 columns |
| black keys ×5 (C# D# F# G# A#) | 64×78 at x = 90, 197, 411, 518, 625, y 384..461; CREATED AFTER the whites so they draw and hit-test on top |
| octave − | 1080..1123 × 420..475 (`PREV_X`/`PREV_W`) |
| octave digit | 1157..1186 × 420..475 (30 px, §2.4) |
| octave + | 1220..1263 × 420..475 (`NEXT_X`/`NEXT_W`) |
| `OCTAVE` caption | centred on x 1172, y 486 |
| note label | centred in the freed 912..1061 × 96..245 |

Touch sizes on glass: white 10 × 12.4 mm (its exposed lower zone 10 × 5 mm),
black 6.1 × 7.4 mm; the lane's cells are 9.5 mm.

The note label uses Montserrat 28 (enabled in `lv_conf.h`, not yet linked by
acid_box). ★ Its glyph data is NOT a flash cost here: `LV_ATTRIBUTE_LARGE_CONST`
is empty and `imxrt1176.ld` collects `.rodata*` into `.data > DTCM`, so the
font's **37,293 B** (measured from the built `lv_font_montserrat_28.c.obj`)
lands in DTCM. DTCM holds 51 KB (default build) to 104 KB (`build-bench`) of
256 KB today, so it fits in every configuration; §8 records the cost. If any
of the four builds fails to link, or ITCM moves beyond noise, the label stays
at the default 14.

## 6. Code shape (all in `examples/display/acid_box/`)

* **Removed:** `pitchKnob`, `angleToNote`, `noteToAngle`, `cbPitch`, the
  `PITCH_*` constants, the knob's creation block. The GC355 compositor
  discovers knobs itself (`synthui_rotary_gpu_begin_deferred`), so silicon
  composites 8 knobs instead of 9 with no wiring change.
* **New `keyboard_map.h`** — pure, LVGL-free, host-tested beside
  `tests/loopstat_pct_test.c`: `kb_base(view)`, `kb_key_for_note(note, view)`
  → −1..12, `kb_view_for_note(note, view)` (keep-or-snap), `kb_is_black(k)`,
  `kb_key_rect(k)`. The top-C boundary and the snap rule are off-by-one
  territory no pixel golden can see; the host test carries mutant arms
  (top C treated as the next view's 0; snap that ignores "already visible";
  clamp off by one at each end).
* **New state:** `pianoKey[13]`, `viewOct`, `litKey`, `auditionNote`.
* **`refresh_keyboard()`** moves the lit key, sets the digit, prints the
  witness. Called from `select_step`, `commit_selected`, `cbOctDn`/`cbOctUp`.
* **Callbacks** `cbKeyPress`, `cbKeyRelease`, `cbOctDn`, `cbOctUp` and
  `refresh_keyboard` live in FLASH (`.progmem`, the `UIBUILD_FN`/`ROTWIT_FN`
  pattern): tap-rate code, and the I-cache (NEW-36) covers it. Scene
  construction goes in the existing `UIBUILD_FN` build function.
  Every callback asks for `current_target`, per the file's standing rule.

**Witness tokens**
* `OCT=<n>` — printed by every `refresh_keyboard()` (so at boot, on every
  select, commit and page), immediately before `KEY_LIT`.
* `AUDITION=<note>` / `AUDITION=off`.
* `KEY_LIT=<k>` — READ BACK by scanning the 13 widgets with
  `synthui_piano_key_get_lit` (−1 = none, and a second lit key is reported as
  an error value), so it cannot agree with the model by construction — the
  `PLAY_LIT` lesson.
* `STEP[i]=…` is unchanged and already prints on every commit.

## 7. The gate (count unchanged at 141)

`touch_script.txt` is REGENERATED by its generator block (quoted in
`transcript_qemu.txt`), never hand-edited; the three pinned touches and the
pad lengths that give the RMS assertions their PRE/POST windows are preserved.
The boot golden is printed in `setup()` before any touch is processed, so it
stays a no-touch frame.

New gestures and what each asserts:

1. **Stopped, before PLAY — tap the already-lit A key** (603, 490):
   `AUDITION=33` then `AUDITION=off`, `STEP[0]=note33 gate1 acc1 sld0`.
   Pattern-neutral (it rewrites the note already there), so every RMS window
   downstream is unchanged.
2. **Playing, after the cell-2 tap — tap the top C** (817, 490):
   `STEP[2]=note36 gate1`, `KEY_LIT=12`, and NO `AUDITION=` line after the
   first `PLAYING=1` — asserted by LINE NUMBER, not presence.
3. **Tap octave +** (1242, 448): `OCT=2`, then `KEY_LIT=0` — note 36 is now
   the bottom C, the boundary rule seen from both sides. No `STEP` token
   between the two (a view change must not write the pattern).
4. **Tap the A# black key** (657, 420): `STEP[2]=note46`. At that x a white
   key's note (45 or 47) means the black keys are not on top.

Step 2's RMS assertion (`> 0.02` after the edit) holds at any of these
pitches; the gate asserts thresholds there, never magnitudes.

**Demonstrated RED, each by name:** the top-C rule broken; audition not gated
on `playing()`; `noteOff` dropped on release; black keys created before the
whites; `set_lit` never moved (the boot golden stays GREEN — only `KEY_LIT`
sees it). Two vacuity negatives join `gate-vacuity.test.sh`
(`AUDITION=` after `PLAYING=1`; `KEY_LIT` line missing), each matching
`^FAIL: <message>`.

**Goldens.** sw `0x18B7B637` moves: pinned only after two bit-identical gate
runs AND a no-touch frame dump whose FNV-1a equals the printed sum, with the
frame LOOKED AT — specifically the squat keys (LED clear of the black keys,
pad inside the key), the unclipped octave digit, the note label. gpu
`0xA67828E9` goes STALE; the four-boot re-bench is §9's. `transcript_qemu.txt`
is a hand-maintained DOCUMENT: splice and reconcile, never overwrite, then
replay it through the gate with the vacuity suite's fake QEMU.

## 8. ITCM and DTCM — recorded, not trusted

DTCM: `.data` + `.bss` before → after for all four configurations.
Pre-registered: `.data` grows by the Montserrat 28 glyph data (≈ 37.3 KB) plus
well under 1 KB of new state, and nothing else (today: `build` 51,451 B,
`build-loopstat` 53,167 B, `build-bt` 102,769 B, `build-bench` 104,477 B).

ITCM: before → after for all four configurations (`build`, `build-loopstat`,
`build-bt`, `build-bench`), read from `--print-memory-usage`. Pre-registered:
the default build's headroom (2804 B, no floor) does NOT shrink — the knob
maps and their `roundf` leave ITCM, the new callbacks are flash-resident.
The metric is quantised to 16 B (`imxrt1176.ld` `ALIGN(16)`): ±16 is noise,
a multiple of 32 is signal. `tools/build-bench-configs.sh` runs at close-out,
never concurrently with the sweep.

## 9. Silicon acceptance (by hand; no gate can see or hear these)

* audition audible while stopped AND paused, silent while playing;
* no stuck note: press, slide off the key, release;
* the 6 mm black keys hit reliably, and a tap low on a white key never lands
  on a black one;
* the pressed look shows on finger-down and clears on release;
* the lit LED reads clearly on glass at arm's length (it is ~1.6 mm);
* gpu golden re-recorded over four boots, bit-identical;
  `ACIDBOX_GPU_ERR=0`, `ROT_EQ mismatch=0 starved=0`, `full<=2`,
  `ACIDBOX_VSYNC timeouts=0` throughout;
* touch p95 not worse than the NEW-54 bench with the playhead stopped.

## 10. Delivery

Sweep 141/141/0 on a quiet host (rebuild acid_box's dirs first; no pin moved,
so the self-building BT gates need no rebuild), vacuity suite with the two new
negatives, `LICENSE-AUDIT: PASS` after the sweep, host tests
(`tests/run.sh`) green with the new `keyboard_map` suite, README rows and the
CLAUDE.md block, `bench_check` at close-out.

## 11. Risks

* **The squat-key look** (§2.3). If it does not survive the frame dump, the
  fallbacks in order: tune `zone_top`/`pad_height`; shorten the black keys;
  only then consider a SynthUI change (which would end "no pin bump").
* **The LED may be too faint a "selected note" marker** at 1.6 mm. It is the
  reference design and no colour or size knob exists; judged on glass (§9).
* **Whole-key press damage**: 13,650 px plus the overlapping black keys, twice
  per tap — tap-rate only. Worst tick ≈ 12 invalidated areas (playhead 8 +
  press 1 + lit 2 + label 1) against `LV_INV_BUF_SIZE` 32, so `full<=2` should
  hold; the gate asserts it on every run.
* **13 more objects on the screen.** PianoKey's draw is clip-aware (three
  `_lv_area_intersect` groups), so frames that do not touch the band should
  pay nothing; the bench's touch p95 is the check.
