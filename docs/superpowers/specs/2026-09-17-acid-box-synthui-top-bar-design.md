# acid_box top bar and SAW/SQR toggle on SynthUI — design

Date: 2026-09-17
Status: approved (brainstorm 2026-09-17, mockups reviewed option by option)
Tracking: Linear **NEW-54**.
Follows: `2026-09-15-acid-box-synthui-editor-design.md` (NEW-51), whose §1
named exactly these controls as its out-of-scope follow-up.
Related: `2026-09-14-acid-box-landscape-design.md` (the present pipeline and
the touch-script geometry this keeps), `2026-09-11-acid-box-itcm-headroom-design.md`
(the ITCM floor §6 reads against), NEW-27 PanelButton, NEW-29 SevenSegment,
NEW-30 SlideToggle.

## 1. Goal & scope

Replace acid_box's last stock-LVGL controls with SynthUI widgets, so the whole
panel is one widget family:

| Control | Today | After |
|---|---|---|
| PLAY | `mkbtn()`, `LV_SYMBOL_PLAY`/`PAUSE` label swapped by the poller | `synthui_panel_button`, `GLYPH_PLAY`, green accent, **`on` = playing** |
| STOP | `mkbtn()`, `LV_SYMBOL_STOP` | `synthui_panel_button`, `GLYPH_STOP`, pale accent, momentary-lit |
| tempo − / + | `mkbtn()` "-" / "+" | `synthui_panel_button` `GLYPH_DOWN` / `GLYPH_UP`, pale accent, momentary-lit |
| tempo readout | `lv_label` "128.0" | `synthui_seven_segment`, fixed five-cell field |
| SAW/SQR | `mkbtn()` with a "SAW"/"SQR" label | `synthui_slide_toggle`, SAW left / SQUARE right |

**No SynthUI change.** Every widget is used through its existing API, so this
is evkb-only: no SynthUI push, no `evkb.cmake` pin bump, no rebuild of the
SynthUI-linking dirs beyond acid_box's own, no fresh-user cycle (the fetched
sources are identical).

**Out of scope**: the title, the note-name label, the ACC/SLD/STEP captions,
the step numbers and the knob captions stay `lv_label`. No new behaviour
(no hold-to-repeat on tempo, no third waveform). The GC355 compositor is
untouched — it draws knobs only.

## 2. Three widget facts that shaped the design

Found by reading the widgets, not assumed from their names. Each closed off
the "obvious" port.

1. **`synthui_panel_button` has no PAUSE glyph** (PLAY, STOP, RECORD, REWIND,
   FORWARD, UP, DOWN, BAR, DOT, NONE). Today's ▶/⏸ label swap cannot carry
   over without inventing a glyph the DC reference does not have. Hence PLAY's
   `on` state carries "playing" instead (§3).
2. **`synthui_slide_toggle` paints an OPAQUE plate over its whole box and draws
   its glyphs in a FIXED `#232526`.** There is `set_panel_color()` but no
   glyph-colour setter (the DC source hard-codes `glyphColor` too). Matching
   the plate to acid_box's `#101820` ground would leave dark-on-darker legends
   — invisible. Hence a visible plate (§3).
3. **`synthui_seven_segment` scales on HEIGHT (`u = h/112`) and its content
   width follows the text** (76 units per character cell, 44 for `.`/`:`, plus
   a `112·tan(slant)` overhang). It neither fits to its box width nor centres.
   "128.0" at the bar's 48 px height is 154.19 px wide against today's 80 px
   gap between − and +; and "99.0" is one cell narrower than "100.0". Hence
   the cluster widens and the text is a fixed-width field (§3).

## 3. Decisions (each chosen from mockups)

**Transport — two buttons, PLAY lit while playing.** Rects unchanged: PLAY
1040..1139 × 20..67, STOP 1156..1255 × 20..67. `cbPlay` keeps its play/pause
toggle and `cbStop` its rewind; paused and stopped both show PLAY dark, as on
most hardware. Rejected: a new `GLYPH_PAUSE` (a SynthUI change for a glyph the
reference lacks); a single run/stop button (drops the pause-vs-stop distinction
the transport has).

**Tempo — full-height readout, DOWN/UP panel buttons.**

| Constant | Before | After |
|---|---|---|
| `TEMPO_DN_X` | 560 | 560 (unchanged; 560..609) |
| `BPM_X`, `BPM_Y` | 624, 35 | deleted |
| `TEMPO_SEG_X`, `TEMPO_SEG_W` | — | 618, 156 (618..773 × 20..67) |
| `TEMPO_UP_X` | 690 | 780 (780..829) |

