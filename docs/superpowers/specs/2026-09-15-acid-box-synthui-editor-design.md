# acid_box editor on SynthUI — LedButton lane, lamps, step readout — design

Date: 2026-09-15
Status: approved (brainstorm 2026-09-15)
Tracking: Linear **NEW-25** (the widget) plus a new acid_box issue for this
rework (to be filed at plan time; "acid_box editor on SynthUI widgets").
Depends on: `2026-09-15-synthui-led-button-design.md` (the widget must be
merged, pinned and gated first).
Related: `2026-09-14-acid-box-landscape-design.md` (the present pipeline and
touch-script geometry this rework keeps), `2026-09-11-acid-box-itcm-headroom-design.md`
(the routing rules §6 extends), NEW-24 Lamp, NEW-27 PanelButton, NEW-29 SevenSegment.

Mockup (approved): https://claude.ai/artifact/JjHojwmcoZ7U62ekzVkLin

## 1. Goal & Scope

Replace the hand-drawn and stock-LVGL controls in acid_box's **step lane and
editor column** with SynthUI widgets, so the panel reads as the DC reference
hardware rather than as LVGL defaults:

| Control | Today | After |
|---|---|---|
| 16 step cells | `synthui_step` (gate fill, accent dot, slide bar, cursor ring, selected outline) | `synthui_led_button`, red LED: `lit` = gate, `cue` = playhead, latched `pressed` = selected |
| accent / slide per step | drawn on the cell | two `synthui_lamp` pills under each key: amber = accent, blue = slide |
| step number | none | `lv_label` under each key's lamps |
| ACC / SLD toggles | `lv_button` with a bg-colour swap | 56 px `synthui_led_button`, amber and blue LEDs, `lv_label` beside each |
| selected step readout | none (only the note name) | `synthui_seven_segment`, two characters `"01"`..`"16"` |
| previous / next step | none | `synthui_panel_button` REWIND / FORWARD, pale accent |

**Out of scope (follow-up spec)**: PLAY/STOP, tempo −/+, the BPM label and
SAW/SQR stay as they are. The title, note name, pitch knob and the eight
sound knobs are untouched.

## 2. Behaviour

- **Tap on a key** (unchanged): `select_step(i)` then toggle the gate via
  `commit_selected`. So a tap both selects and edits, as today.
- **Prev / next** (new): `select_step((selectedStep + 15) % 16)` /
  `select_step((selectedStep + 1) % 16)`. Selection only; no pattern change.
  Wraps 16 → 1 and 1 → 16.
- **`select_step(i)`**: clear the old key's latch
  (`synthui_led_button_set_pressed(old, false)`), set the new one, write the
  seven-segment (`"%02d"`, 1-based), and as today set the pitch knob angle,
  the note label, and the ACC/SLD LEDs from the step's bits. Prints
  `SELECT=%d` (new, 0-based, for the bench transcript; no gate reads it).
- **`commit_selected(note, gate, accent, slide)`**: writes the sequencer step
  as today, then `set_lit(key, gate)`, the two lamps under that key, the note
  label, and the ACC/SLD LEDs. The `STEP[%d]=note%u gate%d acc%d sld%d`
  console line is unchanged (the gate's audio assertions pivot on the
  engine, not on this line, but the line is bench evidence).
- **Playhead**: `ui_poll` swaps `synthui_led_button_set_cue` on the cell the
  transport reports, exactly where it swaps `synthui_step_set_cursor` today.
