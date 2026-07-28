# Brainstorm: capacitive touch on the RK055HDMIPI4MA0 (v2)

Start by invoking the `superpowers:brainstorming` skill — this is a design
conversation, not an implementation request. Explore before proposing.

---

## PROGRESS (2026-07-28) — delete this file once the plan exists

**Spec written and approved in conversation:**
`docs/superpowers/specs/2026-07-28-rt1176-rk055-touch-design.md`.

Decisions taken (all validated with the user, one question at a time):

1. **Scope** — raw touch only; LVGL `lv_indev` deferred to v3.
2. **Library home** — a new `newdigate/TouchPanel` sibling repo, `gt911/` subdir,
   `import_evkb_library(TouchPanel gt911)`. Not inside `MipiDisplay`: touch has
   none of MIPI-DSI's one-host-only constraint, and this keeps `Wire` out of the
   display library. No abstract base class until the FT5406 actually lands.
3. **Hardware assertion** — all three phases: five ordered on-glass targets, a
   directional swipe, and a two-finger hold.
4. **Config blob** — **read-only, never written.** `GT911_Init` would rewrite 186
   bytes of the touch IC's config; NXP warns a wrong write bricks it. We read,
   verify the checksum, and adapt to the stored point count instead.
5. **Read path** — polled, plus an `attachInterrupt` edge counter on INT as a
   corroborating token only.
6. **QEMU** — a virtual GT911 with the INT-level address latch, the mandatory
   `0x814E` status-clear enforcement, and a **model-owned** timer-driven touch
   path (no firmware-writable injection tap — that would be circular).
7. **Board state** — RK055 on `J48` only, RPi panel disconnected. Recorded as a
   stated precondition.

**Findings this session that were not in the brief below** (all now in the spec):

- **The `GPIO8`/`GPIO9` CM7 interrupt trap does not apply.** Those are fast
  aliases. Netlist: `J48.29 → CTP_INT_C → R2032 (0Ω) → GPIO_AD_00` (U19 N12),
  `J48.28 → CTP_RST_B_C → R2031 (0Ω) → GPIO_AD_01` (U19 R14). This core already
  muxes both through the **normal** ports at ALT5 (`digital.c:76,79`), so INT is
  CM7-interrupt-capable for free via GPIO2 (IRQ 103).
- **★ New board trap: touch INT/RST *are* Arduino D6 and D9.** `irq_attach_test`
  uses a D13→D9 jumper — D9 is the touch reset line. Both pads also appear raw on
  `J25` odd pins 15/13 and fan out through populated 0Ω into the SIM circuit
  (RevC3 sheet 22). Because the I²C address is latched from the INT level, that
  loading can silently change which address the device answers at.
- **`GT911_Init` is destructive** — it rewrites the config blob whenever the
  stored point count or trigger mode differ. The brief did not list this.
- The `irq_attach` QEMU fixture drives `gpio[2]` **input** 0 while the GT911 model
  needs `gpio[2]` **output** 0 — same pin, opposite direction, no contention, but
  it needs a comment in the machine file.

**Remaining:** write the implementation plan to `docs/superpowers/plans/`, then
delete this file.

---

## The goal

Add **capacitive touch** to the RK055HDMIPI4MA0 panel we just brought up, as the
v2 milestone of the display roadmap. Same "smallest vertical slice that proves
the chain" approach the display itself took.

## Where things are

- Project: `~/Development/rt1170/evkb` (`github.com/newdigate/rt1176-evkb`).
  Read `CLAUDE.md` first — build, two-gate rule, flashing, repo layout.
- Display work is on branch **`rk055-display`** across five repos (`evkb`,
  `cores/imxrt1176`, `~/Development/MipiDisplay`, `~/Development/LVGL`,
  `~/Development/qemu2`). All pushed, all clean.
- NXP MCUXpresso SDK (BSD-3, the reference source): `~/Development/mcuxsdk-ws`.
- Project memory: `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170/memory/`.