156 because five cells at h = 48 are (4·76 + 44 + 11.77)·48/112 = 154.19 px
and the widget `ceilf`s the last cell's right edge to 155. Rejected: shrinking
the readout into the 80 px gap (24.5 px tall, ~2.6 px segments); leaving − / +
as the last two stock LVGL buttons.

**Waveform — slide toggle on a `#6D7A85` plate.** Rect unchanged (1080..1263 ×
222..277, 184 × 56). At that size the widget's width-normalised geometry gives
~34 px glyphs and a 48 × 33 px knob. `#6D7A85` is one of the DC reference's own
four panel colours and keeps the fixed dark glyphs legible; it reads as a
silk-screened legend plate let into the panel. Rejected: `#39434B` (glyphs drop
to low contrast), the default `#B9BCBC` (brightest object on the panel), and a
new `set_glyph_color()` (the only option needing a SynthUI change).

**Geometry and hazard 2.** The gate's touch script taps exactly three things:
PLAY at logical (1088,43), step cell 2, and the CUTOFF drag. Only PLAY's rect
is pinned, and it does not move. `TEMPO_UP_X` is the only control that moves,
and nothing taps it. **`touch_script.txt` is not regenerated and must be
byte-identical in every commit of this work** (`git diff --stat` shows it
absent) — proved, additionally, by `PLAYING=1` still appearing in the gate.

## 4. Code shape (all in `examples/display/acid_box/acid_box.cpp`)

**Widget configuration**

- PLAY: accent `SYNTHUI_PANEL_BUTTON_ACCENT_GREEN`, default glyph scale
  (0.62 → ~30 px glyph at 100 × 48), **not** momentary.
- STOP / DOWN / UP: accent `…_PALE`, default glyph scale, momentary-lit — the
  idiom prev/next already use ("lit = being pressed"): `on` driven from
  PRESSED, cleared on RELEASED, PRESS_LOST and INDEV_RESET, because the
  widget's draw never reads `LV_STATE_PRESSED`.
- Tempo readout: default blue accent (matches `stepSeg`), ghost on,
  `LV_OBJ_FLAG_CLICKABLE` removed (a read-out, not a target).
- Wave toggle: `set_positions(2)`, left `GLYPH_SAW`, right `GLYPH_SQUARE`,
  `set_panel_color(0x6D7A85)`, value 0 at boot = `default_patch()`'s sawtooth.

**`mkpanelbtn()` is generalised**, not duplicated: from
`(scr, x, w, glyph, cb)` with `STEP_Y`/`STEP_H`, pale accent and 0.85 scale
baked in, to `(scr, x, y, w, h, glyph, accent, scale, cb, momentary)`. It stays
`UIBUILD_FN` (flash, `.progmem.acid_uibuild`). The four momentary handlers are
attached only when `momentary`, still in the one place, so they cannot be wired
to one button and forgotten on another. Prev/next pass 0.85 explicitly; its
comment is corrected while it moves — the widget normalises glyph size on the
**smaller** dimension (`min(vw, vh)`), not on width.

**`mkbtn()` is deleted** with its last caller (the third commit), and with it
this file's only use of `lv_button`.

**Statics**: `playBtnLabel`, `bpmLabel` → `playBtn`, `tempoSeg`;
`waveBtnLabel` is deleted with no replacement (`cbWave` reads its event target).

**Callbacks**: `cbPlay`, `cbStop`, `cbTempoUp`, `cbTempoDn` unchanged.
`cbWave` is attached to `LV_EVENT_VALUE_CHANGED` (the toggle cycles its own
value on CLICKED and then sends it) and reads
`synthui_slide_toggle_get_value()` — 0 = SAW, 1 = SQR; its `static bool square`
is deleted, so the widget holds the only UI copy and the voice keeps the truth.