- **ACC / SLD keys**: `LV_EVENT_CLICKED` → the existing `cbAccBtn`/`cbSldBtn`
  (which flip the selected step's bit through `commit_selected`); the keys'
  `lit` mirrors the bit, so pressing ACC lights the amber LED on the key AND
  the amber lamp under the selected step, the same hue in both places.
- **Boot**: `select_step(0)` sinks key 1 and shows `"01"`, before `ui_poll`
  is primed and the timer armed (the existing golden-race ordering holds).

## 3. Layout (logical 1280×720 landscape, CW90-presented)

Row 0 and every existing hit target keep their coordinates; see §5.

```
/* lane: 2x8 keys, lamps + number under each */
LANE_X0 = 16, LANE_Y0 = 96, LANE_CELL = 100, LANE_PITCH_X = 108
LANE_PITCH_Y = 134            /* was 112: +12 lamp, +11 number, spacing */
LAMP_DY = 105, LAMP_W = 40, LAMP_H = 12, LAMP_ACC_DX = 8, LAMP_SLD_DX = 52
NUM_DY = 122                  /* label top; 11 px font, centred on the key */
/* lane bottom: 96 + 134 + 100 + 33 = 363; knob row at 520 is clear */

/* editor column, x 1080..1264 (NOTE_X/NOTE_Y unchanged) */
ACC_X = 1080, SLD_X = 1176, TOG_Y = 148, TOG_KEY = 56
ACC_LABEL_X = 1142, SLD_LABEL_X = 1238, TOG_LABEL_Y = 168   /* "ACC" / "SLD" */
WAVE_X/Y/W/H unchanged (1080, 222, 184, 56)
STEP_Y = 300, STEP_H = 56
PREV_X = 1080, PREV_W = 44
SEG_X  = 1130, SEG_W  = 84
NEXT_X = 1220, NEXT_W = 44
STEP_LABEL_Y = 366            /* "STEP" under the readout, dim */
```

The exact label offsets may move by a few pixels once rendered in QEMU (a
screenshot via the panel's checksum boot plus the SWD framebuffer tool on
silicon); the constraints that may NOT move are in §5. The gate's
`touch_script.txt` header documents how a percentage maps to a logical
point; re-derive before assuming.

Colours: lane LEDs `SYNTHUI_LED_BUTTON_RED`; ACC key `AMBER`, SLD key
`BLUE`; accent lamp `SYNTHUI_LAMP_COLOR_AMBER`, slide lamp
`SYNTHUI_LAMP_COLOR_BLUE`, both `SYNTHUI_LAMP_SHAPE_PILL`; seven-segment
default blue face; panel buttons `SYNTHUI_PANEL_BUTTON_ACCENT_PALE`, `on`
never set (momentary). Step-number labels `0x5F6A7C`, the selected step's
number `0xF2F1EA` (updated in `select_step`); "ACC"/"SLD"/"STEP" labels the
existing knob-label grey `0x9AA0B8`.

## 4. Code shape (`acid_box.cpp`)

- `synthui_step.h` include and every `synthui_step_*` call go; `stepCell[16]`
  becomes the LedButton objects; new `accLamp[16]`, `sldLamp[16]`,
  `numLabel[16]`, `accKey`, `sldKey`, `stepSeg`.
- Construction stays in `build_ui()` under `UIBUILD_FN` (`.progmem.acid_uibuild`,
  runs once); new helpers `mkkey()`, `mklamp()`, `mkpanelbtn()` are
  `UIBUILD_FN` too. Run-time paths (`select_step`, `commit_selected`,
  `ui_poll`, the callbacks) stay where they are.
- Two new callbacks `cbPrev`/`cbNext`; `cbStepTap` unchanged.
- `mkbtn()` stays for the four transport buttons and WAVE.

## 5. Invariants the gate depends on

`run_qemu.sh` injects three gestures from `touch_script.txt`, generated from
the geometry block: PLAY at logical (1088, 43); the CUTOFF drag at x 89,
y 583..647; and the tap on step index 2, physical `P 80 22` → logical
(281, 143), which lies in row 0 (x 232..331, y 96..195). Therefore:

1. `LANE_X0`, `LANE_Y0`, `LANE_CELL`, `LANE_PITCH_X` do not change.
2. The transport bar and the knob row do not move.
3. The lane's new bottom (363) stays above `KNOB_Y0` (520).

With those held, `touch_script.txt` is **unchanged** and the gate's audio
assertions (step 2 silent in the window before the tap, sounding after)
keep their meaning without re-derivation. If the implementation has to move
row 0 for any reason, the script is REGENERATED from the block quoted in
`transcript_qemu.txt`, never hand-edited.

What DOES move, by design: both UI goldens. The sw golden `0xE871BF09` is
re-recorded from a no-touch QEMU boot (the anti-golden check for an all-zero
buffer stays); the gpu golden `0x2231070B` is re-recorded on silicon over
THREE boots, bit-identical, per the landscape close-out's discipline. The two
sets are never reconciled.

## 6. ITCM

The default acid_box build has **2,708 B** of headroom (LOOPSTAT 2,564) and,
unlike the `M2_BT_OUT` builds, routes NO archive to flash: SynthUI sits in
ITCM (5,926 B on the BT map). Four more widgets (led_button, lamp,
seven_segment, panel_button) and their LVGL draw paths will not fit there.

Rule from NEW-45 §5, applied: **route the whole `libSynthUI` archive to
flash with `EXCLUDE_FILE` for the named hot exceptions**, in EVERY acid_box
configuration, not only `M2_BT_OUT`:

```
*libSynthUI*.a:(EXCLUDE_FILE(*synthui_rotary_knob*.obj) .text*)
```

The rotary knob objects (`synthui_rotary_knob.cpp`, `synthui_knob_math`
users, and the `vglite/synthui_rotary_knob_gpu.cpp` compositor) stay in
ITCM: they run per FRAME under a drag (the wedge-delta path and the pre-flip
compose). The new widgets are called per DRAW CALL at UI rate — the same
shape as MipiDisplay/Wire/TouchPanel, measured fine from flash on 2026-09-11
(arm b). No `.fastrun` is captured (rule 2). The derived-linker-script
mechanism (`string(REPLACE)` on the core `imxrt1176.ld`) currently lives
under `if(M2_BT_OUT)`; it moves out so the SynthUI rule applies to the
default and LOOPSTAT builds too, and the BT builds keep their existing
rules and the NEW-45 headroom ASSERT on top.

Measured, not assumed: read `--print-memory-usage` on every link (it is
already printed), record the four headroom numbers in the transcript, and
run `bench_check` at close-out (never during the sweep). LVGL stays in ITCM
(arm d); if LVGL's own growth from newly-linked draw features (gradients for
the cap, borders for the halo) eats the margin, the fallback is a per-object
FLASH rule naming those LVGL draw objects — an inclusion rule in the safe
direction, since a new LVGL file then still defaults to ITCM as today.

