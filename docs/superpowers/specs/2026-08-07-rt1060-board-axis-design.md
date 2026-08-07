# RT1060-EVKB as a second board in this tree

**Date:** 2026-08-07
**Status:** design approved, not yet implemented
**Scope of this document:** Phase 1 (the board axis) in full. Phases 2–5 are
sketched as a roadmap and each gets its own spec when it is reached.

---

## 1. What this is not

It is not a port.

The request was "port the USB host audio to the RT1060 EVKB". Exploration
found that nearly every layer already exists, built for other reasons over the
past two months, and that the RT1062 is the **home platform** of the USB host
library rather than a target it must be carried to. The work that remains is
almost entirely in the top layer of this repository — examples, tools, gates —
plus one small qemu2 change.

The single most useful thing this document can do is record what already
exists, with evidence, so that nobody re-derives it.

### 1.1 Already in place

| Layer | State | Evidence |
|---|---|---|
| Core | RT1060-EVKB supported: `TEENSY_VERSION 42`, `imxrt1060_evkb.ld`, `ARDUINO_MIMXRT1060_EVKB`, 32 MB SEMC SDRAM | `teensy-cores` `1282db4`; macros `769660b` |
| Build macros | `42` and `117` dispatched in one `if/elseif`, on `main` | `teensy-cmake-macros/CMakeLists.include.txt:86–89` |
| USB host transport | RT1062 *is* Teensy 4.x silicon; the `__IMXRT1062__` branch is upstream's own | `USBHost_t36/ehci.cpp:180` |
| Board USB host port | **J47 = USB_OTG2 = Host**; J48 = OTG1 = Device. Dedicated `USB_OTG2_VBUS` rail, no GPIO switch, no jumper | `USBHost_t36` `1ef01c3`, README |
| Codec | `control_wm8960.{h,cpp}`; `output_i2s.cpp` carries EVKB SAI1 pinmux | `output_i2s.cpp:83,455,492` |
| QEMU SoC | 819-line `fsl-imxrt1062.c`, both ChipIdea controllers + UTMI PHYs | `qemu2 hw/arm/fsl-imxrt1062.c:159–167,290–293,614–627` |
| QEMU board | `mimxrt1060-evk`, WM8960 on LPI2C1 @ 0x1A | `qemu2 hw/arm/mimxrt1060-evk.c:333` |
| Silicon | LED blink, serial console over MCU-Link VCOM, LinkServer flash, **and audio out through the WM8960** all confirmed working by the user | — |

That last row removes what would otherwise have been an entire bring-up phase.
The console is the load-bearing one: every gate and every transcript in this
tree is UART tokens, so without it nothing can be asserted.

### 1.2 Two claims checked rather than assumed

Both are load-bearing, and both would have cost bench time if taken on trust.

**The DMA-reachability problem does not exist on this board.** On the RT1176,
plain `.bss` is DTCM, which the EHCI bus master cannot reach — which is why
every USBHost_t36 pool carries a `__IMXRT1176__`-only `DMAMEM`
(`memory.cpp:67`, `ehci.cpp:67`, `ehci_iso.cpp:149`, `hid.cpp:41`,
`enumeration.cpp:53` — note the tree's own cross-reference in `ehci_iso.cpp`
says `ehci.cpp:66`, which is a comment line, not the guard). On the EVKB, `.bss` is placed in `RAM`, and `RAM` is
`ORIGIN = 0x20200000` — OCRAM (`imxrt1060_evkb.ld:10,67`). DTCM there holds
only the stack. The `#else` branch (empty `USBHOST_DMAMEM`) is therefore
already correct, and the comments in those files that say so are accurate.

**The USBHost_t36 README's QEMU caveat is stale.** It states the RT1062 model's
USB is "device-mode only (no EHCI host emulation)". `TYPE_CHIPIDEA` derives
from `TYPE_SYS_BUS_EHCI` and is *shared* by `fsl-imxrt1062.c` and
`fsl-imxrt1170.c`; host support landed underneath both when the 1170 work was
done (`hw/usb/chipidea.c:114–116,126,143` — device mode owns the registers,
host mode delegates to the EHCI core). What the 1062 SoC lacks is the wiring
the 1170 added *on top*: naming the host controller so a device can be
attached to its bus (`fsl-imxrt1170.c:1415`), and a `usbhost-dma-view` address
space with TCM holes (`fsl-imxrt1170.c:80–100`). **This README line should be
corrected as part of Phase 2, not carried forward.**

