# acid_box editor on SynthUI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild acid_box's step lane and editor column on SynthUI widgets — 16 `synthui_led_button` keys with accent/slide lamps and step numbers, ACC/SLD as LED keys, a seven-segment step readout and prev/next panel buttons — keeping the gate's injected gestures and audio assertions valid.

**Architecture:** Spec: `docs/superpowers/specs/2026-09-15-acid-box-synthui-editor-design.md`. The UI swap is confined to `acid_box.cpp`: the scene-construction function (flash-resident, runs once), the two state-writing functions `select_step` / `commit_selected`, and the 33 ms poller. `synthui_step` leaves this example. Row 0 of the lane and the transport bar do not move, which is what keeps `touch_script.txt` and every audio assertion in the gate valid; both UI goldens move by design and are re-recorded, the software one only after the frame behind it has been looked at. Four more widgets in the link make ITCM the binding constraint, so `libSynthUI` is routed to flash in every configuration with the rotary knob's per-frame compositor kept in ITCM.

**Tech Stack:** LVGL 9.4 (vendored), SynthUI (sibling checkout, pinned `b599ae1`), MipiDisplay RK055 + PXP CW90 present, Teensyduino-style core for i.MX RT1176, CMake + qemu2 through `tools/qrun`, ARM GCC 10.

## Global Constraints

- `$EVKB` = `/Users/nicholasnewdigate/Development/rt1170/evkb`, `$AB` = `$EVKB/examples/display/acid_box`, `$SYNTHUI` = `/Users/nicholasnewdigate/Development/SynthUI`, `S` = the session scratchpad directory.
- Run gates as `./run_qemu.sh`, never `sh run_qemu.sh`. One QEMU run at a time; never run the sweep, the vacuity suite, the licence audit or `bench_check` concurrently with anything else.
- **Do not weaken a gate assertion to make a change pass.** If an assertion fails, either the change is wrong or the assertion needs re-deriving with a written reason.
- **Goldens are RECORDED from two consecutive bit-identical runs**, never pasted from one run, and the software golden is only recorded after the frame behind it has been dumped and viewed (Task 5).
- Every task commits to `$EVKB` on branch `new-acid-box-synthui-editor` (create it in Task 1 from master). Do not push. Commit trailer: `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`.
- SynthUI is NOT modified by this plan. If a widget defect appears, stop and report it.

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `$AB/acid_box.cpp` | geometry block, scene construction, `select_step` / `commit_selected` / `ui_poll`, two new callbacks | 1, 2, 3 |
| `$AB/CMakeLists.txt` | route `libSynthUI` to flash in every configuration, keeping the rotary compositor in ITCM | 4 |
| `$AB/run_qemu.sh` | the software golden and the eyeball record behind it | 5 |
| `$AB/transcript_qemu.txt` | the vacuity fixture and the dump recipe/eye-check record | 5 |
| `$EVKB/examples/README.md`, `$EVKB/CLAUDE.md` | what the example is, and the close-out measurement | 6 |
| `$AB/transcript_hw_evkb.txt` | silicon acceptance, the gpu golden and the ITCM headroom numbers | 7 |

`touch_script.txt` is deliberately NOT in this table: Task 1 keeps its three targets valid, and Task 5 proves it.

---

### Task 1: The lane — 16 LedButton keys, lamps, step numbers

