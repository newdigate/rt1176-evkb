# acid_box top bar and SAW/SQR on SynthUI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace acid_box's transport (PLAY/STOP), tempo cluster (− / readout / +) and SAW/SQR button with `synthui_panel_button`, `synthui_seven_segment` and `synthui_slide_toggle`, with a gate-visible witness that PLAY lights while playing.

**Architecture:** Spec: `docs/superpowers/specs/2026-09-17-acid-box-synthui-top-bar-design.md` (Linear NEW-54). Everything is confined to `examples/display/acid_box/`: `acid_box.cpp`'s geometry block, the flash-resident scene construction (`build_ui`, `mkpanelbtn`), the ITCM-resident poller (`ui_poll`) and `cbWave`. SynthUI is not modified, so there is no pin bump. Three commits, one per control; each moves the software boot golden deliberately and keeps the gate green. The gpu golden and the by-hand checks are a separate bench task that needs a person at the board.

**Tech Stack:** LVGL 9.4 (vendored), SynthUI (sibling checkout at the pinned `567ad9b`, unmodified), Teensyduino-style core for i.MX RT1176, CMake + ARM GCC 10, qemu2 through `tools/qrun`, POSIX sh gates, Python 3 + PIL for the frame dump.

---

## Global Constraints

- `$EVKB` = `/Users/nicholasnewdigate/Development/rt1170/evkb`, `$AB` = `$EVKB/examples/display/acid_box`. `$S` = your session scratchpad directory (any private temp dir; create one with `S=$(mktemp -d)` if you have none). Run everything from the REAL path, never through a `/tmp` symlink to the repo (a symlinked run poisons self-building gate dirs).
- Branch `new-54-acid-box-synthui-top-bar` already exists and holds the spec. Commit there. **Do not push.** Commit trailer: `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Run the gate as `./run_qemu.sh`, **never `sh run_qemu.sh`**. **One QEMU at a time** — never run a gate, the sweep, the vacuity suite, the licence audit or `build-bench-configs.sh` concurrently with each other.
- **SynthUI is NOT modified.** If a widget defect appears, stop and report it.
- **Do not weaken a gate assertion to make a change pass.**
- **A golden is RECORDED, never pasted**: two consecutive gate runs must print the identical `ACIDBOX_UI_SUM`, AND the frame behind it must have been dumped and LOOKED AT (Procedure B) before the value goes into `run_qemu.sh`.
- **`touch_script.txt` must never appear in `git status`.** Its SHA-256 starts `d1bae9e7f117a6c1`. PLAY's rect (1040..1139 × 20..67) is the only gate-tapped control this plan touches, and it does not move.
- **`transcript_qemu.txt` is a curated ~630-line narrative AND the vacuity suite's fixture. Never `cp` a capture over it.** Use Procedure C.
- The tempo text is built from INTEGERS only. `%f` is banned in this file (see `cbCut`'s comment).
- If the default build's link fails with `NEW-45: ITCM headroom below 2048`, **stop and report** — do not re-route anything to flash.

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `$AB/acid_box.cpp` | geometry constants, `mkpanelbtn()`, `build_ui()`, `ui_poll()`, `cbWave`, deletion of `mkbtn()` | 1, 2, 3 |
| `$AB/run_qemu.sh` | the software golden (moved ×3), the `PLAY_LIT` assertions, the GEOMETRY header note | 1, 2, 3 |
| `$AB/transcript_qemu.txt` | fixture + narrative: re-record entries, the capture block, the eye-check record | 1, 2, 3 |
| `$EVKB/tools/gate-vacuity.test.sh` | two new negatives, `acb_play_never_lit_fails_by_name` and `acb_lit_at_boot_fails_by_name` | 1 |
| `$EVKB/examples/README.md`, `$EVKB/CLAUDE.md` | the example's description; the close-out measurement | 4 |
| `$AB/transcript_hw_evkb.txt` | the silicon record | 5 |

## Shared Procedures

Tasks refer to these by letter. They are complete here; nothing in a task is "similar to" anything.

### Procedure A — build the default image and read ITCM headroom

```bash
cd "$AB" && cmake --build build 2>&1 | tail -15
```

Expected: ends with a `Memory region  Used Size  Region Size  %age Used` table and no error. If instead the configure step fails with `COMPILERPATH is UNDEFINED` or `toolchain file not found`, the build dir is stale: `rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build`.

Headroom, measured the same way every time (equals what `--print-memory-usage` prints for ITCM, subtracted from 262144):

```bash
itcm_headroom() { /Applications/ARM_10/bin/arm-none-eabi-size -A "$1" | awk '/^\.ARM\.exidx/{print 262144-($3+$2)}'; }
itcm_headroom "$AB/build/acid_box.elf"
```

Baseline before this plan: `build` **2884**, `build-loopstat` **2756**, `build-bt` **11604**, `build-bench` **11412**. Pre-registered prediction: `build` moves by less than 300 B over the whole plan.

### Procedure B — dump the boot frame and LOOK at it

Write these two files once into `$S`:

`$S/notouch.txt` — a comment plus 40 `R` lines (with NO script the GT911 model replays a built-in gesture table that pokes the UI; a stable checksum of a poked frame is how a false "golden does not reproduce" panic starts):

```bash
{ echo "# no touches"; i=0; while [ $i -lt 40 ]; do echo R; i=$((i+1)); done; } > "$S/notouch.txt"
```

`$S/dumpfb.py`:

```python
#!/usr/bin/env python3
"""dumpfb.py <monitor-socket> <raw-out> <png-out> : dump acid_box's scanned buffer."""
import re, socket, sys, time
from PIL import Image
sock_path, raw_path, png_path = sys.argv[1:4]
s = socket.socket(socket.AF_UNIX); s.connect(sock_path); s.settimeout(3)
def cmd(c):
    s.sendall(c.encode() + b"\n"); time.sleep(1.0)
    try: return s.recv(65536).decode(errors="replace")
    except socket.timeout: return ""
cmd("")
m = re.search(r"4080820c:\s+0x([0-9a-fA-F]{8})", cmd("xp/1wx 0x4080820c"))
if not m: sys.exit("could not read the LCDIFv2 buffer pointer at 0x4080820c")
addr = int(m.group(1), 16)
cmd('pmemsave 0x%x 0x384000 "%s"' % (addr, raw_path)); time.sleep(2)
cmd("quit")
raw = open(raw_path, "rb").read()
assert len(raw) == 3686400, len(raw)
h = 0x811C9DC5
for b in raw: h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
print("scanned=0x%08x fnv=0x%08X" % (addr, h))
# X byte is 0 in the presented buffer: read BGRX, never BGRA (alpha 0 = a false blank frame).
Image.frombytes("RGB", (720, 1280), raw, "raw", "BGRX", 2880).transpose(Image.Transpose.ROTATE_90).save(png_path)
```

`$S/dumpfb.sh` (the monitor socket path MUST be short — macOS caps `sun_path` at 104 bytes and a scratchpad path blows it, so it lives in `/tmp` by name):

```bash
#!/bin/sh
# dumpfb.sh <S> <elf> : boots with no touches, waits for setup() to finish, dumps the frame.
S="$1"; ELF="$2"; SOCK=/tmp/ab54mon.sock
rm -f "$SOCK" "$S/fb.uart" "$S/fb.raw" "$S/fb.png"
"$HOME/Development/qemu2/build/qemu-system-arm" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on \
    -kernel "$ELF" -display none -serial "file:$S/fb.uart" \
    -global driver=imxrt.gt911,property=touch-script,value="$S/notouch.txt" \
    -monitor "unix:$SOCK,server,nowait" &