### 1.3 The real gaps

1. The top layer of this repo is single-board. 81 gates, **all 81 hardcoding
   `-M mimxrt1170-evk`**.
2. `qemu2`'s `mimxrt1060-evk` never got the host-side wiring (§1.2).
3. The seven `usb_audio*.cpp` files have never been compiled for
   `__IMXRT1062__`. Encouragingly they carry **zero** chip guards — no
   `DMAMEM`, no `__IMXRT1176__`, no OCRAM references — so they are chip-
   agnostic by construction and the platform assumptions all live in the
   transport layer below them.
4. The RT1062 is **single-core at 600 MHz**. All of Phase 7 (the CM4 work) is
   inapplicable.

### 1.4 What single-core-at-600-MHz means for the budget

The CM7 measurement recorded in `usb_audio_duplex_test/transcript_hw_cpu_budget.txt`
found genuine USB duplex work costs **1.16 % of the CM7 at 996 MHz**
(regression intercept, R² = 1.00000 over a 19× loop-rate range), and that the
binding constraint is **contiguous stall length** — 600 µs clean, 850 µs fails.

Scaling to 600 MHz: the duty cost becomes roughly **1.9 %**, still trivial.
But the stall ceiling is wall-clock, not cycles — USB frames are 1 ms
regardless of core speed. So the ceiling does not move, while every piece of
work between service points takes **1.66× longer**. The RT1062 has less stall
headroom than the RT1176, not more, and that is the number to watch in Phase 4.

---

## 2. Scope decisions

Taken with the user during brainstorming, recorded so the plan does not
relitigate them:

| Decision | Choice | Rejected |
|---|---|---|
| End state | **Full second platform** — the 1060 becomes a first-class target with its own gates and examples | single gated capstone; bench-proof-only |
| Layout | **Board axis in this repo** | new sibling repo `rt1062-evkb`; extracting a shared harness repo first |
| Sequencing | **Board axis first**, before any 1060 USB work | thinnest-USB-slice-first; QEMU-model-first |

The layout decision follows what the layers below already did: `teensy-cores`
holds `imxrt1176/` and `teensy4/` side by side, and `teensy-cmake-macros`
dispatches both boards in one file. The top layer is the only one that has not
unified. Forking it would fork `tools/` — 2,695 lines across 18 files,
including the gate harness, the vacuity tests and the licence audit — so the
machinery that exists to catch drift would itself become the thing that
drifts.

The sequencing decision is about diagnosability, not speed. The refactor
touches `run-all-qemu-gates.sh`, `license-audit.sh`, `KNOWN-BROKEN-GATES.md`,
`evkb.cmake` and 81 gate scripts — everything that protects the tree. Doing it
against `blink` (already known-good on this silicon, already has
`tools/qemu_check_blink.py`, zero USB unknowns) means any red is definitively
the refactor.

---

## 3. The RT1062 QEMU tests belong to qemu2, and stay there

`~/Development/rt1062-p2-tests/` holds 13 bare-metal validation images
covering LPUART, GPIO, GPIO6, GPT, eDMA, ENET, uSDHC, SAI, the SGTL5000 codec,
SRC/OCOTP, LPI2C/LPSPI and FlexSPI fidelity. They are a useful record of which
RT1062 peripherals have been validated and by what method.

The instinct on reaching Phase 2 or 3 will be *"we already have RT1062 test
images — use those as the 1060's first gates"*. **Don't.** The reason is
engineering, not licensing, and it is worth stating in that order because the
licensing reason is the weaker one.

**They are QEMU model validation, and they already live in QEMU's functional
test suite** — `qemu2/tests/functional/arm/imxrt1062/`, 31 commits deep, of
which the loose directory above is a stale byte-identical snapshot of an early
13. That is exactly the right home for them. They are bare-metal `-kernel`
ELFs with hand-rolled vector tables and their own linker script; this repo's
examples are Teensy-core CMake builds that boot as FlexSPI XIP images. They
fit neither the example shape nor the gate shape here, and duplicating them
into a firmware repo would be worse engineering **at any licence**.

The licence position, recorded so it is not re-litigated:

- All 13 are `SPDX-License-Identifier: GPL-2.0-or-later`, and they are
  **published** — the suite is on `origin/master` and `origin/imxrt1062` at
  `gitlab.com/Newdigate/qemu-rt1170`.