**`ui_poll()`**, inside the existing change guards (which are load-bearing, not
an optimisation — see the poller's own comment):

```c
if ((int)play != shownPlaying) {
    shownPlaying = (int)play;
    synthui_panel_button_set_on(playBtn, play);
    CONSOLE.printf("PLAY_LIT=%d\n", synthui_panel_button_get_on(playBtn) ? 1 : 0);
}
...
if (bpm10 != shownBpm10) {
    shownBpm10 = bpm10;
    char b[16];
    snprintf(b, sizeof b, "%3d.%d", bpm10 / 10, bpm10 % 10);   /* INTEGERS: %f is banned here */
    synthui_seven_segment_set_text(tempoSeg, b);
}
```

`%3d` pads to a fixed five-character field: the leading space renders as an
all-ghost cell, so 20.0..999.0 (the transport's clamp) never changes the
content width. The prime-the-poller-before-arming-the-timer step stays; it is
what makes the boot golden cover "128.0" rather than an empty readout.

## 5. The gate

**5.1 Golden.** `ACIDBOX_UI_SUM` moves from `0xBB2AEE59` **three times**, once
per commit (§7). Each new value is accepted only when two consecutive QEMU runs
agree, and is recorded with a written reason in the commit and in
`transcript_qemu.txt`'s changelog — never pasted from a single run. The
all-zeros anti-golden check stays. The gate count stays **141**.

**5.2 The PLAY-lit witness.** The golden is the BOOT frame, taken before the
script taps PLAY, and `PLAYING=1` is printed by `cbPlay` from the transport. So
with PLAY's lit state now carrying meaning, a DELETED `set_on()` would leave
every existing assertion green. (Corrected while writing the plan: an INVERTED
one would not — it lights PLAY at boot, which changes the boot frame, so the
golden catches that mutant by a different route. The deleted call is the one
only a witness can see.) `PLAY_LIT=` is read back from the
**widget** (`get_on`), not from the transport, so it cannot agree with
`PLAYING=` by construction. Asserted by line number:

- the first `^PLAY_LIT=` line is `PLAY_LIT=0` (boot is dark; it prints once at
  boot because `shownPlaying` starts at −1);
- a `^PLAY_LIT=1$` exists **after** the first `^PLAYING=1$`; otherwise
  `FAIL: PLAY never lit after the tap`.

It proves the setter ran, not that pixels changed; rendering of the on-state is
`synthui_panel_button_test`'s golden's job.

**5.3 RED demos**, each checked for WHICH assertion caught it (NEW-50's
lesson — a demo caught by a pre-existing check proves nothing about the new
one):

1. `set_on()` deleted → the readback prints `PLAY_LIT=0` after play → "never
   lit" must fire.
2. `set_on(playBtn, !play)` → a live run fails `FAIL: UI golden` FIRST (PLAY
   lit at boot moves the boot frame, and the golden check precedes the gesture
   checks) — recorded as such, because a demo caught by a pre-existing check
   says nothing about the new one. The new checks are then exercised on that
   same capture with its sum line patched back to the golden: the first-line
   check must fire, an UNORDERED `grep -q '^PLAY_LIT=1$'` must exit 0 (it would
   accept the mutant — the boot line satisfies it) and the ordered check must
   exit 1. `run_qemu.sh` is not edited to stage any of this.

**5.4 Fixture and vacuity.** `transcript_qemu.txt` is BOTH a curated ~630-line
narrative AND the vacuity suite's fixture. It is **spliced** in every commit
(golden line, changelog entry; the `PLAY_LIT` lines into the raw-UART section
in commit 1) — never `cp`'d over. Prose quoting `PLAY_LIT=1` is indented so the
`^`-anchored assertion cannot match the narrative. Each splice is proved by
`green_still_passes_acid_box` replaying it. One new negative,
`acb_play_never_lit_fails_by_name`: strip `^PLAY_LIT=1` lines (`cmp` guards
that the mutation changed the file), expect non-zero exit AND
`^FAIL: PLAY never lit`. Vacuity **66 → 67**.

**5.5 Comments.** `run_qemu.sh`'s GEOMETRY header and `touch_script.txt`'s
header describe ▶ as a rect; both stay arithmetically true. The `run_qemu.sh`
header gains one line saying ▶ is now a panel button. `touch_script.txt` is not
edited at all (§3).

No new host suite: the change adds no pure logic.

## 6. ITCM — recorded, not trusted

Baseline, re-measured from the current ELFs on 2026-09-17
(`262144 − (ADDR(.ARM.exidx) + SIZEOF(.ARM.exidx))`, which equals the figure
`--print-memory-usage` prints):

| Build | Headroom |
|---|---|
| `build` (default) | 2,884 B |
| `build-loopstat` | 2,756 B |
| `build-bt` | 11,604 B |
| `build-bench` | 11,412 B |

The non-BT builds are the tight ones; all four are guarded by the 2,048 B
`ACIDBOX_ITCM_MIN_HEADROOM` linker ASSERT. libSynthUI is flash-routed in every
configuration (`synthui_slide_toggle` lands there under the same whole-archive
rule) and `mkpanelbtn`/`build_ui` are `UIBUILD_FN`, so the only ITCM-resident
code that changes is `ui_poll()` (+ a `set_on`, a `get_on`, a `printf`) and
`cbWave` (− a static and a label call).

**Pre-registered prediction: the default build's headroom moves by less than
300 B.** The default number is read after each commit; all four after the last,
and all are recorded in the transcript and CLAUDE.md. If the floor binds:
**stop and report.** The documented next lever (routing
`synthui_rotary_knob_gpu.cpp`, ~3,188 B, to flash) is per-frame on silicon
under a drag and must be benched, not assumed — NEW-45 measured 5.6× where a
micro-benchmark implied 1.5×.

## 7. Delivery

Three commits, one per control, gate green at each:

1. **Transport** — generalise `mkpanelbtn()`, PLAY + STOP, the poller's
   `set_on` + `PLAY_LIT` witness, the gate assertions, both RED demos, the
   vacuity negative, golden move 1, transcript splice.
2. **Tempo** — DOWN/UP + readout, constants, golden move 2, splice.
3. **Waveform** — slide toggle, `cbWave`, delete `mkbtn()`, golden move 3,
   splice, final four ITCM numbers.

The gpu golden is recorded ONCE, at the bench, against the final frame; the
intermediate frames never exist on silicon.

**Software close-out**, in order: rebuild acid_box's human-owned dirs IN PLACE
(`cmake --build` on `build`, `build-loopstat`, `build-bt`, `build-bench` —
never reconfigured from flags: `build-bench` carries the real BT blob); then
`tools/build-bench-configs.sh -n` (never concurrently with the sweep); vacuity
67/67, alone; sweep **141 / 0 / 0** from the real path (never a symlink — it
poisons the self-building gate dirs); `license-audit.sh` AFTER the sweep. Then
CLAUDE.md, the memory file and NEW-54.

## 8. Silicon acceptance (NEW-54 stays open until done)

Flash per the flashing skill — console detached before every LinkServer op.

Counters: gpu golden re-recorded over **≥ 3 SW4 boots, bit-identical**,
`0xEA5AB843` retired with the written reason "top bar and SAW/SQR moved to
SynthUI widgets (NEW-54)"; `ACIDBOX_ENGINE=gpu`, `ACIDBOX_GPU_ERR=0`,
`ACIDBOX_VSYNC … timeouts=0`, `ROT_EQ … fail=0`, `full<=2`, and `PLAY_LIT=1`
following `PLAYING=1` on silicon too.

By hand, because no checksum can see them:

- PLAY lights on play; goes dark on pause; goes dark on stop.
- STOP, DOWN, UP light while held and clear on release AND on a slide-off.
- Tempo stepped across 99 ↔ 100 and to both rails (20, 999): the readout box
  never jumps, the leading cell ghosts.
- The toggle knob slides on a tap anywhere in its box, and the timbre changes
  saw ↔ square by ear.
- No scanout flash while any of the above repaints.

Bench traps, carried forward: the reset witness is `ACIDBOX_ROT ops=` and
`ACIDBOX_VSYNC flips=` restarting at 1, NOT the boot banner (lost across the
CDC drop after SW4); and any run containing a BARE `wiggle=1` line is
discarded, not analysed — the title label toggles the 8-knob wiggle through a
24 px halo (the loopstat `wiggle=` slot is microseconds, where 1 means OFF).

Results go in a new NEW-54 section of `transcript_hw_evkb.txt`.

## 9. Risks

- **A tap that misses for a non-geometric-looking reason.** If PLAY's panel
  button did not receive CLICKED (e.g. a flag difference from `lv_button`),
  the gate fails as "PLAY tap never landed". The editor's prev/next already
  prove the widget delivers CLICKED; the gate proves it for PLAY.
- **Transcript damage.** Three splices of a narrative fixture. Mitigated by
  the green-replay check in each commit and by diff-reviewing that only the
  intended lines changed.
- **ITCM.** §6; bounded by the ASSERT, which turns a miss into a named failure.
- **Press feedback regressions are invisible to every gate** (recorded from
  NEW-51). Accepted; §8's by-hand list is the only instrument.