QP=$!
i=0; until grep -q ACIDBOX_DONE "$S/fb.uart" 2>/dev/null; do sleep 0.5; i=$((i+1)); [ $i -gt 120 ] && break; done
sleep 1
python3 "$S/dumpfb.py" "$SOCK" "$S/fb.raw" "$S/fb.png"
kill $QP 2>/dev/null; wait $QP 2>/dev/null
tr -d '\r' < "$S/fb.uart" | grep "^ACIDBOX_UI_SUM="
```

Run: `chmod +x "$S/dumpfb.sh" && "$S/dumpfb.sh" "$S" "$AB/build/acid_box.elf"`

Expected: two lines, `scanned=0x8……… fnv=0xXXXXXXXX` and `ACIDBOX_UI_SUM=0xXXXXXXXX`, **with the same eight hex digits** — that equality is what makes the PNG you are about to inspect THIS golden's frame. Control (run once): the same FNV over 3686400 zero bytes is `0x9BC99DC5`, the gate's anti-golden. Then open `$S/fb.png` with the Read tool (it is the upright 1280×720 UI) and check the task's eye-check list. Zoom before believing something is misplaced.

### Procedure C — splice `transcript_qemu.txt` (never `cp`)

The capture block is the lines after `==== captured UART ====` up to (not including) the first blank line. This replaces exactly that block with the gate's fresh capture, CR-stripped, dropping a final line the reap cut mid-token:

```bash
python3 - "$AB/transcript_qemu.txt" "$AB/build/acid_box.uart" <<'EOF'
import sys
tpath, cpath = sys.argv[1:3]
t = open(tpath, encoding="utf-8").read().split("\n")
raw = open(cpath, "rb").read().replace(b"\r", b"").decode("utf-8", "replace")
cap = raw.split("\n")
if cap and cap[-1] != "": cap = cap[:-1]        # unterminated final line: not evidence
while cap and cap[-1] == "": cap.pop()
i = t.index("==== captured UART ====") + 1
j = i
while t[j] != "": j += 1
assert t[i] == "=== BOOT ===" and cap[0] == "=== BOOT ===", (t[i], cap[:1])
print("replacing transcript lines %d..%d (%d lines) with %d capture lines" % (i + 1, j, j - i, len(cap)))
open(tpath, "w", encoding="utf-8").write("\n".join(t[:i] + cap + t[j:]))
EOF
git -C "$EVKB" diff --stat -- examples/display/acid_box/transcript_qemu.txt
```

Then the narrative, by hand with the Edit tool (each task says exactly what):
1. the `THE PASSING RUN (…re-captured …)` heading's date/reason;
2. a new `RE-RECORDED 2026-09-17 (NEW-54 …)` paragraph, inserted immediately after the last existing `RE-RECORDED` paragraph (they are in date order, above the first `====` rule);
3. `THE BOOT GOLDEN` section: the golden value, the date, the eye-check sentence, and the previous value appended to `History:`;
4. every OTHER place the old golden or a changed number is quoted: `grep -n "<old golden>" "$AB/transcript_qemu.txt"` must return only `History:`-style mentions afterwards.

Any prose that quotes `PLAY_LIT=1` or `PLAY_LIT=0` **must be indented** (at least two spaces): the gate's assertions are `^`-anchored and the gate reads this whole file as its capture under the vacuity suite.

Review the diff: `git -C "$EVKB" diff -- examples/display/acid_box/transcript_qemu.txt` — lines outside the capture block must be ONLY the edits you made on purpose.

### Procedure D — replay the fixture through the gate (what the vacuity suite does, for one gate)

```bash
cat > "$S/fake-qemu" <<'FAKE'
#!/bin/sh
target=""; prev=""
for a in "$@"; do
    case "$a" in file:*) [ "$prev" = "-serial" ] && target="${a#file:}" ;; esac
    prev="$a"
done
if [ -n "${FAKE_CAPTURE:-}" ]; then
    [ -n "$target" ] && cat "$FAKE_CAPTURE" > "$target"
    sleep 300
fi
exit 0
FAKE
chmod +x "$S/fake-qemu"
replay() { ( cd "$AB" && REAL_QEMU="$S/fake-qemu" FAKE_CAPTURE="$1" GATE_TIMEOUT=120 QRUN_TIMEOUT=40 ./run_qemu.sh 2>&1 | tail -3; ); }
replay "$AB/transcript_qemu.txt"
```

Expected: last line starts `PASS: acid box`. (A replay overwrites `build/acid_box.uart` with the fixture; re-run the real gate if you need a fresh capture afterwards.)

---

### Task 1: Transport — PLAY/STOP panel buttons and the `PLAY_LIT` witness

**Files:**
- Modify: `$AB/acid_box.cpp` (constants comment ~1120; statics ~1165; `ui_poll` ~1360; `mkpanelbtn` ~1395-1417; `build_ui` ~1473-1478 and ~1568/1576)
- Modify: `$AB/run_qemu.sh` (GEOMETRY header ~40; golden ~288; gestures block ~296)
- Modify: `$EVKB/tools/gate-vacuity.test.sh` (acid_box section, before its `acb_badsum` case ~885)
- Modify: `$AB/transcript_qemu.txt` (Procedure C)

- [ ] **Step 1: Baseline.** Confirm the branch, the untouched touch script, a green gate, and the headroom.

```bash
cd "$EVKB" && git branch --show-current && shasum -a 256 examples/display/acid_box/touch_script.txt | cut -c1-16
```
Expected: `new-54-acid-box-synthui-top-bar` and `d1bae9e7f117a6c1`.

Run Procedure A, then `cd "$AB" && ./run_qemu.sh 2>&1 | tail -2`. Expected: `PASS: acid box …`; headroom `2884`.

- [ ] **Step 2: Write the failing gate assertion (RED first).** In `$AB/run_qemu.sh`, directly after the line

```sh
grep -qE "^PLAYING=1$" "$OUT" || { echo "FAIL: PLAY tap never landed"; exit 1; }
```

insert:

```sh
# NEW-54: PLAY's lit state now MEANS "playing", and no golden can see it -- the
# boot golden is taken before this script taps PLAY, and PLAYING= is printed by
# cbPlay from the TRANSPORT.  PLAY_LIT= is read back from the WIDGET
# (synthui_panel_button_get_on) where the poller sets it, so it cannot agree
# with PLAYING= by construction.  BY LINE NUMBER: a PLAY_LIT=1 printed BEFORE the
# tap (an inverted set_on() does exactly that, at boot) must not satisfy it, and
# an unordered grep would accept it.  (A live run of that mutant is ALSO caught
# by the boot golden, since a lit PLAY changes the boot frame; the deleted
# set_on() is the mutant ONLY these two checks can see.)
# DEMONSTRATED RED twice (set_on deleted; set_on inverted) -- transcript_qemu.txt.
# ★ WHAT IT DOES NOT PROVE: this reads the widget's STORED FLAG, not that a lit
# PLAY renders any differently.  The inverted mutant showed the rendering does
# change (its boot sum was 0xFDD7C2DA against this golden's 0xE7711DD4), but that
# was a one-off manual measurement -- nothing here checks it, and a second
# checksum taken mid-animation would not reproduce.
LIT_FIRST=$(grep -E "^PLAY_LIT=[01]" "$OUT" | head -1 | tr -d '\r')
[ "$LIT_FIRST" = "PLAY_LIT=0" ] \
    || { echo "FAIL: PLAY not dark at boot (first PLAY_LIT line: '${LIT_FIRST:-none}')"; exit 1; }