- Sole authorship is established: every commit touching that directory is by
  the same author, no file asserts a third-party copyright, and `<stdint.h>`
  is the only include across all 13 — so none of it derives from QEMU's own
  GPL source.
- **Contributing to QEMU under copyleft is deliberate and fine.** The policy
  is one-directional and unchanged: firmware→qemu2 is fine, qemu2→firmware is
  not, and `tools/license-audit.sh` exists to prove no copyleft compiles into
  firmware. This tree stays MIT/BSD-only; every inherited LGPL file has a
  clean-room rewrite.
- The author could dual-license any of it MIT at will — a licence binds
  recipients, not the copyright holder. **That does not make importing it a
  good idea**, which is why the paragraph above leads. If a specific chunk is
  ever genuinely wanted in a firmware example, dual-licensing that one file is
  a two-minute edit and the author's call; the default answer stays no.

What *may* be taken freely, because neither is code:

- the **fact** of what has been validated in the 1062 model, and
- the **technique** in `sai_audio_test.c` — streaming SAI1 TX into
  `-audiodev wav` so a gate asserts on captured audio rather than on register
  state. Techniques are not copyrightable. Worth remembering for Phase 5.

Note also there is **no USB test among the 13**, which is consistent with the
stale README claim in §1.2 and means the 1062 model's host path has never been
exercised by anything.

---

## 4. Phase 1 — the board axis

### 4.1 Principle

An example declares which boards it supports. Everything else derives. Nothing
about the RT1176 experience changes.

### 4.2 `evkb.cmake`

Today the file is 295 lines with exactly **two functionally board-specific
lines**: line 54 picks `imxrt1176` as the cores subdirectory, line 294 derives
`COREPATH` from it. The remaining six mentions of "1176"/"1170" are comments.

It gains one cache variable:

```
EVKB_BOARD    rt1176 (default) | rt1062
```

from which it derives, in one place:

| Derived | `rt1176` | `rt1062` |
|---|---|---|
| cores subdirectory | `imxrt1176` | `teensy4` |
| `TEENSY_VERSION` | 117 | 42 |
| QEMU machine | `mimxrt1170-evk` | `mimxrt1060-evk` |

Defaulting to `rt1176` is what makes the change inert for every existing
example.

### 4.3 Per-example declaration

Each example declares `EVKB_BOARDS`. **The default is `rt1176` alone**, so all
99 example directories keep their present meaning without being edited. An
example opts in explicitly:

```cmake
set(EVKB_BOARDS rt1176 rt1062)
```

Build output moves to `build-<board>/`.

Categories that will never declare `rt1062`: `dualcore/` (23 examples — the
RT1062 is single-core) and the RK055 MIPI-DSI examples in `display/` (the
RT1062 has eLCDIF but no MIPI-DSI host). This is not a limitation to work
around; it is the reason the declaration is per-example rather than global.

### 4.4 Gates

This is the substantive part of the refactor, and the design choice that keeps
it from being 81 forked scripts.

**Today:** one gate script per example directory; gate id is
`<category>/<name>`; each script hardcodes `-M mimxrt1170-evk`.

**After:** one gate script per example directory still. The board arrives as a
parameter, the machine name is resolved in `tools/gate-lib.sh` alone, and the
runner invokes each gate once per board the example declares. Gate id becomes
`<board>:<category>/<name>`.

Consequences, all intended:

- The 81 existing gates become `rt1176:<category>/<name>`. Count and meaning
  unchanged.
- A gate cannot silently boot the wrong machine, because no gate names a
  machine.
- `run-all-qemu-gates.sh -l`, the licence audit's `GATES` list, and
  `KNOWN-BROKEN-GATES.md` all gain the board dimension.
- The SKIP signal is preserved per board: a declared board with no built ELF
  is a SKIP, exactly as now. **Zero SKIP remains the load-bearing number.**

### 4.5 Verification

Phase 1 is codegen-neutral for `rt1176` by construction. It gets the
byte-identity treatment used in Phase 2B and again for the `abs()` comment
fix:

1. **Before:** record `sha256` of a representative sample of `rt1176` ELF/hex
   and `.cm4.bin` outputs across categories — at minimum one `dualcore`, one
   `usb`, one `display`, one `audio`.
2. **After:** rebuild; every one must be byte-identical. A difference is a
   defect in the refactor, not an acceptable consequence of it.