## Verified facts (I checked these; don't re-derive, but do sanity-check)

**The controller is a GT911 — NOT the FT5406.** This is the first thing to get
right, because the *other* panel uses the other chip:
`examples/lvgl_examples/lvgl_sdk/lvgl_support/rt11xx/lvgl_support.c`
`DEMO_InitTouch()` selects `FT5406_RT_Init()` only for
`DEMO_PANEL_RASPI_7INCH`, and `GT911_Init()` for **every other panel**,
including RK055.

- Driver: `mcuxsdk/components/touch/gt911/fsl_gt911.{c,h}` — **BSD-3-Clause**,
  so the licence firewall is a non-issue here, same as the HX8394 was.
- Bus: **LPI2C5 = `Wire2`**, `J48` pins 26/27 (`GPIO_LPSR_04`/`GPIO_LPSR_05`).
  NXP clocks it from OSC24M / 12 = **2 MHz** (`board.h`).
- 7-bit address **0x5D** (`kGT911_I2cAddrMode0`) or **0x14** (mode 1).
- **★ The I²C address is latched at reset from the INT pin level.** `GT911_Init()`
  drives INT as an *output* (`intPinFunc(kGT911_IntPinPullDown)` for 0x5D,
  `PullUp` for 0x14), releases reset, then flips INT to an input. Get this wrong
  and the device answers at the other address — or not at all. This is the
  single subtlest fact in the whole task.
- Touch RST = **`GPIO9_IO00`**, INT = **`GPIO8_IO31`** (`board.h`
  `BOARD_MIPI_PANEL_TOUCH_{RST,INT}_*`); `J48` pin 28 = `CTP_RST_B_C`, pin 29 =
  `CTP_INT_C` (RevC3 netlist).
- `intTrigMode = kGT911_IntRisingEdge` in NXP's config.
- The GT911 **reports its own resolution** (`GT911_GetResolution()`), which is
  not guaranteed to equal 720×1280 — decide early whether to scale or assert.
- It supports multi-touch (`GT911_GetMultiTouch`) as well as single
  (`GT911_GetSingleTouch`).

## The strongest prior art — read this before anything else

**The display half is done and HW-verified**, so the hard SoC work is behind you:

- Spec: `docs/superpowers/specs/2026-07-27-rt1176-rk055-display-design.md`
- Plan: `docs/superpowers/plans/2026-07-27-rt1176-rk055-display.md`
- Memory: **`rt1176-rk055-display`** — read this first, it has the whole story.
- Driver: `~/Development/MipiDisplay` (`soc/` + `panels/rpi7/` + `panels/rk055/`)
- Gate: `examples/display/rk055_panel_test/`, with three hardware transcripts.

**LPI2C5/Wire2 is already HW-proven on this bus** ([[rt1176-rpi-display-hw-bringup]]):
the WM8962 codec at 0x1A reads ID `0x6243` deterministically. Two hard-won
details from that work: **the pad must be `0x20` (open-drain), not `0x0A`** —
push-pull gives arbitration-lost on every address on this shared bus — and a
**stuck BBF (bus-busy latch) needs a physical power-cycle**; soft-reset, master
reset and a GPIO bus-clear all fail to clear it. Also see [[rt1176-lpi2c-wire]]
and [[rt1176-wire-library-move]].

**An architectural question to work out early:** the display library is split
`soc/` (panel-independent) + `panels/<name>/` (compile-time selected, because
the RT1176 has exactly one MIPI-DSI host). Touch does *not* share that
constraint — it is an I²C device, and two panels' touch controllers could in
principle coexist. So: does GT911 belong in `panels/rk055/`, in a new sibling
library, or somewhere else? Note the RPi panel's FT5406 is the natural second
implementation, which argues for a seam. The boundary is yours to design.

## Risks I'd want the brainstorm to take seriously

1. **The INT-pin address latch** (above). It is the most likely silent failure,
   and it means the INT pin needs *both* directions — output during reset, input
   afterwards — which complicates any "just configure it once" GPIO helper.
