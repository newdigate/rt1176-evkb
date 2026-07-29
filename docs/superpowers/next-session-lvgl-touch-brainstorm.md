# Brainstorm: LVGL input on the RK055HDMIPI4MA0 (v3)

Start by invoking the `superpowers:brainstorming` skill — this is a design
conversation, not an implementation request. Explore before proposing.

## The goal

Bind the GT911 to LVGL as an **`lv_indev`**, so a finger drives real LVGL
widgets on the RK055 panel. This is the **v3** milestone of the display
roadmap; v1 (pixels) and v2 (raw touch) are both done and hardware-verified.

## Where things are

- Project: `~/Development/rt1170/evkb` (`github.com/newdigate/rt1176-evkb`),
  branch **`master`**. Read `CLAUDE.md` first — build, two-gate rule, flashing,
  repo layout.
- **v2 is merged** (`1823098`, "Merge branch 'rk055-touch'"). Nothing is
  outstanding from it except the two open findings in §Risks below.
- Spec: `docs/superpowers/specs/2026-07-28-rt1176-rk055-touch-design.md`
  — **read §3.3's dated amendment**, it is the single most useful page for this
  task.
- Plan: `docs/superpowers/plans/2026-07-28-rt1176-rk055-touch.md`
- Evidence: `examples/display/rk055_touch_test/transcript_hw_evkb.txt`
- Memory: **`rt1176-rk055-touch`** and **`rt1176-rk055-display`** — read both.
- NXP MCUXpresso SDK (BSD-3): `~/Development/mcuxsdk-ws`. Its
  `examples/lvgl_examples/lvgl_sdk/lvgl_support/rt11xx/lvgl_support.c` has a
  working `lv_indev` over the same part — `DEMO_ReadTouch()` and
  `lv_port_indev_init()`. Facts may be transcribed with citation; code
  structure may not be copied.

## Verified facts (I checked these; sanity-check, don't re-derive)

**LVGL layout.** `~/Development/LVGL` — `lvgl/` is vendored upstream (**v9**
API: `lv_display_t`, `lv_indev_create`, `lv_indev_set_read_cb`), `port/` is
ours. `port/` currently holds `lv_conf.h`, `lvgl_rt1176.{cpp,h}`,
`lvgl_ili9341.{cpp,h}` and `lvgl_mipi_panel.{cpp,h}`. **There is no input
binding of any kind yet** — v3 writes the first one. `lv_indev.c` already
compiles as part of LVGL core. `LV_USE_GESTURE_RECOGNITION` is **0**.

**The display binding is direct-render, and that matters here.**
`lvgl_mipi_panel_create(DisplayClass &)` hands LVGL the very buffer LCDIFv2 is
scanning out — there is no blit, and `flush_cb` only latches completion. Its
header says plainly that v1 **accepts tearing**: no back buffer, no vsync
fence. That was tolerable for a static scene. It will be much more visible
once a finger is dragging something.

**The GT911 driver API** (`~/Development/TouchPanel`, pinned `d20499c`):

```cpp
enum class Poll : uint8_t { Idle, Contacts, Released, Failed };
[[nodiscard]] Poll read(TouchPoint *points, uint8_t maxPoints,
                        uint8_t *count = nullptr);
```

Read `gt911/gt911.h` before designing anything — it documents which of
`read()`'s guarantees are structural and which merely rest on the step order,
and it states its own trust boundaries.

**Import pattern**, from `examples/display/lvgl_rpi_panel_test/CMakeLists.txt`:
`import_evkb_lvgl()`, then `evkb_library_dir(LVGL _lvgl_dir)`, then compile
`${_lvgl_dir}/port/<binding>.cpp` **into the example**, and link LVGL with
plain `target_link_libraries(<tgt>.elf LVGL stdc++)` — *not*
`teensy_target_link_libraries`, which rewrites names to `<name>.o` and would
drop LVGL's PUBLIC include dirs.