3. **Sweep:** full run on an idle machine. Expect `81/0/0` for `rt1176`, or
   `80/1/0` with the documented `dualcore/cm4_audio_test` intermittent — plus
   the new `rt1062:gpio-analog/blink`. Zero SKIP.
4. **Licence audit:** PASS, with the `rt1062` gate present in `GATES`.

Machine load matters when reading step 3. `cm4_audio_test` is documented as
failing under load and passing 5/5 idle on the same cores pin; the sweep taken
during this design's own session ran at load 40–50 (against the ~4–5 of the
green runs) and produced exactly that red. Check `uptime` before believing a
lone dual-core red.

### 4.6 Definition of done

- `examples/gpio-analog/blink` declares both boards, builds for both, and its
  gate passes on both.
- `run-all-qemu-gates.sh` reports both boards with zero SKIP.
- Every sampled `rt1176` image is byte-identical to its pre-refactor build.
- `license-audit.sh` PASS.
- `CLAUDE.md` and `KNOWN-BROKEN-GATES.md` record the new gate count and the
  board dimension.

---

## 5. Roadmap — Phases 2 to 5

Each gets its own spec and plan when reached. Sketched here only so Phase 1's
design is checked against where it must lead.

**Phase 2 — qemu2 1060 host wiring.** Give OTG2 the `usbhost` id so
`-device usb-audio,bus=usbhost.0` resolves; add the `usbhost-dma-view` address
space if the firmware turns out to need TCM holes (it may not: on the RT1062
the USB structures live in `.bss`, which is already OCRAM — §1.2). Run
red-first, as Phase 7.1 did. Correct the stale README paragraph. **This change
stays local to `~/Development/qemu2` per the GPL firewall**, which means — as
with the IRQ-135 split — a fresh clone will see the corresponding gate red,
and that must be documented rather than worked around.

**Phase 3 — transport.** `usb_descriptor_survey` and `usb_enum_test` declare
`rt1062`. QEMU gate against the emulated `usb-audio`; silicon against the real
dongle in **J47**. Two known facts carry over: QEMU's `usb-audio` offers 48000
Hz only, and isochronous data does not flow against it — so this phase asserts
the descriptor plane only, and silicon remains the sole proof that audio moves.

**Phase 4 — audio driver.** The seven `usb_audio*.cpp` files compiled for
`__IMXRT1062__` for the first time; `usb_audio_uac1_test` gated and an audible
tone on silicon. Watch contiguous stall length here (§1.4).

**Phase 5 — capstone.** The RT1060-EVKB can do something the RT1176 capstone
could not: it has a **working on-board codec on the same board as the USB host
port**. So the capstone is USB audio IN → AudioStream graph → WM8960 line out,
exercising both paths at once, with `sai_audio_test`'s `-audiodev wav`
technique available as an oracle.

---

## 6. Risks

| Risk | Mitigation |
|---|---|
| The 81-gate refactor breaks something subtly | Byte-identity on a sampled set (§4.5), and `blink` as the only new variable |
| A lone dual-core red is misread as a regression | Documented load sensitivity; re-run idle; check whether the gate compiles what changed |
| The qemu2 RT1062 tests get imported as 1060 gates | §3 — wrong shape for this repo regardless of licence, and the licence audit backstops it |
| The qemu2 1060 change is local-only, so fresh clones go red | Same situation as the IRQ-135 split; document it in `KNOWN-BROKEN-GATES.md` when Phase 2 lands |
| `usb_audio*.cpp` hides an RT1176 assumption | They carry zero chip guards; if one surfaces it belongs in the transport layer, where the pattern already exists |
| Stall headroom is tighter at 600 MHz | Measure in Phase 4 rather than assume; the instrument already exists |

---

## 7. Open questions

Deliberately left open; none blocks Phase 1.

1. **Repo name.** This repo is `github.com/newdigate/rt1176-evkb` and will
   host two boards. A rename is cosmetic and can happen any time, or never.
2. **`~/Development/rt1060/evkb/`.** That workspace holds older `cores`,
   `teensy-cmake-macros`, `blink` and `SPI` checkouts from June. Once the board
   axis lands, it is redundant. Whether to delete it, or keep it as a fresh-user
   sanity check, is a later call.
3. **`camera/`** (5 examples). The MIMXRT1060-EVKB has a CSI connector, so
   these are not obviously inapplicable the way `dualcore/` is. Unassessed.