# PLAY_LN is hoisted from the gesture-order block below and GUARDED, because an
# empty p would make awk's `NR > p` a STRING comparison that is true for every
# line -- silently turning the ordered check into the unordered grep the comment
# above says must not be accepted.  A vacuous pass, not a failure.
# ★ STATUS OF THIS GUARD, so it is not read as inheriting the DEMONSTRATED RED
# above: it is UNREACHABLE today and was NOT shown to fail -- the
# `grep -qE "^PLAYING=1$"` at the top of this block exits first with "PLAY tap
# never landed", so nothing can reach here with PLAY_LN empty.  It is kept
# because PLAY_LN has THREE consumers, and an empty one is vacuous in BOTH
# directions: `NR > p` (the lit check above) fires on EVERY line, while
# `NR < p` (the boot-silence check below, "audio before ▶") fires on NO line --
# measured, not reasoned, on a two-line fixture that really does carry a bar
# before the tap.  The second of those was latent BEFORE NEW-54; the hoist
# closes it too, which is the argument for a guard nothing can currently trip.
PLAY_LN=$(grep -n "^PLAYING=1$" "$OUT" | head -1 | cut -d: -f1)
[ -n "$PLAY_LN" ] || { echo "FAIL: no PLAYING=1 line to order against"; exit 1; }
awk -v p="$PLAY_LN" 'NR>p && /^PLAY_LIT=1\r?$/ { ok=1 } END { exit ok ? 0 : 1 }' "$OUT" \
    || { echo "FAIL: PLAY never lit after the tap"; exit 1; }
```

Then, in the gesture-order block nine lines below, replace the now-duplicate
`PLAY_LN=$(grep -n "^PLAYING=1$" …)` assignment with a comment saying it is set
and checked above — one assignment, one guard, one name. (Measured: with an
empty `p`, `awk 'NR>p'` compares strings and accepts EVERY line, exit 0.)

(acid_box prints these with `printf("…\n")`, which emits a bare `\n` — the existing `^PLAYING=1$` assertion directly above relies on the same fact — but the new checks tolerate a `\r` anyway, because `println` elsewhere in this file does emit one and the next edit should not have to know which was used.)

- [ ] **Step 3: Run the gate against the UNCHANGED firmware; verify it fails for the right reason.**

Run: `cd "$AB" && ./run_qemu.sh 2>&1 | tail -1`
Expected: `FAIL: PLAY not dark at boot (first PLAY_LIT line: 'none')`

- [ ] **Step 4: Generalise `mkpanelbtn()`.** In `$AB/acid_box.cpp` replace the whole existing comment + function (from `/* Shared prev/next construction:` through its closing `}`) with:

```c
/* Shared panel-button construction: size/pos/glyph/accent/glyph-scale plus the
 * CLICKED action and -- for a PANEL_MOMENTARY button -- the press/release
 * handlers, all in one place so the momentary wiring cannot be attached to one
 * button and forgotten on another.  The two modes are EXCLUSIVE, not merely
 * different: cbPanelDown/cbPanelUp OWN `on`, so a stateful button built
 * PANEL_MOMENTARY would have its state clobbered on every press and release.
 * PLAY is the one PANEL_STATEFUL button today -- its `on` is STATE, owned by
 * ui_poll, not press feedback.
 *
 * The scales: the widget sizes its glyph from the SMALLER of its two dimensions
 * (min(vw, vh) in synthui_panel_button_compute_geom), not from its width.  At
 * the step row's 44x56 the default 0.62 lands the chevron around 27 px and it
 * reads as a faint mark, so prev/next pass PANEL_GLYPH_SCALE_STEP; the 100x48
 * transport buttons get a ~30 px glyph from the default. */
enum PanelBtnPress { PANEL_STATEFUL, PANEL_MOMENTARY };
/* SynthUI exports no macro for its own default, so this COMMENT is the only
 * link between the two -- if synthui_panel_button's constructor default ever
 * moves, nothing here will fail to compile. */