**Panel geometry:** 720×1280 portrait. The real part reports `RES=720x1280`
and 5 contacts (silicon-confirmed; the driver scales by the reported value and
asserts nothing).

## ★ The problem to think hardest about: `Idle` is not `Released`

LVGL calls `read_cb` on *its* schedule and expects the **current** state every
call — `LV_INDEV_STATE_PR`/`REL` plus a point. The GT911 publishes on *its*
schedule, and a poll loop runs far faster than it publishes, so most polls
return `Idle`, meaning "nothing new since you last acknowledged me".

**`Idle` must not be forwarded as a release.** The binding has to latch. This
is not a hypothetical: spec §3.3's amendment records **three** bugs that came
from conflating those two states, one of them in this project's own gate,
which stayed green against a permanently wedged part. The `Poll` enum exists
precisely so the distinction cannot be lost — do not throw it away at the LVGL
seam.

## Architectural questions to work out early

1. **Where does the binding live?** `LVGL/port/` next to the display binding
   (couples LVGL to `TouchPanel`), inside `TouchPanel` (couples it to LVGL), or
   behind a callback seam so neither library knows the other. Note the display
   binding stayed panel-neutral by taking a `DisplayClass&`; the symmetric move
   is taking a `GT911&`, and the symmetric cost is a hard dependency. Decide
   deliberately — the FT5406 is still the notional second controller.

2. **Rotation.** The panel is portrait; LVGL apps often want landscape. If
   rotation is ever applied, the **touch mapping must rotate with it**, and a
   rotation bug is invisible in QEMU for exactly the reason orientation was:
   the model places contacts at the same percentages the firmware uses. NXP's
   `DEMO_USE_ROTATE` path shows the shape. Decide whether v3 rotates at all, and
   say so.

3. **Polled or interrupt-driven?** LVGL polls via `lv_timer_handler`. v2's INT
   line is a corroborating counter only, and the two open findings below would
   land squarely on an interrupt-driven design.

4. **Multi-touch.** LVGL's pointer indev is single-contact. The driver returns
   up to 5 sorted by track id. Which one wins, and what happens when it lifts
   while another is still down?

## ★ Verification — the interesting part, again

v2 proved *coordinates*. v3 must prove **LVGL reacted** — a different claim.

The existing LVGL gates assert a render checksum
(`LVGL_SUM=0x…`, `LVGL_BYTES=…` in `lvgl_rpi_panel_test/run_qemu.sh`). A
checksum alone cannot distinguish "the widget moved because of a touch" from
"the scene changed for some other reason", so think about what a *sufficient*
assertion looks like — e.g. a button that must latch pressed from the scripted
taps, and an object whose position must track the scripted drag, with the
checksum as corroboration rather than as the proof.

**The QEMU GT911 model already replays a 25-instant script** the firmware
cannot influence (5 taps + releases, a 10-sample drag, 3 two-contact holds).
It was shaped for v2's three phases. If v3 needs different contacts, changing
the model is allowed **only to make it stricter or more faithful** — never to
make a divergence disappear. Read the "WHAT IT CANNOT PROVE" section of
`~/Development/qemu2/include/hw/i2c/imxrt_gt911.h` first; it is honest about
its own limits, including that orientation is hardware-only by construction.

## Risks to take seriously

1. **Two open hardware findings from v2**, both in
   `docs/arduino-header-revc3.md` under "Board traps":
   - **`SWITCH1=0x05`** — the panel is programmed for a **falling-edge**
     interrupt while the firmware attaches `RISING`, and the QEMU model serves
     `0x00`. NXP's own config requests rising; this panel disagrees, and we
     deliberately never write the config space, so the stored value governs.
   - **`INT_EDGES` exceeded `BUFFERS` by 6.5%** with `POLL_FAILS=0` — the line
     moved with nothing published, the predicted symptom of a floating `D6`
     (no pull-up on `CTP_INT` anywhere in the RevC3 netlist).

   Neither is closed. Both need a scope on `GPIO_AD_00` and an `INPUT_PULLUP`
   experiment. They do not affect a polled design; they are fatal to an
   interrupt-driven one.