**Files:**
- Modify: `$AB/acid_box.cpp` (includes near line 51; geometry block around lines 1113–1128; `stepCell`/state declarations around line 1135; `commit_selected` and `select_step` around lines 1202–1231; `ui_poll` around line 1290; `build_ui`'s lane loop around line 1396)

- [ ] **Step 1: Branch**

```bash
cd $EVKB && git checkout master && git status --short   # must be empty
git checkout -b new-acid-box-synthui-editor
```

- [ ] **Step 2: Swap the includes**

In `$AB/acid_box.cpp`, replace the `#include "synthui_step.h"` line with:

```cpp
#include "synthui_led_button.h"
#include "synthui_lamp.h"
```

(`synthui_rotary_knob.h` and `synthui_rotary_knob_gpu.h` stay.)

- [ ] **Step 3: Extend the geometry block**

Replace the pattern-band constants with the block below. `LANE_PITCH_Y` is the only existing lane value that changes; `LANE_X0`, `LANE_Y0`, `LANE_CELL` and `LANE_PITCH_X` MUST NOT change (the gate's step-2 tap lands at logical (281,143), inside row 0's cell 2 at 232..331 x 96..195).

```cpp
/* pattern band */
static constexpr int LANE_X0 = 16,  LANE_Y0 = 96, LANE_CELL = 100, LANE_PITCH_X = 108;
/* Row pitch grew 112 -> 134 for the lamp strip and the step number: 100 px key
 * + 12 px lamps + an 11 px number + spacing.  ROW 0 DOES NOT MOVE, which is
 * what keeps touch_script.txt valid -- cell 2 is still 232..331 x 96..195 and
 * the gate's tap still lands at (281,143).  Row 1 therefore sits at y 230 and
 * the lane's last pixel row is 96 + 134 + 100 + 33 = 363, clear of the knob
 * row at 520. */
static constexpr int LANE_PITCH_Y = 134;
static constexpr int LAMP_DY = 105, LAMP_W = 40, LAMP_H = 12;
static constexpr int LAMP_ACC_DX = 8, LAMP_SLD_DX = 52;
static constexpr int NUM_DY = 122;      /* step-number label top, centred on the key */
```

- [ ] **Step 4: Widen the per-cell state arrays**

Replace `static lv_obj_t *stepCell[16];` with:

```cpp
static lv_obj_t *stepCell[16];          /* synthui_led_button: lit = gate, cue = playhead, latched pressed = selected */
static lv_obj_t *accLamp[16], *sldLamp[16], *numLabel[16];
```

- [ ] **Step 5: Rewrite `commit_selected`'s view half**

Replace the two `synthui_step_*` calls in `commit_selected` so the function reads:

```cpp
static void commit_selected(uint8_t note, bool gate, bool accent, bool slide)
{
    seq.step(selectedStep, note, gate, accent, slide);
    synthui_led_button_set_lit(stepCell[selectedStep], gate);
    synthui_lamp_set_on(accLamp[selectedStep], accent);
    synthui_lamp_set_on(sldLamp[selectedStep], slide);
    lv_label_set_text(noteLabel, gate ? noteName(note) : "--");
    lv_obj_set_style_bg_color(accBtn, accent ? lv_color_hex(0x5b62b8) : lv_color_hex(0x232b3a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sldBtn, slide  ? lv_color_hex(0x5b62b8) : lv_color_hex(0x232b3a), LV_PART_MAIN);
    CONSOLE.printf("STEP[%d]=note%u gate%d acc%d sld%d\n",
                   selectedStep, (unsigned)note,
                   gate ? 1 : 0, accent ? 1 : 0, slide ? 1 : 0);
}
```

The `accBtn` / `sldBtn` background swaps stay for now; Task 2 replaces those objects and removes these two lines. The `STEP[...]` line is unchanged — the gate greps it exactly.

- [ ] **Step 6: Rewrite `select_step`**

```cpp
static void select_step(int i)
{
    synthui_led_button_set_pressed(stepCell[selectedStep], false);
    lv_obj_set_style_text_color(numLabel[selectedStep], lv_color_hex(0x5f6a7c), LV_PART_MAIN);
    selectedStep = i;
    synthui_led_button_set_pressed(stepCell[i], true);
    lv_obj_set_style_text_color(numLabel[i], lv_color_hex(0xf2f1ea), LV_PART_MAIN);
    const AcidStep st = seq.step(i);
    synthui_rotary_knob_set_angle(pitchKnob, noteToAngle(st.note ? st.note : 33));
    lv_label_set_text(noteLabel, st.gate ? noteName(st.note) : "--");
    lv_obj_set_style_bg_color(accBtn, st.accent ? lv_color_hex(0x5b62b8) : lv_color_hex(0x232b3a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sldBtn, st.slide  ? lv_color_hex(0x5b62b8) : lv_color_hex(0x232b3a), LV_PART_MAIN);
    CONSOLE.printf("SELECT=%d\n", i);
}
```

`SELECT=` is new bench evidence; no gate reads it. The step readout is added in Task 2.

- [ ] **Step 7: Move the playhead onto `cue`**

In `ui_poll`, replace the two cursor calls:

```cpp
        if (shownCursor >= 0) synthui_led_button_set_cue(stepCell[shownCursor], false);
        if (s >= 0)           synthui_led_button_set_cue(stepCell[s], true);
```

- [ ] **Step 8: Build the lane**

The spec sketched `mkkey()`/`mklamp()` helpers; this plan constructs inline instead, because `build_ui` is already `UIBUILD_FN` (`.progmem.acid_uibuild`, `noinline`) so the code is flash-resident either way, and three one-use helpers would add call sites without removing any repetition. `mkbtn()` and `mkknob()` stay for the transport buttons, WAVE and the eight sound knobs.

Replace the lane loop in `build_ui` with:

```cpp
    /* step lane: 2x8 keys, each with an accent lamp, a slide lamp and a number */
    for (int i = 0; i < 16; i++) {
        const int x = LANE_X0 + (i % 8) * LANE_PITCH_X;
        const int y = LANE_Y0 + (i / 8) * LANE_PITCH_Y;
        lv_obj_t *c = synthui_led_button_create(scr);
        lv_obj_set_size(c, LANE_CELL, LANE_CELL);
        lv_obj_set_pos(c, x, y);
        synthui_led_button_set_color(c, SYNTHUI_LED_BUTTON_RED);
        synthui_led_button_set_lit(c, kPreset[i].gate);
        lv_obj_add_event_cb(c, cbStepTap, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        stepCell[i] = c;

        lv_obj_t *al = synthui_lamp_create(scr);
        lv_obj_set_size(al, LAMP_W, LAMP_H);
        lv_obj_set_pos(al, x + LAMP_ACC_DX, y + LAMP_DY);
        synthui_lamp_set_shape(al, SYNTHUI_LAMP_SHAPE_PILL);
        synthui_lamp_set_color(al, SYNTHUI_LAMP_COLOR_AMBER);
        synthui_lamp_set_on(al, kPreset[i].accent);
        accLamp[i] = al;

        lv_obj_t *sl = synthui_lamp_create(scr);
        lv_obj_set_size(sl, LAMP_W, LAMP_H);
        lv_obj_set_pos(sl, x + LAMP_SLD_DX, y + LAMP_DY);
        synthui_lamp_set_shape(sl, SYNTHUI_LAMP_SHAPE_PILL);
        synthui_lamp_set_color(sl, SYNTHUI_LAMP_COLOR_BLUE);
        synthui_lamp_set_on(sl, kPreset[i].slide);
        sldLamp[i] = sl;

        lv_obj_t *n = lv_label_create(scr);
        char nb[4];
        snprintf(nb, sizeof nb, "%02d", i + 1);
        lv_label_set_text(n, nb);
        lv_obj_set_style_text_color(n, lv_color_hex(0x5f6a7c), LV_PART_MAIN);
        lv_obj_set_pos(n, x + LANE_CELL / 2 - 10, y + NUM_DY);
        numLabel[i] = n;
    }
```

- [ ] **Step 9: Build**

Run: `cd $AB && cmake --build build 2>&1 | tail -5`
Expected: links, no warnings from `acid_box.cpp`. A `synthui_step` symbol error means a call was missed — `grep -n synthui_step acid_box.cpp` must return nothing.

- [ ] **Step 10: Boot and read the tokens (golden expected to fail)**

Run: `cd $AB && ./run_qemu.sh 2>&1 | tail -30`
Expected: `FAIL: UI golden` (the scene changed on purpose — do NOT touch the golden yet), and BEFORE that failure the capture must already show `PANEL_OK`, `CODEC_OK`, `I2C_OK` and an `ACIDBOX_UI_SUM=` line that is neither the old golden nor the all-zero `0x9BC99DC5`. Record the new sum; it must be identical on a second run.

- [ ] **Step 11: Commit**

```bash
cd $EVKB
git add examples/display/acid_box/acid_box.cpp
git commit -m "acid_box: step lane on synthui_led_button with accent/slide lamps and step numbers; row 0 geometry unchanged so the touch script stays valid

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 2: The editor column — ACC/SLD keys, step readout, prev/next

**Files:**
- Modify: `$AB/acid_box.cpp` (includes; geometry block; `accBtn`/`sldBtn` declarations; `commit_selected`, `select_step`; `cbAccBtn`/`cbSldBtn` neighbourhood; `build_ui`'s editor section)

- [ ] **Step 1: Add the two widget includes**

```cpp
#include "synthui_seven_segment.h"
#include "synthui_panel_button.h"
```

- [ ] **Step 2: Replace the editor geometry constants**

```cpp
static constexpr int ACC_X = 1080,  SLD_X = 1176, TOG_Y = 148, TOG_KEY = 56;
static constexpr int ACC_LABEL_X = 1142, SLD_LABEL_X = 1238, TOG_LABEL_Y = 168;
static constexpr int WAVE_X = 1080, WAVE_Y = 222, WAVE_W = 184, WAVE_H = 56;
static constexpr int STEP_Y = 300,  STEP_H = 56;
static constexpr int PREV_X = 1080, PREV_W = 44;
static constexpr int SEG_X  = 1130, SEG_W  = 84;
static constexpr int NEXT_X = 1220, NEXT_W = 44;
static constexpr int STEP_LABEL_X = 1152, STEP_LABEL_Y = 366;
```

`TOG_W`/`TOG_H` are gone; every use is replaced below.

- [ ] **Step 3: Re-declare the editor objects**

Replace the `accBtn, sldBtn` entries in the object declaration line so it reads:

```cpp
static lv_obj_t *playBtnLabel, *bpmLabel, *noteLabel, *waveBtnLabel;
static lv_obj_t *accKey, *sldKey, *stepSeg;
```

- [ ] **Step 4: Point the two state writers at the keys**

In `commit_selected`, replace the two `lv_obj_set_style_bg_color(...)` lines with:

```cpp
    synthui_led_button_set_lit(accKey, accent);
    synthui_led_button_set_lit(sldKey, slide);
```

In `select_step`, replace its two `lv_obj_set_style_bg_color(...)` lines with:

```cpp
    synthui_led_button_set_lit(accKey, st.accent);
    synthui_led_button_set_lit(sldKey, st.slide);
    char sb[4];
    snprintf(sb, sizeof sb, "%02d", i + 1);
    synthui_seven_segment_set_text(stepSeg, sb);
```

- [ ] **Step 5: Add the prev/next callbacks**

Directly after `cbSldBtn`:

```cpp
/* Selection only -- prev/next never change the pattern, which is what makes
 * them safe to hold down while auditioning a bar. */
static void cbPrevStep(lv_event_t *e)
{ (void)e; select_step((selectedStep + 15) % 16); }
static void cbNextStep(lv_event_t *e)
{ (void)e; select_step((selectedStep + 1) % 16); }
```

- [ ] **Step 6: Build the editor column**

In `build_ui`, replace the ACC/SLD button construction with:

```cpp
    accKey = synthui_led_button_create(scr);
    lv_obj_set_size(accKey, TOG_KEY, TOG_KEY);
    lv_obj_set_pos(accKey, ACC_X, TOG_Y);
    synthui_led_button_set_color(accKey, SYNTHUI_LED_BUTTON_AMBER);
    lv_obj_add_event_cb(accKey, cbAccBtn, LV_EVENT_CLICKED, NULL);
    lv_obj_t *accLbl = lv_label_create(scr);
    lv_label_set_text(accLbl, "ACC");
    lv_obj_set_style_text_color(accLbl, lv_color_hex(0x9aa0b8), LV_PART_MAIN);
    lv_obj_set_pos(accLbl, ACC_LABEL_X, TOG_LABEL_Y);

    sldKey = synthui_led_button_create(scr);
    lv_obj_set_size(sldKey, TOG_KEY, TOG_KEY);
    lv_obj_set_pos(sldKey, SLD_X, TOG_Y);
    synthui_led_button_set_color(sldKey, SYNTHUI_LED_BUTTON_BLUE);
    lv_obj_add_event_cb(sldKey, cbSldBtn, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sldLbl = lv_label_create(scr);
    lv_label_set_text(sldLbl, "SLD");
    lv_obj_set_style_text_color(sldLbl, lv_color_hex(0x9aa0b8), LV_PART_MAIN);
    lv_obj_set_pos(sldLbl, SLD_LABEL_X, TOG_LABEL_Y);
```

and add, after the WAVE button:

```cpp
    /* step readout: < [NN] > .  Momentary panel buttons -- `on` is never set. */
    lv_obj_t *prev = synthui_panel_button_create(scr);
    lv_obj_set_size(prev, PREV_W, STEP_H);
    lv_obj_set_pos(prev, PREV_X, STEP_Y);
    synthui_panel_button_set_glyph(prev, SYNTHUI_PANEL_BUTTON_GLYPH_REWIND);
    synthui_panel_button_set_accent(prev, SYNTHUI_PANEL_BUTTON_ACCENT_PALE);
    lv_obj_add_event_cb(prev, cbPrevStep, LV_EVENT_CLICKED, NULL);

    stepSeg = synthui_seven_segment_create(scr);
    lv_obj_set_size(stepSeg, SEG_W, STEP_H);
    lv_obj_set_pos(stepSeg, SEG_X, STEP_Y);
    synthui_seven_segment_set_text(stepSeg, "01");

    lv_obj_t *next = synthui_panel_button_create(scr);
    lv_obj_set_size(next, NEXT_W, STEP_H);
    lv_obj_set_pos(next, NEXT_X, STEP_Y);
    synthui_panel_button_set_glyph(next, SYNTHUI_PANEL_BUTTON_GLYPH_FORWARD);
    synthui_panel_button_set_accent(next, SYNTHUI_PANEL_BUTTON_ACCENT_PALE);
    lv_obj_add_event_cb(next, cbNextStep, LV_EVENT_CLICKED, NULL);

    lv_obj_t *stepLbl = lv_label_create(scr);
    lv_label_set_text(stepLbl, "STEP");
    lv_obj_set_style_text_color(stepLbl, lv_color_hex(0x5f6a7c), LV_PART_MAIN);
    lv_obj_set_pos(stepLbl, STEP_LABEL_X, STEP_LABEL_Y);
```

`select_step(0)` already runs at the end of `build_ui`; it now also writes the readout, so the first frame shows `01` rather than the widget's constructor default.

- [ ] **Step 7: Build and boot**

Run: `cd $AB && cmake --build build 2>&1 | tail -3 && ./run_qemu.sh 2>&1 | tail -25`
Expected: builds with no warnings; still `FAIL: UI golden`; the capture shows `SELECT=0` once during boot and no `accBtn`/`sldBtn` compile leftovers (`grep -n "accBtn\|sldBtn\|TOG_W\|TOG_H" acid_box.cpp` returns nothing).

- [ ] **Step 8: Commit**

```bash
cd $EVKB
git add examples/display/acid_box/acid_box.cpp
git commit -m "acid_box: ACC/SLD as amber and blue LedButtons, seven-segment step readout, prev/next panel buttons (selection only)

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 3: Prove the gestures and the wrap on the real gate path

**Files:**
- Modify: none (this task only runs and reads)

The gate's three injected gestures must still land, and prev/next must wrap. The touch script cannot press the new buttons (its instants are fixed), so the wrap is checked by reasoning plus a temporary probe build that is NOT committed.

- [ ] **Step 1: Confirm the scripted gestures still land**

Run: `cd $AB && ./run_qemu.sh 2>&1 | grep -E "^PLAYING=1|^STEP\[2\]=note33 gate1|CUTOFF=" | head -3`
Expected: all three present, in that order. `STEP[2]=note33 gate1` proves the step-2 tap still hits cell 2 after the row-pitch change — the load-bearing invariant of this rework. If it is missing, STOP: row 0 moved, and `touch_script.txt` must be regenerated from the block quoted in `transcript_qemu.txt` rather than hand-edited.

- [ ] **Step 2: Probe the wrap with a temporary build**

Copy the example to the scratchpad so nothing in the repo changes:

```bash
cp -R $AB $S/ab_probe && cd $S/ab_probe && rm -rf build build-* 
```
In `$S/ab_probe/acid_box.cpp`, at the end of `build_ui` and after `select_step(0)`, add:

```cpp
    /* PROBE ONLY -- never committed */
    for (int k = 0; k < 3; k++) cbPrevStep(NULL);   /* 0 -> 15 -> 14 -> 13 */
    for (int k = 0; k < 4; k++) cbNextStep(NULL);   /* 13 -> 14 -> 15 -> 0 -> 1 */
```
Build and boot it bounded:
```bash
cd $S/ab_probe && cmake -B build -DCMAKE_TOOLCHAIN_FILE=$EVKB/toolchain/rt1170-evkb.toolchain.cmake >/dev/null && cmake --build build >/dev/null 2>&1
QRUN_TIMEOUT=25 $EVKB/tools/qrun -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on \
  -kernel build/acid_box.elf -display none -serial file:$S/ab_probe.uart
grep "^SELECT=" $S/ab_probe.uart | head -8
```
Expected exactly: `SELECT=0`, `SELECT=15`, `SELECT=14`, `SELECT=13`, `SELECT=14`, `SELECT=15`, `SELECT=0`, `SELECT=1` — the two wraps (0→15 and 15→0) are the point. No `STEP[` line may appear between them: prev/next must not write the pattern.

- [ ] **Step 3: Delete the probe**

```bash
rm -rf $S/ab_probe $S/ab_probe.uart
cd $EVKB && git status --short   # must show no change under examples/display/acid_box
```

- [ ] **Step 4: Record the result**

No commit. Paste the eight `SELECT=` lines into the task report; Task 6 quotes them in the close-out.

---

### Task 4: ITCM — route libSynthUI to flash in every configuration

**Files:**
- Modify: `$AB/CMakeLists.txt` (the linker-script derivation currently inside `if(M2_BT_OUT)`)

Four more widgets in the link make ITCM binding: the default build had 2,708 B of headroom before this rework and SynthUI already occupied 5,926 B of ITCM in the BT map.

- [ ] **Step 1: Read the existing mechanism**

Read `$AB/CMakeLists.txt` from the `if(M2_BT_OUT)` line to its `endif()`, and the two rules quoted there (NEW-45 §5): route a WHOLE ARCHIVE with `EXCLUDE_FILE` for named hot exceptions, never an inclusion list; never capture `.fastrun`.

- [ ] **Step 2: Lift the derivation out of the BT branch**

Restructure so the linker script is derived for EVERY configuration, with the BT-only rules still applied only when `M2_BT_OUT` is on. The SynthUI rule, which applies in all configurations, is:

```
		/* NEW-25 consumer rework: the editor links four more SynthUI widgets
		   (led_button, lamp, seven_segment, panel_button) and ITCM is the
		   binding constraint -- the default build had 2,708 B of headroom.
		   These widgets are called per DRAW CALL at UI rate, the same shape as
		   MipiDisplay/Wire/TouchPanel, which measured fine from flash on
		   2026-09-11 (NEW-45 arm b) with the I-cache covering them.
		   EXCLUDED and kept in ITCM: the rotary knob and its GC355 compositor,
		   which run per FRAME under a drag (wedge-delta + pre-flip compose).
		   Whole archive with an exclusion, per NEW-45 rule 1, so a NEW SynthUI
		   file defaults to FLASH rather than to ITCM.  No `.fastrun`, per rule 2. */
		*libSynthUI*.a:(EXCLUDE_FILE(*synthui_rotary_knob.cpp.obj *synthui_rotary_knob_gpu.cpp.obj) .text*)
```

Keep the existing `FATAL_ERROR` guard that checks the NEW-45 headroom ASSERT was injected; it must still fire for the BT builds.

- [ ] **Step 3: Rebuild all four configurations and read the headroom**

```bash
cd $AB
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake >/dev/null && cmake --build build 2>&1 | grep -A3 "Memory region"
cmake --build build-loopstat 2>&1 | grep -A3 "Memory region"
cmake --build build-bt 2>&1 | grep -A3 "Memory region"
```
Expected: every link prints its region table (`--print-memory-usage` is already on), all four link, and ITCM headroom is LARGER than the pre-rework numbers recorded in `CMakeLists.txt` (default 2,708 B, LOOPSTAT 2,564 B, build-bt 11,364 B, build-bench 11,188 B). Record the four new numbers. A raw `region ITCM overflowed` means the rule did not match — check the archive name with `ls build/**/libSynthUI.a` rather than guessing.

- [ ] **Step 4: Confirm the rotary knob is still in ITCM**

```bash
cd $AB && arm-none-eabi-nm -S --size-sort build/acid_box.elf | grep -i rotary | head -5
```
(Use `/Applications/ARM_10/bin/arm-none-eabi-nm` if the bare name is not on PATH.) Expected: the rotary knob's symbols are present with addresses below `0x00040000` (ITCM), while e.g. `synthui_lamp` symbols sit in flash (`0x30...`). If the rotary symbols moved to flash, the `EXCLUDE_FILE` object names do not match the archive's member names — check with `arm-none-eabi-nm build/.../libSynthUI.a | grep '\.cpp\.obj'`.

- [ ] **Step 5: Boot to confirm nothing regressed**

Run: `cd $AB && ./run_qemu.sh 2>&1 | tail -20`
Expected: still `FAIL: UI golden` and nothing else new; `ACIDBOX_VSYNC ... timeouts=0` present, the equality-guard lines present, and the `ACIDBOX_UI_SUM` identical to Task 2's (routing code to flash must not change a pixel).

- [ ] **Step 6: Update the headroom comment and commit**

Update the ITCM comment block in `CMakeLists.txt` with the four measured numbers and the date, then:

```bash
cd $EVKB
git add examples/display/acid_box/CMakeLists.txt
git commit -m "acid_box: route libSynthUI to flash in every configuration, rotary knob and its GC355 compositor excluded (per-frame); ITCM headroom re-measured

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 5: Re-record the software golden, with the frame looked at

**Files:**
- Modify: `$AB/run_qemu.sh` (the golden and the eyeball record)
- Modify: `$AB/transcript_qemu.txt` (the fixture)

**This tree does not record a golden for a frame nobody has seen.** Both the knob pilot's clamped arc and the VGLite GPU frame were perfectly reproducible and visibly wrong.

- [ ] **Step 1: Two runs, same sum**

```bash
cd $AB && ./run_qemu.sh > $S/ab1.log 2>&1; grep -a "ACIDBOX_UI_SUM=" build/acid_box.uart
./run_qemu.sh > $S/ab2.log 2>&1; grep -a "ACIDBOX_UI_SUM=" build/acid_box.uart
```
Expected: the same value twice, and not `0x9BC99DC5`. If they differ, STOP and diagnose nondeterminism (do not pick one).

- [ ] **Step 2: Dump the frame with a NO-TOUCH script**

A missing `touch-script` makes the model replay its built-in gesture table, which pokes the UI and produces a stable checksum for a frame that has been silently edited. So:

```bash
printf '# no-touch\n' > $S/notouch.txt
for i in $(seq 40); do echo R >> $S/notouch.txt; done
cd $AB
$EVKB/tools/qrun -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on \
  -kernel build/acid_box.elf -display none -serial file:$S/fb.uart \
  -global driver=imxrt.gt911,property=touch-script,value=$S/notouch.txt \
  -monitor unix:$S/acidmon.sock,server,nowait &
sleep 12
printf 'xp/1wx 0x4080820c\n' | nc -U $S/acidmon.sock      # the scanned buffer address
ADDR=$(printf 'xp/1wx 0x4080820c\n' | nc -U $S/acidmon.sock | grep -oE '0x[0-9a-f]{8}' | tail -1)
printf 'pmemsave %s 0x384000 "%s/fb.raw"\n' "$ADDR" "$S" | nc -U $S/acidmon.sock
```
Then convert, undoing the CW90 present (`-alpha off` is load-bearing: the X byte is 0, so a BGRA read makes every pixel transparent and the PNG looks blank):

```bash
convert -size 720x1280 -depth 8 bgra:$S/fb.raw -alpha off -rotate -90 $S/fb.png
```
Confirm `grep -a ACIDBOX_UI_SUM= $S/fb.uart` equals Step 1's value — the dump must be of the same frame the golden checksums.

- [ ] **Step 3: LOOK at the PNG and check the list**

Open `$S/fb.png` (send it to the user with SendUserFile) and confirm, item by item:
- "ACID BOX" top-left; `-` / `128.0` / `+` centred; PLAY and STOP top-right (unchanged bar);
- the 2x8 lane: ivory keys with a red LED lit on the gated steps and dark on rests 2, 5, 9, 14;
- amber accent lamps under steps 0, 7, 12 and blue slide lamps under 3, 10, 15, matching `kPreset`;
- step numbers 01..16 under the lamps, with 01 brighter than the rest;
- key 1 visibly SUNK (the selection latch) and no key showing a red cue bezel (the transport is stopped);
- the editor column: pitch knob at A1, ACC lit amber, SLD dark, SAW, and `01` on the seven-segment between the two pale panel buttons;
- the eight sound knobs along the bottom at their boot angles; nothing mirrored or lying on its side.

If anything is wrong, fix the code and return to Step 1. Do not proceed with a golden for a frame that fails this list.

- [ ] **Step 4: Pin the golden and record what was seen**

In `run_qemu.sh` replace `ACIDBOX_UI_SUM=0xE871BF09` with the recorded value, and add a dated paragraph to the golden's comment block saying what the re-goldened frame is (the SynthUI editor rework) and what was checked in Step 3. Keep the `\r?$` anchor and the all-zero anti-golden check.

- [ ] **Step 5: Demonstrate RED, then GREEN**

```bash
cd $AB && sed -i.bak 's/ACIDBOX_UI_SUM=0x[0-9A-F]\{8\}\\r?\$/ACIDBOX_UI_SUM=0xDEADBEEF\\r?$/' run_qemu.sh && ./run_qemu.sh 2>&1 | tail -2   # expect FAIL: UI golden
mv run_qemu.sh.bak run_qemu.sh && ./run_qemu.sh 2>&1 | tail -2                                                                          # expect PASS
```

- [ ] **Step 6: Re-capture the fixture and update its dump record**

```bash
cd $AB && cp build/acid_box.uart transcript_qemu.txt
```
Then update the transcript's dump-recipe section with the new scanned-buffer address if it changed and the new eye-check list from Step 3.

- [ ] **Step 7: Vacuity**

Run: `cd $EVKB && ./tools/gate-vacuity.test.sh > $S/vac.log 2>&1; grep -c "^PASS:" $S/vac.log; grep "^FAIL:" $S/vac.log`
Expected: 62 PASS, no FAIL (the acid_box cases replay the NEW fixture against the NEW gate). One suite at a time — an aborted run with a missing `$WORK` file means two overlapped.

- [ ] **Step 8: Commit**

```bash
cd $EVKB
git add examples/display/acid_box/run_qemu.sh examples/display/acid_box/transcript_qemu.txt
git commit -m "acid_box: software golden re-recorded for the SynthUI editor -- frame dumped with a no-touch script and eye-checked before pinning; fixture re-captured

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 6: Docs, sweep, audit, bench_check

**Files:**
- Modify: `$EVKB/examples/README.md` (the acid_box entry in the display row)
- Modify: `$EVKB/CLAUDE.md` (a close-out measurement block)

- [ ] **Step 1: README**

In the `**display**` row's `acid_box` entry, replace the phrase describing the UI (`8 knobs, a step editor and a 2x8 step lane`) with:

```
8 knobs, and a step editor built on SynthUI widgets -- a 2x8 lane of `synthui_led_button` keys (LED = gate, red bezel = playhead, sunk = selected) with amber accent and blue slide lamps and step numbers, ACC/SLD as amber and blue LED keys, a `synthui_seven_segment` step readout between REWIND/FORWARD panel buttons
```
Keep the row one line and leave the golden values in that entry to Step 3.

- [ ] **Step 2: Sweep and audit, one at a time**

```bash
cd $EVKB && ./tools/run-all-qemu-gates.sh > $S/sweep.log 2>&1; echo exit=$?; tail -2 $S/sweep.log
./tools/license-audit.sh > $S/audit.log 2>&1; tail -2 $S/audit.log
./tools/build-bench-configs.sh -n > $S/bench.log 2>&1; tail -3 $S/bench.log
```
Expected: `gates: 141 passed` exit 0 (the count does NOT change — this rework adds no gate); `LICENSE-AUDIT: PASS`; `bench_check` builds both declared bench configurations and its nm-diff pairs match. Never run these concurrently. A red in the documented load-sensitivity class is re-run idle before it is believed.

- [ ] **Step 3: Fresh-user check — the pin must carry the widget this example now references**

Until this rework, `synthui_led_button.cpp` was compiled into every SynthUI-linking example by the `src/*.cpp` glob and then dropped by `--gc-sections`, so no example proved the PINNED SynthUI actually contains the widget. acid_box now references it, so verify the fetch path end to end:

```bash
cd $AB
rm -rf build-fetch
cmake -B build-fetch -DEVKB_FORCE_FETCH=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake > $S/ab-fetch-configure.log 2>&1
grep -i "synthui" $S/ab-fetch-configure.log | head -3
cmake --build build-fetch > $S/ab-fetch-build.log 2>&1; echo build=$?
mv build build-local && ln -s build-fetch build && ./run_qemu.sh > $S/ab-fetch-gate.log 2>&1; rc=$?; rm build && mv build-local build; echo gate=$rc
ls -la build | head -1     # must be a real directory again
rm -rf build-fetch
```
Expected: the configure log shows SynthUI fetched at the pinned SHA, the build succeeds, and the gate PASSES (`gate=0`) against the fetched-source ELF. A configure that succeeds only proves the subdirectory resolves; only the gate run proves the fetched widget behaves.

- [ ] **Step 4: CLAUDE.md close-out block**

Add a `✅ **Measured YYYY-MM-DD: 141 gates discovered, 141 passed, 0 failed, 0 SKIP**` block (today's date, from `date +%F`) above the newest existing block, recording: the sweep line and wall time, audit PASS + manifest count, vacuity 62/62, the fresh-user gate run on the fetched ELF, the new software golden, the four ITCM headroom numbers from Task 4, and that the gpu golden is re-recorded on silicon in Task 7. State plainly that the gate count is unchanged and why (the rework adds no gate; it moves one golden).

- [ ] **Step 5: Commit**

```bash
cd $EVKB
git add examples/README.md CLAUDE.md
git commit -m "docs: acid_box editor on SynthUI -- close-out, sweep 141/141/0, audit PASS, ITCM headroom after routing libSynthUI to flash

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 7: Silicon acceptance (bench session)

**Files:**
- Modify: `$AB/transcript_hw_evkb.txt`

Needs the EVKB on the bench. Follow the `flashing-rt1170-evkb` skill; the notes below are this tree's recorded recipe.

- [ ] **Step 1: Flash with nothing holding the VCOM**

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
cd $AB && LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/acid_box.hex
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build/acid_box.hex
```
Expected: `File matches flash`. Attach the console only after verify, then press SW4.

- [ ] **Step 2: Record the gpu golden over three boots**

With the GC355 compositor active, `ACIDBOX_UI_SUM` is the gpu golden (a separate set from the software one, never reconciled). Press SW4 three times and confirm the value is bit-identical on all three; the portrait-era `0x1479CEE8` and landscape `0x2231070B` are both retired by this rework.

- [ ] **Step 3: Touch acceptance, by hand**

Confirm on glass, and write each result into the transcript:
- taps on all four lane corners select (the key sinks, the readout follows) and toggle the gate (the LED lights); the previously selected key rises;
- ACC and SLD light their own LED and the matching lamp under the selected step, and clear on a second press;
- prev/next move the sunk key and the readout, wrapping 16→1 and 1→16, with no `STEP[...]` line printed;
- PLAY runs the red cue bezel along the lane in step order, and the cue and the sunk selected key coexist on the same step;
- `ACIDBOX_VSYNC ... timeouts=0` across the run and no scanout flash by eye over a few hundred frames.

- [ ] **Step 4: Performance and the BT build**

- Touch p95 from an `ACIDBOX_LOOPSTAT` build, compared against the landscape figures (median 1 s windows 43–49 ms). "Not worse" is the bar; the tail is a known open item and not this rework's to close.
- Flash the `M2_BT_OUT` build, confirm the NEW-45 ITCM headroom ASSERT is green at link, stream to the Shokz and confirm `pcmdrops=0`.

- [ ] **Step 5: Commit the transcript**

```bash
cd $EVKB
git add examples/display/acid_box/transcript_hw_evkb.txt
git commit -m "acid_box: silicon acceptance for the SynthUI editor -- gpu golden over three boots, touch on all four lane corners, prev/next wrap, fences clean, BT build headroom green

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

Then file the rework's Linear issue as Done with the transcript's summary, and note in it that the LedButton's own silicon fps checkpoint (NEW-25 Task 7) rides along with this bench session.