static constexpr float PANEL_GLYPH_SCALE_DEFAULT = 0.62f;   /* = the widget constructor's own default */
static constexpr float PANEL_GLYPH_SCALE_STEP    = 0.85f;   /* prev/next at 44x56; see above */
UIBUILD_FN static lv_obj_t *mkpanelbtn(lv_obj_t *scr, int x, int y, int w, int h,
                                       synthui_panel_button_glyph_t glyph,
                                       uint32_t accent, float scale,
                                       lv_event_cb_t cb, PanelBtnPress press)
{
    lv_obj_t *b = synthui_panel_button_create(scr);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    synthui_panel_button_set_glyph(b, glyph);
    synthui_panel_button_set_accent(b, accent);
    synthui_panel_button_set_glyph_scale(b, scale);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (press == PANEL_MOMENTARY) {
        lv_obj_add_event_cb(b, cbPanelDown, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(b, cbPanelUp, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(b, cbPanelUp, LV_EVENT_PRESS_LOST, NULL);
        lv_obj_add_event_cb(b, cbPanelUp, LV_EVENT_INDEV_RESET, NULL);
    }
    return b;
}
```

Update the two existing call sites in `build_ui()`:

```c
    mkpanelbtn(scr, PREV_X, STEP_Y, PREV_W, STEP_H, SYNTHUI_PANEL_BUTTON_GLYPH_REWIND,
               SYNTHUI_PANEL_BUTTON_ACCENT_PALE, PANEL_GLYPH_SCALE_STEP, cbPrevStep,
               PANEL_MOMENTARY);
```
```c
    mkpanelbtn(scr, NEXT_X, STEP_Y, NEXT_W, STEP_H, SYNTHUI_PANEL_BUTTON_GLYPH_FORWARD,
               SYNTHUI_PANEL_BUTTON_ACCENT_PALE, PANEL_GLYPH_SCALE_STEP, cbNextStep,
               PANEL_MOMENTARY);
```

- [ ] **Step 5: PLAY and STOP.** In the statics, change `static lv_obj_t *playBtnLabel, *bpmLabel, *noteLabel, *waveBtnLabel;` to:

```c
static lv_obj_t *playBtn, *bpmLabel, *noteLabel, *waveBtnLabel;
```

In `build_ui()` replace the six lines creating `play` and `stop` with:

```c
    /* PLAY's `on` is STATE -- lit while the transport runs, set by ui_poll --
     * so it is the one PANEL_STATEFUL button.  The widget has no
     * PAUSE glyph (and the DC reference has none), which is why today's
     * play/pause label swap became a lit/dark PLAY: a tap while playing still
     * pauses (cbPlay), and paused and stopped both read as PLAY dark. */
    playBtn = mkpanelbtn(scr, PLAY_X, BAR_Y, TRANSPORT_BTN_W, BAR_BTN_H,
                         SYNTHUI_PANEL_BUTTON_GLYPH_PLAY, SYNTHUI_PANEL_BUTTON_ACCENT_GREEN,
                         PANEL_GLYPH_SCALE_DEFAULT, cbPlay, PANEL_STATEFUL);
    mkpanelbtn(scr, STOP_X, BAR_Y, TRANSPORT_BTN_W, BAR_BTN_H,
               SYNTHUI_PANEL_BUTTON_GLYPH_STOP, SYNTHUI_PANEL_BUTTON_ACCENT_PALE,
               PANEL_GLYPH_SCALE_DEFAULT, cbStop, PANEL_MOMENTARY);
```

`mkpanelbtn` is defined AFTER `mkbtn` and before `mkknob`, i.e. before `build_ui` — no forward declaration needed.

- [ ] **Step 6: The poller.** In `ui_poll()` replace

```c
        lv_label_set_text(playBtnLabel, play ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
```
with
```c
        synthui_panel_button_set_on(playBtn, play);
        /* The gate's only view of the lit state (run_qemu.sh, NEW-54): read
         * BACK from the widget, so this cannot agree with cbPlay's PLAYING=
         * line by construction.  Prints once at boot (shownPlaying starts at
         * -1) and once per transition -- inside the change guard, never per tick. */
        CONSOLE.printf("PLAY_LIT=%d\n", synthui_panel_button_get_on(playBtn) ? 1 : 0);
```

Update the poller's header comment `/* 33 ms poller: cursor ring, play label, bpm readout (spec §3.3).` to `/* 33 ms poller: cursor ring, PLAY's lit state, bpm readout (spec §3.3).`, and the constants comment on the `PLAY_X` line to:

```c
static constexpr int PLAY_X = 1040, STOP_X = 1156, TRANSPORT_BTN_W = 100;   /* PLAY 1040..1139 x 20..67 -- PINNED: the gate's tap lands at (1088,43) */
```

- [ ] **Step 7: Build; read the headroom.** Procedure A. Expected: clean link; record `itcm_headroom build/acid_box.elf` (expect within ~100 B of 2884).

- [ ] **Step 8: Run the gate; it must now fail ONLY on the golden.**

Run: `cd "$AB" && ./run_qemu.sh 2>&1 | tail -1`
Expected: `FAIL: UI golden`. Then confirm the witness is present and ordered:
`tr -d '\r' < build/acid_box.uart | grep -n "^PLAY_LIT=\|^PLAYING=\|^ACIDBOX_UI_SUM="`
Expected order: `PLAY_LIT=0`, `ACIDBOX_UI_SUM=0x<NEW1>`, `PLAYING=0`, `PLAYING=1`, `PLAY_LIT=1`. If the gate instead fails `PLAY tap never landed`, the panel button is not receiving CLICKED at (1088,43): stop and report.

- [ ] **Step 9: Second run — the value must repeat.** Run the gate again; `ACIDBOX_UI_SUM` must equal `<NEW1>` exactly. If it differs, stop: the frame is not deterministic and nothing may be pinned.

- [ ] **Step 10: Dump the frame and LOOK at it.** Procedure B. `fnv` must equal `<NEW1>`. Eye-check on `$S/fb.png`: top-right, two periwinkle-blue panel buttons with a bright sheen band along the top — a DARK play triangle (boot is stopped: not green, no glow) in 1040..1139 and a dark square in 1156..1255, both 48 px tall on the bar; "ACID BOX" top-left; `-`, "128.0", `+` STILL the old LVGL controls; the lane, editor column (still a "SAW" LVGL button) and the eight knobs unchanged; nothing mirrored.

- [ ] **Step 11: Pin golden 1.** In `$AB/run_qemu.sh` change `0xBB2AEE59` to `<NEW1>` in the `grep -qE "ACIDBOX_UI_SUM=…` line, and append to the comment block directly above it:

```sh
# MOVED 2026-09-17 (NEW-54, 1 of 3): 0xBB2AEE59 -> <NEW1>.  PLAY and STOP became
# synthui_panel_button (same rects; PLAY dark at boot).  Two runs bit-identical;
# the frame was dumped with a no-touch script, its FNV-1a matched the printed
# sum, and it was looked at before this value was written here.
```

In the GEOMETRY header, after the `-> P 94 85 -> raw (676,1088) -> logical (1088,43)    ✔ inside` line, add:

```sh
#                (since NEW-54 ▶ is a synthui_panel_button; the rect is unchanged)
```

Run the gate. Expected: `PASS: acid box …`.

- [ ] **Step 12: RED demo 1 — `set_on()` deleted.** Comment out the `synthui_panel_button_set_on(playBtn, play);` line, build, run the gate.
Expected: `FAIL: PLAY never lit after the tap` (the readback prints `PLAY_LIT=0` both times). Record the exact FAIL line. Restore the line.

- [ ] **Step 13: RED demo 2 — `set_on()` inverted.** Change it to `synthui_panel_button_set_on(playBtn, !play);`, build, run the gate.
Expected: **`FAIL: UI golden`** — a PLAY lit at boot changes the boot frame, and the golden check runs before the gesture checks. That is a legitimate catch and worth recording, but it is the GOLDEN's, not the new assertions'. Exercise those on the same capture without touching `run_qemu.sh`:

```bash
tr -d '\r' < "$AB/build/acid_box.uart" | sed 's/^ACIDBOX_UI_SUM=0x.*/ACIDBOX_UI_SUM=<NEW1>/' > "$S/inverted.txt"
replay "$S/inverted.txt"                                   # Procedure D's helper
grep -q '^PLAY_LIT=1$' "$S/inverted.txt"; echo "unordered grep exit=$?"
P=$(grep -n '^PLAYING=1$' "$S/inverted.txt" | head -1 | cut -d: -f1)
awk -v p="$P" 'NR>p && /^PLAY_LIT=1$/ {ok=1} END{exit ok?0:1}' "$S/inverted.txt"; echo "ordered check exit=$?"
```
Expected: replay ends `FAIL: PLAY not dark at boot (first PLAY_LIT line: 'PLAY_LIT=1')`; `unordered grep exit=0` (it would ACCEPT the mutant); `ordered check exit=1`. Record all four results. Restore `set_on(playBtn, play)`, rebuild, run the real gate: `PASS`. `git diff --stat` must show `run_qemu.sh` changed only by Steps 2 and 11.

- [ ] **Step 14: Splice the transcript.** With a fresh PASSING capture in `build/acid_box.uart` (from Step 13's final run), run Procedure C. Narrative edits:
  - heading → `THE PASSING RUN  (./run_qemu.sh, idle host — re-captured 2026-09-17, NEW-54 transport)`;
  - new paragraph after the last `RE-RECORDED` one:

```
RE-RECORDED 2026-09-17 (NEW-54, 1 of 3 -- transport; spec
docs/superpowers/specs/2026-09-17-acid-box-synthui-top-bar-design.md): PLAY and
STOP became synthui_panel_button on their old rects, so touch_script.txt is
UNTOUCHED and the tap still lands at (1088,43).  The widget has no PAUSE glyph,
so PLAY's `on` now carries "playing": lit green while the transport runs, dark
when paused or stopped.  The boot golden moved 0xBB2AEE59 -> <NEW1>.  One new
token, printed by the poller from the WIDGET's own getter:
    PLAY_LIT=0   at boot, before the sum line
    PLAY_LIT=1   after PLAYING=1
The gate asserts the first is 0 and that a 1 follows PLAYING=1 BY LINE NUMBER.
DEMONSTRATED RED twice.  set_on() deleted -> "<exact FAIL line from Step 12>"
  -- the mutant NOTHING else in this gate can see (golden, gestures, audio all
  green).  set_on(!play) -> a live run fails "FAIL: UI golden" first (a PLAY lit
  at boot changes the boot frame); with the sum line patched back so the new
  checks are reached, "<exact FAIL line from Step 13>", and on that capture an
  unordered grep for the lit line exits 0 while the ordered check exits 1.
ITCM headroom, default build: 2884 -> <Step 7 number> B.
```

  - `THE BOOT GOLDEN`: replace the opening sentence's value/date/plan reference with `<NEW1>`, `2026-09-17`, `NEW-54 transport`; in the eye-check sentence change `PLAY and STOP top-right, all unchanged` to `PLAY and STOP top-right as panel buttons, PLAY's triangle DARK (stopped)`; prepend `0xBB2AEE59 (2026-09-16, the SynthUI editor rework; checked as listed above with stock LVGL PLAY/STOP), ` to the `History:` list; add `, and 2026-09-17 for <NEW1>` to the `VERIFIED (…)` line.
  - `grep -n "0xBB2AEE59" transcript_qemu.txt` — every remaining hit must be a historical mention (a dated `RE-RECORDED` paragraph or `History:`).

- [ ] **Step 15: Replay the fixture.** Procedure D. Expected: `PASS: acid box …`.

- [ ] **Step 16: The vacuity negatives (TWO).** In `$EVKB/tools/gate-vacuity.test.sh`, inside the acid_box section, directly before the `sed 's|^ACIDBOX_UI_SUM=0x........|…` case, insert BOTH cases below. One pins each half of the witness: the ordered check, and the boot-dark check. The boot-dark half needs its own case because a LIVE inverted-`set_on()` run is caught by the boot golden first, so nothing else ever shows that assertion firing.

```sh
    # NEW-54: PLAY's lit state is invisible to every golden (the boot frame is
    # taken before the tap), so the PLAY_LIT witness is the only thing between
    # a deleted set_on() and a green gate.  A capture with no lit line after
    # the tap must fail by name.  cmp guards the mutation.
    grep -v "^PLAY_LIT=1$" "$EVKB/$ACB/transcript_qemu.txt" > "$WORK/acb_neverlit.txt"
    run_gate "$ACB" "run_qemu.sh" "$WORK/acb_neverlit.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    cmp -s "$EVKB/$ACB/transcript_qemu.txt" "$WORK/acb_neverlit.txt" && result=1
    echo "$OUT_TEXT" | grep -q "^FAIL: PLAY never lit after the tap" || result=1
    report "acb_play_never_lit_fails_by_name" $result

    # ...and the OTHER half of the same witness: a PLAY already lit at boot is
    # what an INVERTED set_on() produces.  A live run of that mutant is caught
    # by the boot golden first (the frame really does change), so this ordered
    # pair is the only place the boot-dark check itself is shown to fire.
    sed 's|^PLAY_LIT=0$|PLAY_LIT=1|' \
        "$EVKB/$ACB/transcript_qemu.txt" > "$WORK/acb_litatboot.txt"
    run_gate "$ACB" "run_qemu.sh" "$WORK/acb_litatboot.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    cmp -s "$EVKB/$ACB/transcript_qemu.txt" "$WORK/acb_litatboot.txt" && result=1
    # The PARENTHETICAL is part of the assertion, not decoration: the bare
    # message also fires for a capture with NO PLAY_LIT line at all ('none'),
    # and cmp proves only THAT the fixture changed, not HOW.  Without it, a
    # future edit turning this sed into a deletion would leave the case green
    # while testing a different scenario -- and this is the only automated
    # place the boot-dark assertion fires.
    echo "$OUT_TEXT" \
        | grep -q "^FAIL: PLAY not dark at boot (first PLAY_LIT line: 'PLAY_LIT=1')" || result=1
    report "acb_lit_at_boot_fails_by_name" $result
```

Verify BOTH in isolation with Procedure D's `replay` (the indentation rule above is
what keeps these `sed`/`grep -v` mutations confined to the capture block):
```bash
grep -v "^PLAY_LIT=1$" "$AB/transcript_qemu.txt" > "$S/neverlit.txt" && replay "$S/neverlit.txt"
sed 's|^PLAY_LIT=0$|PLAY_LIT=1|' "$AB/transcript_qemu.txt" > "$S/litatboot.txt" && replay "$S/litatboot.txt"
```
Expected: `FAIL: PLAY never lit after the tap`, then `FAIL: PLAY not dark at boot (first PLAY_LIT line: 'PLAY_LIT=1')`. The suite total goes 66 -> **68**, not 67. (The full suite runs in Task 4.)

- [ ] **Step 17: Commit.**

```bash
cd "$EVKB" && git status --short   # expect exactly: acid_box.cpp, run_qemu.sh, transcript_qemu.txt, tools/gate-vacuity.test.sh
git add examples/display/acid_box/acid_box.cpp examples/display/acid_box/run_qemu.sh \
        examples/display/acid_box/transcript_qemu.txt tools/gate-vacuity.test.sh
git commit -m "acid_box: PLAY/STOP on synthui_panel_button -- PLAY lit while playing, a PLAY_LIT widget-readback witness asserted by line number and shown RED twice, golden 0xBB2AEE59 -> <NEW1> (NEW-54, 1 of 3)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: Tempo — DOWN / seven-segment readout / UP

**Files:**
- Modify: `$AB/acid_box.cpp` (constants ~1118-1119; statics; `ui_poll` bpm branch ~1366-1371; `build_ui` ~1464-1472)
- Modify: `$AB/run_qemu.sh` (golden), `$AB/transcript_qemu.txt` (Procedure C)

- [ ] **Step 1: Constants.** Replace

```c
static constexpr int TEMPO_DN_X = 560, TEMPO_UP_X = 690, TEMPO_BTN_W = 50;
static constexpr int BPM_X = 624,   BPM_Y = 35;
```
with
```c
/* The readout scales on HEIGHT (u = h/112) and its width follows the text: five
 * cells "128.0" are (4*76 + 44 + 112*tan 6deg) * 48/112 = 154.19 px and the
 * widget ceils the last cell's edge to 155, hence a 156 px box and UP at 780.
 * None of the three is tapped by the gate. */
static constexpr int TEMPO_DN_X = 560, TEMPO_UP_X = 780, TEMPO_BTN_W = 50;
static constexpr int TEMPO_SEG_X = 618, TEMPO_SEG_W = 156;   /* 618..773 x 20..67 */
```

- [ ] **Step 2: Statics.** `static lv_obj_t *playBtn, *bpmLabel, *noteLabel, *waveBtnLabel;` → `static lv_obj_t *playBtn, *tempoSeg, *noteLabel, *waveBtnLabel;`

- [ ] **Step 3: `build_ui()`.** Replace the nine lines from `lv_obj_t *dn = mkbtn(scr, "-", cbTempoDn, NULL);` through `lv_obj_set_size(up, TEMPO_BTN_W, BAR_BTN_H);` with:

```c
    mkpanelbtn(scr, TEMPO_DN_X, BAR_Y, TEMPO_BTN_W, BAR_BTN_H,
               SYNTHUI_PANEL_BUTTON_GLYPH_DOWN, SYNTHUI_PANEL_BUTTON_ACCENT_PALE,
               PANEL_GLYPH_SCALE_DEFAULT, cbTempoDn, PANEL_MOMENTARY);
    tempoSeg = synthui_seven_segment_create(scr);
    lv_obj_set_size(tempoSeg, TEMPO_SEG_W, BAR_BTN_H);
    lv_obj_set_pos(tempoSeg, TEMPO_SEG_X, BAR_Y);
    lv_obj_remove_flag(tempoSeg, LV_OBJ_FLAG_CLICKABLE);  /* read-out; see accLamp below */
    mkpanelbtn(scr, TEMPO_UP_X, BAR_Y, TEMPO_BTN_W, BAR_BTN_H,
               SYNTHUI_PANEL_BUTTON_GLYPH_UP, SYNTHUI_PANEL_BUTTON_ACCENT_PALE,
               PANEL_GLYPH_SCALE_DEFAULT, cbTempoUp, PANEL_MOMENTARY);
```

(No `set_text` here: `ui_poll(NULL)` at the end of `build_ui()` paints "128.0" before the first frame — that is what the RUN THE POLLER ONCE comment guarantees. In that comment, change `or the lv_label default "Text"` to `or an empty seven-segment readout`.)

- [ ] **Step 4: The poller's bpm branch.** Replace

```c
        char b[16];
        snprintf(b, sizeof b, "%d.%d", bpm10 / 10, bpm10 % 10);
        lv_label_set_text(bpmLabel, b);
```
with
```c
        /* %3d: a FIXED five-cell field.  The widget's width follows its text,
         * so "99.0" would be one cell narrower than "100.0" and the digits
         * would jump; the leading space renders as an all-ghost cell instead.
         * Same-length text also keeps set_text on its per-cell damage path. */
        char b[16];
        snprintf(b, sizeof b, "%3d.%d", bpm10 / 10, bpm10 % 10);
        synthui_seven_segment_set_text(tempoSeg, b);
```

Also update the poller's guard comment: `lv_label_set_text() reallocates and invalidates unconditionally, so an unguarded poller would dirty two labels 30 times a second forever` → `An unguarded poller would re-send the same widget state 30 times a second forever; the SynthUI setters early-out on no change, but the PLAY_LIT print would not, and the guard is what keeps that a per-transition line`.

- [ ] **Step 5: Build, headroom.** Procedure A; record the number.

- [ ] **Step 6: Gate run 1.** Expected `FAIL: UI golden`; note `ACIDBOX_UI_SUM=0x<NEW2>`. Everything else (PLAY_LIT, gestures, RMS) must already pass — confirm by temporarily reading the capture: `tr -d '\r' < build/acid_box.uart | grep -c "^PLAY_LIT="` → `2`.

- [ ] **Step 7: Gate run 2.** `<NEW2>` must repeat exactly.

- [ ] **Step 8: Dump and LOOK.** Procedure B; `fnv == <NEW2>`. Eye-check: a DOWN triangle button at 560..609, then a dark-navy (`#181830`) readout 618..773 showing slanted pale-blue "128.0" with faint ghost segments and a round decimal point, its top and bottom edges level with the buttons; an UP triangle button at 780..829; ~8 px of ground between each pair and no ink right of x=773 inside the old 690..739 slot; PLAY/STOP as Task 1; everything else unchanged. Measure, don't guess: 
```bash
python3 - "$S/fb.png" <<'EOF'
import sys; from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); g = (0x10,0x18,0x20)
cols = [x for x in range(540, 860) if any(im.getpixel((x, y)) != g for y in range(20, 68))]
runs, s = [], cols[0]
for a, b in zip(cols, cols[1:] + [None]):
    if b != a + 1: runs.append((s, a)); s = b
print(runs)
EOF
```
Expected: three runs — `(560, 609)`, `(618, 772 or 773)`, `(780, 829)`.

- [ ] **Step 9: Pin golden 2.** `<NEW1>` → `<NEW2>` in `run_qemu.sh`; append to the comment block:
```sh
# MOVED 2026-09-17 (NEW-54, 2 of 3): <NEW1> -> <NEW2>.  Tempo -/+ became DOWN/UP
# panel buttons and the bpm label a full-height synthui_seven_segment (618..773);
# UP moved 690 -> 780.  Two runs bit-identical, frame dumped and looked at, ink
# extents measured: <the three runs from Step 8>.
```
Run the gate: `PASS`.

- [ ] **Step 10: Splice.** Procedure C with the passing capture. Narrative: heading reason → `NEW-54 tempo`; new paragraph:
```
RE-RECORDED 2026-09-17 (NEW-54, 2 of 3 -- tempo): -/+ became DOWN/UP
synthui_panel_button and the bpm lv_label a synthui_seven_segment at the bar's
full 48 px height.  That widget scales on height and its width follows the
text, so the cluster widened: readout 618..773, UP 690 -> 780 (measured ink:
<runs>).  The text is a fixed five-cell field ("%3d.%d", integers only), so
99.0 <-> 100.0 cannot resize it.  No gesture touches any of them and
touch_script.txt is untouched.  Golden <NEW1> -> <NEW2>; every behavioural
token and the third-column step-2 values are unchanged.
ITCM headroom, default build: <Task 1 number> -> <Step 5 number> B.
```
`THE BOOT GOLDEN`: value → `<NEW2>`; eye-check `-, "128.0", + centred` → `DOWN, a seven-segment "128.0", UP centred (560..609, 618..773, 780..829)`; prepend `<NEW1> (2026-09-17, NEW-54 transport), ` to `History:`. Check the step-2 column claim is TRUE before writing it: bars 1..6 third value in the new capture vs `0.0001, 0.0001, 0.0003, 0.3928, 0.3925, 0.3854`; if any differs, write what you measured instead and do not claim bit-identity.

- [ ] **Step 11: Replay.** Procedure D → `PASS: acid box …`.

- [ ] **Step 12: Commit.**
```bash
cd "$EVKB" && git status --short   # expect: acid_box.cpp, run_qemu.sh, transcript_qemu.txt
git add examples/display/acid_box/acid_box.cpp examples/display/acid_box/run_qemu.sh examples/display/acid_box/transcript_qemu.txt
git commit -m "acid_box: tempo on SynthUI -- DOWN/UP panel buttons and a full-height seven-segment readout in a fixed five-cell field, UP 690 -> 780, golden <NEW1> -> <NEW2> (NEW-54, 2 of 3)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: Waveform — slide toggle; delete `mkbtn()`

**Files:**
- Modify: `$AB/acid_box.cpp` (includes ~56; statics; `cbWave` ~1325-1334; `mkbtn` ~1383-1394 deleted; `build_ui` ~1563-1565; the `UIBUILD_FN` comment)
- Modify: `$AB/run_qemu.sh`, `$AB/transcript_qemu.txt`

- [ ] **Step 1: Include.** After `#include "synthui_panel_button.h"` add `#include "synthui_slide_toggle.h"`.

- [ ] **Step 2: Statics.** `static lv_obj_t *playBtn, *tempoSeg, *noteLabel, *waveBtnLabel;` → `static lv_obj_t *playBtn, *tempoSeg, *noteLabel;` (the toggle needs no file-scope pointer: `cbWave` reads its event target).

- [ ] **Step 3: `cbWave`.** Replace the whole function with:

```c
/* VALUE_CHANGED from the slide toggle, which cycles its own value on a tap
 * anywhere in its box and then sends this.  0 = SAW (knob left), 1 = SQR.  The
 * widget holds the only UI copy of the state; the voice keeps the truth. */
static void cbWave(lv_event_t *e)
{
    const bool square = synthui_slide_toggle_get_value(lv_event_get_target_obj(e)) != 0;
    acid.waveform(square ? WAVEFORM_SQUARE : WAVEFORM_SAWTOOTH);
}
```

- [ ] **Step 4: `build_ui()`.** Replace the three `wave` lines with:

```c
    /* The plate is VISIBLE on purpose: the widget paints an opaque panel over
     * its whole box and draws its glyphs in a fixed #232526 (no glyph-colour
     * setter), so matching the plate to this screen's #101820 would erase the
     * legends.  0x6D7A85 is one of the DC reference's own four panel colours.
     * Boot value 0 = SAW mirrors default_patch()'s WAVEFORM_SAWTOOTH. */
    lv_obj_t *wave = synthui_slide_toggle_create(scr);
    lv_obj_set_size(wave, WAVE_W, WAVE_H);
    lv_obj_set_pos(wave, WAVE_X, WAVE_Y);
    synthui_slide_toggle_set_positions(wave, 2);
    synthui_slide_toggle_set_left_glyph(wave, SYNTHUI_SLIDE_TOGGLE_GLYPH_SAW);
    synthui_slide_toggle_set_right_glyph(wave, SYNTHUI_SLIDE_TOGGLE_GLYPH_SQUARE);
    synthui_slide_toggle_set_panel_color(wave, 0x6D7A85u);
    synthui_slide_toggle_set_value(wave, 0);
    lv_obj_add_event_cb(wave, cbWave, LV_EVENT_VALUE_CHANGED, NULL);
```

- [ ] **Step 5: Delete `mkbtn()`.** Remove the whole function (`UIBUILD_FN static lv_obj_t *mkbtn(…)` through its closing `}`). Keep the `#define UIBUILD_FN` line and its comment, editing `build_ui() runs once from setup(), and mkbtn()/mkknob() only from build_ui().` → `build_ui() runs once from setup(), and mkpanelbtn()/mkknob() only from build_ui().` Confirm nothing is left: `grep -n "mkbtn\|lv_button\|waveBtnLabel\|bpmLabel\|playBtnLabel\|LV_SYMBOL_" "$AB/acid_box.cpp"` → no output.

- [ ] **Step 6: Build, headroom.** Procedure A; record the number. `-Werror`-class noise to expect: none; an `unused function` warning means Step 5 missed something.

- [ ] **Step 7: Gate run 1 → `FAIL: UI golden`, note `<NEW3>`. Step 8: run 2 → identical `<NEW3>`.**

- [ ] **Step 9: Dump and LOOK.** Procedure B; `fnv == <NEW3>`. Eye-check the editor column: a mid-slate (`#6D7A85`) plate at 1080..1263 × 222..277 with a dark SAW zig-zag at its left end, a dark SQUARE wave at its right end, a black housing between them and the ridged knob sitting in the LEFT half (SAW); ACC/SLD keys above and the prev / "01" / next row below unchanged and not overlapped (plate bottom 277 < step row top 300). Measure the plate:
```bash
python3 - "$S/fb.png" <<'EOF'
import sys; from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); p = (0x6D,0x7A,0x85)
xs = [x for x in range(1040, 1280) if im.getpixel((x, 224)) == p]
ys = [y for y in range(200, 300) if im.getpixel((1081, y)) == p]
print("plate x", xs[0], xs[-1], " y", ys[0], ys[-1])
EOF
```
Expected: `plate x 1080 1263  y 222 277` (x may start a pixel or two later if the SAW glyph's first stroke crosses row 224 — then sample another row and say which).

- [ ] **Step 10: Pin golden 3.** `<NEW2>` → `<NEW3>`; comment:
```sh
# MOVED 2026-09-17 (NEW-54, 3 of 3): <NEW2> -> <NEW3>.  SAW/SQR became a
# synthui_slide_toggle on its old rect, panel 0x6D7A85 (the widget's glyph colour
# is fixed dark, so the plate must stay visible).  mkbtn() and lv_button are gone
# from this example.  Two runs bit-identical, frame dumped and looked at.
```
Gate: `PASS`.

- [ ] **Step 11: All four headroom numbers.** Rebuild the three other human-owned dirs IN PLACE — `cmake --build` only, never `cmake -B … -D…` (`build-bench` carries a real BT firmware blob in its cache that a reconfigure from flags would strip):
```bash
cd "$AB" && for d in build-loopstat build-bt build-bench; do cmake --build $d 2>&1 | tail -4; done
for d in build build-loopstat build-bt build-bench; do printf "%-15s %s\n" $d "$(itcm_headroom $d/acid_box.elf)"; done
```
Expected: four numbers, each within ~300 B of `2884 / 2756 / 11604 / 11412`. State plainly whether the <300 B prediction for `build` HELD or was REFUTED.

- [ ] **Step 12: Splice.** Procedure C. Narrative: heading reason → `NEW-54 waveform`; paragraph:
```
RE-RECORDED 2026-09-17 (NEW-54, 3 of 3 -- waveform): the SAW/SQR lv_button
became a synthui_slide_toggle on its old 184x56 rect -- SAW left, SQUARE right,
knob left at boot -- on a VISIBLE 0x6D7A85 plate, because the widget draws its
glyphs in a fixed dark #232526 and a ground-coloured plate would erase them.
cbWave reads the widget's value; its private static flag is gone, and with it
mkbtn() and this example's last use of lv_button.  Golden <NEW2> -> <NEW3>.
ITCM headroom, all four builds, before -> after NEW-54: default 2884 -> <n>,
LOOPSTAT 2756 -> <n>, build-bt 11604 -> <n>, build-bench 11412 -> <n> B; the
pre-registered "<300 B on the default build" prediction <HELD|was REFUTED>.
```
`THE BOOT GOLDEN`: value → `<NEW3>`; eye-check `ACC lit amber, SLD dark, SAW,` → `ACC lit amber, SLD dark, the slide toggle's knob at SAW on its slate plate (1080..1263 x 222..277),`; prepend `<NEW2> (2026-09-17, NEW-54 tempo), ` to `History:`.

- [ ] **Step 13: Replay** (Procedure D → PASS) **and confirm the touch script never moved:** `cd "$EVKB" && git diff --stat master -- examples/display/acid_box/touch_script.txt` → empty; `shasum -a 256 examples/display/acid_box/touch_script.txt | cut -c1-16` → `d1bae9e7f117a6c1`.

- [ ] **Step 14: Commit.**
```bash
cd "$EVKB" && git add examples/display/acid_box/acid_box.cpp examples/display/acid_box/run_qemu.sh examples/display/acid_box/transcript_qemu.txt
git commit -m "acid_box: SAW/SQR on synthui_slide_toggle (0x6D7A85 plate -- the widget's glyph colour is fixed dark), cbWave reads the widget, mkbtn() and lv_button deleted, golden <NEW2> -> <NEW3>, four ITCM numbers recorded (NEW-54, 3 of 3)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: Software close-out

**Files:** `$EVKB/examples/README.md`, `$EVKB/CLAUDE.md`; Linear NEW-54; the memory directory.

- [ ] **Step 1: Bench configurations still link, and the proxies still match.**
`cd "$EVKB" && tools/build-bench-configs.sh -n 2>&1 | tail -12` — Expected: every declared configuration `OK`, both `nm-diff OK`. If an nm-diff fails, read the SYMBOL LIST before anything else: the human-owned dir being stale (Task 3 Step 11 skipped) and a real proxy mismatch print the same verdict.

- [ ] **Step 2: Vacuity, alone.** `cd "$EVKB" && sh tools/gate-vacuity.test.sh 2>&1 | tee "$S/vacuity.log" | grep -c "^PASS:"` → **68** (66 + NEW-54's two), and `grep "^FAIL:\|^SKIP:" "$S/vacuity.log"` → empty. (~9 min. If it aborts mid-suite with a missing `$WORK/…` file, two suites overlapped — run it again, alone.)

- [ ] **Step 3: Sweep.** Read `docs/KNOWN-BROKEN-GATES.md` first. `cd "$EVKB" && ./tools/run-all-qemu-gates.sh 2>&1 | tee "$S/sweep.log" | tail -5` (~24 min). Target `gates: 141 passed`. Any red that is not `display/acid_box`: re-run that gate ALONE and idle before believing it (the load-sensitivity class), and report names, not counts.

- [ ] **Step 4: Licence audit, AFTER the sweep.** `cd "$EVKB" && LICENSE_AUDIT_EVKB=$(pwd) tools/license-audit.sh 2>&1 | tail -3` → `LICENSE-AUDIT: PASS`.

- [ ] **Step 5: `examples/README.md`.** In the display row's acid_box description, after `a \`synthui_seven_segment\` step readout between REWIND/FORWARD panel buttons,` insert ` and since NEW-54 a top bar on the same family -- PLAY (lit green while playing) and STOP panel buttons, a seven-segment tempo readout between DOWN/UP panel buttons -- plus a \`synthui_slide_toggle\` for SAW/SQR,`.

- [ ] **Step 6: `CLAUDE.md`.** Add a `✅ **Measured 2026-09-17 …` block above the 2026-09-16 NEW-50 block, stating only what was measured: the sweep line, vacuity 68/68, audit PASS, gate count unchanged at 141, the three golden moves with reasons, the four ITCM before→after numbers and whether the prediction held, the three widget facts from the spec's §2, both RED demos with their exact FAIL lines, and "gpu golden `0xEA5AB843` is STALE until the NEW-54 bench". Also update the two earlier mentions of the sw golden `0xBB2AEE59` as the CURRENT value (search for it) with a `(superseded 2026-09-17 by NEW-54: <NEW3>)` note rather than rewriting history.

- [ ] **Step 7: Commit the docs.** `git add examples/README.md CLAUDE.md && git commit` with the message `docs: NEW-54 software close-out -- <the sweep's gates: line>, vacuity 68/68, audit PASS, golden 0xBB2AEE59 -> <NEW3> in three recorded moves, ITCM default 2884 -> <n> B` plus the trailer.

- [ ] **Step 8: Linear + memory.** Update NEW-54's description with a **Measured** table (sweep, vacuity, audit, bench check, goldens, ITCM) and leave it In Progress with "Outstanding: the bench". Write `memory/new54-acid-box-top-bar.md` (type `project`) and its one-line pointer in `MEMORY.md`.

- [ ] **Step 9: Stop.** Report to the user; merging to master and the bench are theirs to call.

---

### Task 5: Silicon bench (needs a person at the board)

**Files:** `$AB/transcript_hw_evkb.txt`, then `CLAUDE.md`, NEW-54.

Use the `flashing-rt1170-evkb` skill for the flash/console procedure. **Detach `tools/rt1170-console.py` before EVERY LinkServer operation** (holding the VCOM across a re-enumeration has kernel-panicked this Mac). Flash `build/acid_box.hex` if the `.elf` is refused.

- [ ] **Step 1:** Flash the default build; attach the console; press SW4. Reset witness: `ACIDBOX_ROT ops=` and `ACIDBOX_VSYNC flips=` restart at 1 (the boot banner is lost across the CDC drop — its absence means nothing).
- [ ] **Step 2: gpu golden.** Three SW4 boots, touching nothing. `ACIDBOX_ENGINE=gpu`, `ACIDBOX_GPU_ERR=0`, and `ACIDBOX_UI_SUM` identical on all three → record it; retire `0xEA5AB843` with the reason "top bar and SAW/SQR moved to SynthUI widgets (NEW-54)". If the three disagree, stop — do not pick one.
- [ ] **Step 3: Counters over ≥ 20 bars of playing:** every `ACIDBOX_VSYNC` line `timeouts=0`; every `ACIDBOX_ROT_EQ` line `fail=0`; last `ACIDBOX_ROT` `full<=2`; `PLAY_LIT=1` follows `PLAYING=1`, and `PLAY_LIT=0` follows a pause and a stop.
- [ ] **Step 4: By hand** (ask the person; record their words): PLAY lights green on play, dark on pause, dark on stop · STOP, DOWN, UP light while held and clear on release AND on a finger slid off the button · tempo stepped 99 ↔ 100 and held to both rails (20.0, 999.0): the readout box never jumps and the leading cell ghosts · a tap anywhere on the toggle slides the knob and the timbre changes saw ↔ square BY EAR · no scanout flash during any of it.
- [ ] **Step 5: Contamination check.** `grep -n "^wiggle=1" <capture>` — a BARE `wiggle=1` line means the title label was tapped and the 8-knob sweep ran: discard that run, do not analyse it. (In a LOOPSTAT line `wiggle=` is MICROSECONDS and `1` means off.)
- [ ] **Step 6:** Append a `NEW-54` section to `transcript_hw_evkb.txt` (by hand — it is a narrative too), update CLAUDE.md's gpu golden mentions, commit, and move NEW-54 to Done only if every item above was met; otherwise say which was not.

---

## Self-Review (done at writing time)

- **Spec coverage:** §3 transport → T1; tempo → T2; waveform → T3; §4 code shape → T1 S4-6, T2 S1-4, T3 S1-5; §5.1 golden ×3 → T1 S8-11, T2 S6-9, T3 S7-10; §5.2 witness → T1 S2/S6; §5.3 RED demos → T1 S12-13; §5.4 fixture + vacuity → Procedures C/D, T1 S14-16, T4 S2; §5.5 comments → T1 S11; §6 ITCM → Procedure A, T3 S11; §7 close-out → T4; §8 bench → T5; §9 risks → T1 S8's stop condition, Procedure C's diff review.
- **Names used consistently:** `playBtn`, `tempoSeg`, `PANEL_GLYPH_SCALE_DEFAULT`, `PANEL_GLYPH_SCALE_STEP`, `mkpanelbtn(scr, x, y, w, h, glyph, accent, scale, cb, press)` with `press` one of `PANEL_STATEFUL` / `PANEL_MOMENTARY`, `TEMPO_SEG_X/W`, `PLAY_LIT=`, `acb_play_never_lit_fails_by_name`, `acb_lit_at_boot_fails_by_name`, FAIL texts `PLAY not dark at boot` / `PLAY never lit after the tap` / `no PLAYING=1 line to order against`.
- **Deliberate placeholders:** `<NEW1>`, `<NEW2>`, `<NEW3>` and the measured numbers are values that do not exist until the step that measures them; each is defined by the step that produces it.