2. **D6 and D9 are the touch INT and RST lines.** `irq_attach_test` jumpers
   D13→D9. Because the I²C address is latched from the INT level at reset
   release, anything loading those pins can silently move the device's address.

3. **Tearing.** Direct-render with no vsync fence. Dragging a widget across a
   live scanout buffer is the worst case for it. This may force the v4
   double-buffer/PXP work earlier than planned — decide whether v3 tolerates it
   or pulls that milestone in.

4. **The LVGL tick.** Check how `lvgl_rt1176.cpp` feeds `lv_tick_inc` before
   assuming input timing is sane; indev press/release/gesture timing all derive
   from it.

5. **The sweep expectation has changed.** `docs/KNOWN-BROKEN-GATES.md` now
   describes **68 gates** (discovery widened from `run_qemu.sh` to
   `run_qemu*.sh` on 2026-07-29), and `dualcore/cm4_audio_test` is recorded as
   **intermittent, not reliably broken** — both `67 passed, 1 failed` and
   `68 passed, 0 failed` are acceptable. **Read that file before any sweep.**
   Any *other* failure is a real regression.

6. **Two follow-ups are outstanding** and may or may not have been done — check
   before duplicating: I²C fault coverage for the GT911 `read()` path (two of
   its guarantees rest on step order that no current test can check), and a
   sweep of two silent-failure patterns in other gates' `run_qemu.sh`.

## Non-negotiable project conventions

- **Two-gate rule:** every capability needs a QEMU gate *and* hardware
  verification with un-fakeable assertions. **Silicon wins.** Never weaken a
  gate or the QEMU model to make a divergence disappear — document it.
- **An assertion must not read stronger than it is.** Four vacuous assertions
  were caught during v2, each of which passed reliably while proving nothing,
  and every one was found by someone deliberately trying to break the test
  rather than by the test failing. Budget for that.
- **Licence firewall:** MIT/BSD only, enforced by `tools/license-audit.sh`.
  LVGL is MIT; NXP's `lvgl_support.c` is BSD-3. Transcribe facts with citation,
  never code structure. A new example with a `run_qemu.sh` **must** be added to
  the Part-2 `GATES` list or the audit fails by design.
- Run gates as `./run_qemu.sh`, **never** `sh run_qemu.sh`.
- **Check `uptime` and `ps` for a competing `run_qemu.sh`/`qemu-system-arm`
  before every gate run.** A starved host produces missing or truncated UART
  capture that mimics a regression convincingly, and two runners in one
  directory corrupt each other's capture. This cost real time during v2.
- **Flashing:** do **not** hold the VCOM while programming (DAP status 131).
  Order: `flash … load` → `flash … verify` (both VCOM-free) → attach
  `tools/rt1170-console.py` → reset via a backgrounded `LinkServer run` (there
  is no standalone `reset` subcommand). `pkill LinkServer` also needs
  `redlinkserv` and `crt_emu_cm_redlink`.
- **Confirm the panel is connected before planning a hardware gate.** It was
  for v2 (RK055 on `J48`, RPi panel disconnected); check rather than assume.

## What I want out of this session

A validated design — scope, architecture, decomposition, and an honest
verification story (what QEMU can prove about LVGL's *reaction* versus what
only a finger on real glass can) — written to `docs/superpowers/specs/`.
Implementation comes after, as its own plan.

Ask me questions one at a time. Where something is uncertain, say so and check
it against the SDK, the LVGL source, the reference manual
(`~/Development/rt1170/rm_full.txt`), the RevC3 netlist
(`~/Development/rt1170/MIMXRT1170-EVKB-DESIGNFILES_RevC3/pst2kicad/board.net`)
or the board — rather than assuming.