## 7. Verification

**QEMU gate `display/acid_box`**: every existing assertion unchanged except
the sw golden; touch script unchanged (§5); `ACIDBOX_VSYNC timeouts=0` per
bar; equality guard PASS; the engine/GPU tripwires; vacuity fixture
`transcript_qemu.txt` RE-CAPTURED and `gate-vacuity.test.sh` green
(55 cases today; the bad-golden case must still fail by name against the new
golden). Sweep at **141** (the widget gate lands first), `0 SKIP`,
`LICENSE-AUDIT: PASS` after the sweep, fresh-user `-DEVKB_FORCE_FETCH=ON`
verified by RUNNING the acid_box gate on the fetched ELF.

**Silicon (`transcript_hw_evkb.txt`)**, all with the landscape present:
- upright, the lane reads as 16 ivory keys with lamps and numbers;
- taps on all four lane corners select (key sinks, readout follows) and
  toggle (LED lights); the previously selected key rises;
- ACC and SLD light the key LED and the lamp under the selected step, and
  clear them on the second press;
- prev/next move the sunk key and the readout, wrapping 16 → 1 and 1 → 16,
  without changing the pattern (`STEP[...]` lines absent, `SELECT=` present);
- PLAY runs the red cue along the lane in step order, the cue and the sunk
  selected key visibly coexist on the same step;
- gpu golden re-recorded over three boots bit-identical;
  `ACIDBOX_VSYNC timeouts=0` across the run; no scanout flash by eye;
- touch p95 not worse than the landscape figures (median 1 s windows
  43–49 ms; the tail is a known open item, not this spec's);
- the four ITCM headroom numbers recorded; `M2_BT_OUT` still links with the
  NEW-45 ASSERT green and a Shokz stream reads `pcmdrops=0`.

## 8. Risks

- **Touch p95 under the extra objects.** 16 keys + 32 lamps + 16 labels is
  64 more LVGL objects than today's 16 cells; LVGL's hit-test and refresh
  walk them. Expected negligible (all are leaf objects with early-return
  setters), but it is measured, not assumed — the loopstat build's touch
  p95 is the instrument.
- **A cue and a latch on the same key** paint different parts (bezel vs
  cap group) and different damage boxes; the delta-equality guard covers the
  case per boot.
- **Golden churn.** Two goldens move in this spec and will move again for
  the transport follow-up. Accepted in the brainstorm (option 1) as the price
  of a reviewable diff.