2. **`J48` pins 26/27 and `J84` pins 11/12 are the SAME NETS.** If both panels
   are physically plugged in, the RK055's GT911 (0x5D) shares LPI2C5 with the
   RPi board's ATtiny (0x45), its FT5406 touch (0x38) and an unidentified device
   at 0x4A. No address collision, but pull-up loading and bus contention are
   real, and the RPi panel needs its own 5 V to be alive at all. Decide whether
   the design assumes one panel connected at a time, and say so.
3. **Verification is genuinely hard, and this is the interesting part.** Touch
   needs a finger. What is the un-fakeable hardware assertion? A gate that
   prints "TOUCH_OK" because a register read succeeded proves nothing. Consider
   something a stuck or fabricated value cannot satisfy — e.g. requiring a
   *sequence* of touches in specific screen regions, or a swipe whose
   coordinates must move monotonically. QEMU can model a virtual GT911 that
   injects coordinates (the machine already models LPI2C5 with the RPi ATtiny on
   it, so there is a pattern to follow), but decide deliberately what that proves
   versus what only a real finger can.
4. **Polled vs interrupt-driven.** `GPIO8_IO31` for INT — check
   [[rt1176-gpio-irq-cm7-trap]] and whether GPIO8 is one of the fast-GPIO ports
   with the CM7 interrupt trap, and note the CM4 has no interrupt on fast-GPIO
   ports at all. Polled-first may be the smaller slice.
5. **Where the coordinates go.** v1 draws a static test pattern with no input
   loop. v2 needs somewhere to *put* touch data — a raw API, or straight into an
   LVGL `lv_indev`? The LVGL binding already exists
   (`~/Development/LVGL/port/lvgl_mipi_panel.*`, and
   `examples/display/lvgl_rpi_panel_test/`), so v2-then-v3 or v2-with-LVGL is a
   real scoping choice.

## Non-negotiable project conventions

- **Two-gate rule:** every capability needs a QEMU gate *and* hardware
  verification with un-fakeable assertions. **Silicon wins.** Never weaken a gate
  or the QEMU model to make a divergence disappear — document it.
- **Licence firewall:** MIT/BSD-only, enforced by `tools/license-audit.sh`. The
  GT911 driver is BSD-3, so transcribe values as facts with a source citation,
  never copy code structure, and consult no GPL source. **Part 2's `GATES` list
  now has a drift check** — a new example with a `run_qemu.sh` must be added to
  it or the audit fails. `tools/license-audit.test.sh` proves that check fires.
- Run gates as `./run_qemu.sh`, **never** `sh run_qemu.sh`.
- **Read `docs/KNOWN-BROKEN-GATES.md` before running the sweep.**
  `dualcore/cm4_audio_test` is broken for unrelated reasons — do not chase it.
  Expected sweep result is **28 passed, 1 failed, 0 SKIP**; any *other* failure
  is a real regression from your work.
- **Flashing:** do **not** hold the VCOM while programming — it kills the
  LinkServer debug session (DAP status 131). Order: `flash … load` →
  `flash … verify` (both VCOM-free) → attach `tools/rt1170-console.py` → reset
  via a backgrounded `LinkServer run` (there is no standalone `reset`
  subcommand). `pkill LinkServer` also needs `redlinkserv` and
  `crt_emu_cm_redlink`.
- **Confirm the panel is connected before planning a hardware gate.** It was for
  v1; check rather than assume.

## What I want out of this session

A validated design — scope, architecture, decomposition, and an honest
verification story (what QEMU can prove vs what only a finger on real glass can)
— written to `docs/superpowers/specs/`. Implementation comes after, as its own
plan.

Ask me questions one at a time. Where something is uncertain, say so and check
it against the SDK, the reference manual (`~/Development/rt1170/rm_full.txt`),
the RevC3 netlist
(`~/Development/rt1170/MIMXRT1170-EVKB-DESIGNFILES_RevC3/pst2kicad/board.net`),
or the board — rather than assuming.
