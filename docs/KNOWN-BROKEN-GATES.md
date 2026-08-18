# Known-broken gates — do not run these by default

Gates listed here are **broken for reasons unrelated to whatever you are working on**. Running them
will give you a red result that is not your fault and not your bug, and chasing it will cost you
time on a subsystem you probably are not touching.

**Read this before running `./tools/run-all-qemu-gates.sh`.**

---

## `rt1176:dualcore/cm4_wire_test` + `cm4_wire_int_master_test` — ✅ RESOLVED 2026-08-15

**Root cause: QEMU's WM8962 stub was replaced by a real model.** The stub read
back 0x0000 from every register; `hw/audio/wm8962.c` (qemu2 `23a86da9c3`)
returns the true **0x6243 device ID** for R15. The firmware writes R15←0x6243
and reads it back, so QEMU now returns exactly what silicon returns — and the
`rdv=00000000` these gates asserted was a property of the stub, not of the
device. Both now assert `rdv=00006243`, which is **stronger**: 0x6243 can only
come off the wire, whereas 0x0000 was equally what a read that moved no data
would leave in the firmware's zero-initialised `rdv`.

**Why no commit explained it.** The model change sat *uncommitted* in the qemu2
working tree, already compiled into the binary the gates ran against, and was
committed only after the investigation began. Every `git log` on the model and
the board looked a month stale, which is what made this look mysterious.
**Exonerated**: the 2026-08-14 shared cores/macros migration (`3bf458e`), the
Wire library, the LPI2C model, and the acid-bass phase.

★ **`cm4_wire_dma_test` was affected too, and silently.** It reads the same
register but deliberately does not assert `rdv`, so it stayed green while its
committed transcript went stale and its comment kept describing the vanished
stub. Both corrected. Its only reason for not asserting `rdv` was the split, so
asserting it is now an available strengthening — left for review rather than
taken unilaterally. **A grep for the failing assertion will not find gates in
this state; grep the printed token too.**

The original report is kept below, because the *shape* of the reasoning is
worth keeping: the gates were correctly identified as not-the-load-artefact
(they reproduced idle and after `rm -rf build`), and that is what ruled out
every candidate this tree usually blames first.

---

### Original report (superseded)

**Status: RED, reproducible, cause unknown.** Both fail on the same assertion:

```
FAIL: expected rdv=00000000        (reported: rdv=00006243)
```

Everything else in both gates passes, including `WIRE_CM4=PASS` /
`WIRE_INT_MASTER_CM4=PASS` from the firmware itself — it is the host-side
`rdv` check that is red, and the firmware believes it succeeded.

**This is NOT the load artefact, and that is the most important thing to know
about it.** The distinguishing test is re-running idle:

| | re-run idle | `rm -rf build` + reconfigure + rebuild |
|---|---|---|
| `cm4_audio_test` (the documented nondeterministic gate) | passes, ~1 s | — |
| these two | **fails identically** | **fails identically** |

Three consecutive idle runs of `cm4_wire_test` and one of
`cm4_wire_int_master_test` all produced the same `rdv=00006243`. So the
"re-run it idle before believing a red" rule in `CLAUDE.md` has already been
applied here and did not clear it. Do not spend the time again.

**What has been ruled out**, all on 2026-08-15:

- **Not caused by the acid-bass phase.** That branch changed only
  `docs/`, `examples/audio/acid_bass_test/` and the `GATES` list in
  `tools/license-audit.sh` (`git diff --name-only` over the whole branch).
  Neither failing example references the Audio library at all, and both ELFs
  were dated 2026-08-14 16:31 — before that session started.
- **Not a stale build.** A full `rm -rf build` and fresh configure reproduced
  it exactly, which also rules out the "build dirs configured before
  2026-08-14 cached an absolute toolchain path" trap.
- **Not an obvious upstream change.** `Wire` last changed 2026-07-24
  (`19babd1`), and the QEMU LPI2C/GT911 model 2026-07-29 (`db4bbd40b6`).
  Neither is close to the 2026-08-14 build date.

**Not investigated**: what `0x6243` actually is, whether the CM4 side reads a
real register or uninitialised memory, and whether the 2026-08-14 shared
cores/macros migration (`3bf458e`) is implicated — it is the change closest in
time, and it altered how the core is resolved for every example, but nothing
has been done to confirm or exonerate it. That is the first thread to pull.

**Do not weaken either gate to green the sweep.** Two independent gates
agreeing on the same unexpected value is evidence about the system, not noise.

---

## `rt1062:usb/usb_descriptor_survey` — RESOLVED 2026-08-08 (was red by design)

**Status: GREEN on both boards.** Kept here as a record because this gate was
deliberately carried red for part of a day, and because the reason it went green
reverses a conclusion this file previously asserted.

Phase 2 gave `fsl-imxrt1062` a USB DMA view with the ITCM and DTCM windows
punched out, mirroring `fsl-imxrt1170`. That turned the gate red: USBHost_t36's
`periodictable` linked at `0x20002000`, inside DTCM, so the modelled controller
could not read its own periodic list. The red was carried rather than fixed
because it was **not established** that RT1062 silicon actually enforces the
constraint — and the available evidence pointed the other way, since upstream
USBHost_t36 leaves those buffers in `.bss` (= DTCM) and USB host works on a
Teensy 4.1.

**The bench settled it, and the model was right.** On a MIMXRT1060-EVKB with a
USB audio adapter in J47, read over the debug probe while the firmware ran:

```
PORTSC1 = 0x10001805   CCS=1 PE=1 PP=1 -- device connected, powered, enabled
USBSTS  = 0x0000d09a   HCH=1 (halted) + SEI=1 (SYSTEM ERROR)
```

`SEI` is the controller faulting on its own DMA fetch from DTCM. Rebuilt with
those buffers in `DMAMEM`/OCRAM (USBHost_t36 `fa939cc`), `USBSTS` became
`0x0000d080` — SEI gone, same board, same cable, one variable changed. The same
change makes this QEMU gate pass **with the TCM holes still in place**.

So the red was a **true positive**: the model correctly refused an access the
silicon also refuses, and the firmware really was handing the controller
TCM-resident memory. Do not remove the holes.

✅ **The board enumerates too, as of the same day.** `DMAMEM` was only half the
fix. The EVKB core enables the D-cache and maps OCRAM write-back, so once these
descriptors moved out of DTCM the CPU's writes sat in cache while the EHCI read
stale physical memory — it walked a garbage periodic list and halted one frame
after start (`HCH=1`, `FRINDEX` frozen at `0x8`, and **no error bit**, because
the port-connect ISR had already acked the fatal status). Mapping OCRAM
non-cached on that board (`cores` `a090c9d`, guarded by
`ARDUINO_MIMXRT1060_EVKB`) closes it. The EVKB now walks all 244 descriptor
bytes of a real GeneralPlus UAC1 device on J47 — four interfaces, both
isochronous endpoints, the HID interface.

The two halves are causally linked, not independent bugs: DTCM is unreachable so
the buffers must go to OCRAM, and OCRAM is cached so moving them there creates
the coherency exposure. Fixing either alone leaves the port dead.

---

## `dualcore/cm4_audio_test` — BROKEN, cause unidentified

**Status:** broken. **Do not run by default.** Not a regression from any current work.

> ### ⚠ 2026-07-28 — it passed, 5 runs out of 5. Cause still unidentified; section stays.
>
> During the RK055-touch wrap this gate came up **green in the full sweep and in four further
> individual runs**, all on an otherwise idle machine (load average ~4–5). Not vacuous: it printed
> `fft_peak_bin=00000006`, `AUDIO_CM4_DET=PASS`, `codec_ack=1`, `cm7_audio_isers=0` — the four
> values it actually asserts — and exited 0 every time.
>
> **Unconfirmed hypothesis: machine load.** The 2026-07-27 bisect below ran while this machine was
> carrying load averages above 300 from unrelated builds. A starved host decouples QEMU's SAI
> timing from the audio graph, which would present exactly as "ISRs fire, dispatch runs, the FFT
> sees silence". That would make **every row of the bisect table below a false negative**, which is
> also why none of them moved the needle. This is a hypothesis, not a finding — it has not been
> tested by deliberately reloading the machine and re-running.
>
> One real difference from the committed transcript: `underruns` is now `0x2E4`–`0x2E7` (~740), not
> 0. The gate does not assert `underruns`, so it passes regardless, and the transcript's
> hardware-only `AUDIO_CM4=FAIL` line is likewise unasserted. Whether the underruns are the same
> host-starvation artefact is unknown.
>
> **The section stays until someone understands why.** The rule below is that the only way off this
> list is a fix, and nobody fixed anything — the gate changed behaviour on its own, which is a
> weaker reason to trust it, not a stronger one. Cheapest next step is now: re-run it under
> deliberate heavy load and see whether the old red reproduces.
>
> **★ A second variable, recorded because it confounds the first: the host was REBOOTED between
> the red runs and the green ones.** Earlier in the same session `uptime` read `up 45 days` under
> load 300+; the green runs above were taken at `up 32 mins` under load ~4. So "load" and "45 days
> of uptime" changed together, and this evidence cannot separate them. A 45-day-old host
> accumulates its own pathologies — memory pressure, thermal state, leaked file descriptors in
> long-lived daemons — any of which could starve QEMU as effectively as the build did. Whoever
> tests the load hypothesis should reload the machine *without* rebooting it first, or the
> experiment answers the wrong question.

**Symptom (as recorded 2026-07-27).** The gate reports:

```
fft_peak_bin=00000000        (expected 00000006)
AUDIO_CM4_DET=FAIL
GATE FAILED
```

The SAI path itself looks healthy in the same run — `sai_isr_count=0x400`, `dispatch_count=0x400`,
`underruns=0`, `rx_overflows=0`. ISRs fire and dispatch; the FFT simply sees silence.

The stored `transcript_qemu.txt` (committed 2026-07-22, `ffac1d6`) shows `fft_peak_bin=00000006` and
`AUDIO_CM4_DET=PASS`, so the gate did pass at some point.

### Platform

**It fails on macOS.** That is the only platform it has been run on here, so whether the failure is
macOS-specific or would reproduce elsewhere is **unknown and untested** — do not repeat "macOS bug"
as though it were diagnosed. If you ever run this tree on Linux, that single data point would be
genuinely valuable.

### What has already been ruled out — do not redo this

Bisected 2026-07-27, one variable at a time. **Every row below failed identically**, so none of them
is the cause:

| Variable | Value tested | Result |
|---|---|---|
| `cores` | `afff78a` — `BUS_CLK_ROOT` 240 MHz | FAIL |
| `cores` | `91a7efa` — `BUS_CLK_ROOT` 176 MHz | FAIL |
| `cores` | `189241c` — before any bus-clock change | FAIL |
| `qemu2` | `79a9990b39` reverted (ANADIG AI-gated STABLE + PGMC) | FAIL |
| `qemu2` | whole tree at `36d3be0e1f`, before **all 20** display commits | FAIL |

So it is **not** a bus-clock regression and **not** a QEMU-model regression. The ANADIG model is
exonerated. A hypothesis blaming `qemu2 79a9990b39` was tested and **refuted**, not assumed.

Full evidence: `docs/superpowers/plans/2026-07-27-m0-gate-sweep.md`.

### Remaining search space

1. `cores` commits from Jul 23–25 (ten, all display/PXP work — none obviously touching SAI).
2. The `Audio` or `CMSIS-DSP` sibling libraries.
3. **A stale transcript.** `ffac1d6` modified `CMakeLists.txt` *and* `transcript_qemu.txt` in the
   same commit, changing which pipeline the example builds by default ("default = working pre-arm
   pipeline; pure-CM4-PLL is the opt-in probe"). A transcript recorded against the *old* config
   would present exactly like this. **Untested — a hypothesis, not a finding.**

Cheapest next step: rebuild `cm4_audio_test` against `cores` at `ffd3547` (its last Jul-22 commit).
One build, one gate run, and it partitions the space into "cores Jul 23–25" vs "libraries or stale
transcript".

---

## SKIP-class gates — ✅ RESOLVED 2026-08-17, and the class is now empty

For one day this file carried two entries of a kind it had never had before:
`display/synthui_knob_test` and `display/vglite_probe` could not configure at
all on a fresh clone, because `SynthUI` and `VGLite` were unpushed and their
import macros raised `FATAL_ERROR`. No ELF meant the runner reported **SKIP
`(not built)`** rather than a failure.

**Both repos were pushed on 2026-08-17** (`newdigate/SynthUI`,
`newdigate/VGLite`, both public, both MIT), the `evkb.cmake` pins were bumped
and the FATAL_ERROR guards deleted. Verified the only way that means anything —
a `-DEVKB_FORCE_FETCH=ON` configure of both examples, which ignores the local
checkouts and fetches from GitHub. Both built, and the knob's five goldens came
back **bit-for-bit identical** from fetched sources
(`ALL=0x8E1F9956`, `ENDLESS=0xBF7FAB41`, `BOUNDED=0x7D77023E`,
`DETENTS=0xBEF7F8CE`, `ARC=0xAAE57894`).

★ **Keep the distinction this entry existed to draw, because the next
local-only library will re-create it.** A SKIP is invisible in the pass/fail
columns. Every other documented exception here *builds* and merely behaves
differently under QEMU, so it shows up RED, by name. A SKIP-class gate shows up
only in a count — and `0 SKIP` is exactly the number CLAUDE.md calls
load-bearing, because it is what says the sweep covered everything instead of
quietly measuring less. If you add a library that cannot be fetched, write its
entry here the same way, and say how many SKIPs a fresh clone should expect.

★ **SynthUI's history was rewritten before it went public** (dropping
`reference/rebirth/`, whose rights are unclear), so every SynthUI SHA before
`e132012` is unreachable. A pin restored from this repo's git history will not
fetch.

## `rt1176:display/vglite_probe` — QEMU gates the FALLBACK, not the GPU

★ **QEMU has no GC355 model.** This gate asserts that a chip-ID probe result
was printed at all, that it reported `VGLITE_INIT=ABSENT`, that
`VGLITE_TIMEOUTS=0`, and that the run reached `VGLITE_PROBE_DONE` — i.e. the
probe *detects* the GPU is absent, says so, and takes its software path without
hanging. (It matches the ID line by shape, `0x[0-9A-F]{8}`, not against the
`0x00000000` QEMU actually returns; the assertion is "the probe ran and
answered", and the ABSENT line carries `reason=no_chip_id`.)

That is a real assertion with teeth, and both of its failure modes were
observed while building it: `vg_lite_init()` **spins forever** on absent
hardware rather than returning an error, so a port that skipped the chip-ID
probe goes silent after `PANEL_OK`; and without qemu2's GPU2D window stub, even
*reading* the ID register raised an external abort and hung in an unhandled bus
fault. Either regression fails the gate by timing out with no DONE token.
`VGLITE_TIMEOUTS=0` carries its own weight: under QEMU no bounded wait should
ever run, so a non-zero count means the fallback path took a GPU branch it
should not have. The gate also proves the one-binary story — the same ELF runs
accelerated on silicon and falls back here.

But it proves the FALLBACK path. **It says nothing about whether the GPU
renders.** Do not read a green `vglite_probe` as "VGLite works".

**The GPU path is verified on silicon only**, in `transcript_hw_evkb.txt`:
`VGLITE_CHIP_ID=0x00000355`, `VGLITE_INIT=OK`, the nine-bit feature table,
`VGLITE_DRAW=OK`, `VGLITE_TIMEOUTS=0`, `VGLITE_IRQS=3`, the golden
`VGLITE_SUM=0x45465405`, and a human eye on a blue square on the RK055.

★ **Every one of those is necessary and none is sufficient alone**, which is
the lesson Phase 1 paid for. `VGLITE_DRAW=OK` and `VGLITE_TIMEOUTS=0` were both
reported while the GPU drew *nothing* — the bounded wait was consuming
interrupt flags left over from init. That is what `VGLITE_IRQS` exists for: a
wait that "succeeds" without the ISR count advancing did not succeed. And the
checksum can change while the panel stays black, because the driver was
resetting the display mix out from under LCDIFv2. Counters, checksum and glass
each catch what the others miss.

★ **When Phase 2 lands, the GPU and software paths will NOT produce identical
pixels** — hardware antialiasing differs from LVGL's mask arithmetic. Two
golden sets, never one. Do not "fix" a mismatch by copying one over the other.

This gate was SKIP-class on a fresh clone until 2026-08-17, when `VGLite` was
pushed public and the FATAL_ERROR guard came out — see the resolved SKIP-class
entry above. It builds from the pin now, verified with `-DEVKB_FORCE_FETCH=ON`.

## `rt1176:display/acid_box` — RED ON A FRESH CLONE BY DESIGN (local-only qemu2 dependency)

**Status on this machine: GREEN, measured 2026-08-18.** **Status on a fresh
clone: RED, and that is the licence firewall working, not a regression.**

**What the gate proves.** Every other display gate in this tree stops at the
framebuffer and every other audio gate stops at the samples; this one closes the
loop between them. A scripted finger presses ▶, then a step cell, then drags the
CUTOFF knob, and the assertion is that the per-step RMS table produced by the
**audio** graph changes at exactly the step the finger touched: step 2 is a rest
in the preset and reads silent (`< 0.005`) in a bar that completed *before* the
tap, and reads sounding (`> 0.02`) in a bar that *opened after* it. Nothing
short of touch → LVGL hit test → `cbStepTap` → `seq.step()` → the note pump →
the voice → the analyzer can move that number. A dead touch path, a UI that
repaints but writes no engine state, and a sequencer that ignores the write all
land here as a red gate, and none of them is visible to a pixel golden or to an
audio gate alone.

The windows are rigorous by construction rather than by timing assumption: a bar
line is emitted at the 15→0 seam after its whole window has been filled, so a
bar printed before the STEP token is entirely pre-edit, and the gate requires
**two** post-edit bars so the one it reads provably opened after the tap rather
than containing it. Bar 1 is excluded by name — the transport records boundaries
strictly inside `(from, to]`, so it never emits tick 0 at phase 0 and step 0
reads ~0.18 there against 0.42+ in every later bar. Asserting bar 1 would either
fail honestly or invite someone to lower the margin until it passed.

★ **IT DEPENDS ON A LOCAL-ONLY qemu2 CHANGE.** The injected gestures come from
the `touch-script` property on qemu2's `imxrt.gt911` model (qemu2
`a2d5d9dd16`), which is not upstream and is not pinned anywhere in this repo.
QEMU is GPL, so that change stays on this machine per the one-way firewall, and
a fresh clone therefore sees this gate red for a reason that has nothing to do
with the firmware. Same standing arrangement as the `sai1-rxinject` binding
(`audio/audioinput_i2s_test`, rt1062 half) and the rt1062 halves of
`usb/usb_descriptor_survey` and `dualcore/cm4_usb_irq_probe`. Do not "fix" it by
vendoring the QEMU patch, and do not delete the gate to make a clean-clone sweep
green — the red *is* the firewall reporting itself.

The gate fails loudly rather than vacuously when the script is missing: QEMU's
own refusal (`imxrt_gt911_load_script` → `error_report` + `exit(1)`) goes to
stderr, which `qrun` redirects into the `-D` log, so the operator's first
symptom would otherwise be "no UART capture" — indistinguishable from firmware
that never booted. The gate pre-flights the file by name and dumps the `-D` log
when the capture is empty.

★ **THE `-global` LONG FORM IS MANDATORY.** `qemu_global_option()` splits the
driver name at the FIRST dot, and the device type is `imxrt.gt911`, so
`-global imxrt.gt911.touch-script=FILE` parses as driver `imxrt`, property
`gt911.touch-script`, matches nothing, and prints **nothing**. A nonexistent
path passed that way boots happily and the run silently asserts against the
model's built-in script instead of ours. Measured during Task 6 of the capstone
plan. Never shorten the line in `run_qemu.sh`.

**QEMU renders in software, and silicon verification is still owed.** The UI
golden `0xD3BC88D7` is FNV-1a over the whole 720x1280 XRGB8888 framebuffer taken
from the LVGL **software** renderer — there is no GC355 model in QEMU (see the
`vglite_probe` entry above), so this frame says nothing about the GPU path. The
frame behind the golden was dumped with `pmemsave` from the QEMU monitor and
eyeballed rather than merely reproduced (layout, all eight knob boot angles, the
lane matching the preset cell-for-cell) — this tree does not record a golden for
a frame nobody has seen, because the knob pilot's clamped arc and the first
VGLite GPU frame were both perfectly reproducible AND visibly wrong. The
anti-golden `0x9BC99DC5` (FNV of 3686400 zero bytes) is rejected by name, and
the golden grep is anchored with `\r?$` because FNV-1a converges in its low bits
over a repeating 4-byte pattern: blank, X=0 and X=0xFF frames all end in `9DC5`,
so an unanchored match would accept the blank screen the assertion exists to
reject.

**Hardware run on the real EVKB is outstanding** (capstone plan Task 8, blocked
on physical access at the time of writing). Per this tree's two-gate rule a QEMU
pass is necessary but not sufficient, and that applies with full force here:
the codec, the panel and the GT911 are all modelled. Until
`transcript_hw_evkb.txt` exists beside the QEMU one, the honest claim is
"QEMU-verified", not "verified".

## Rules for this list

- **Do not delete, weaken, or `exit 0` a gate to get it off this list.** That defeats the entire
  point of having gates, and this project treats a gate that passes vacuously as a serious defect.
  The only way off this list is a fix.
- **Do not remove it from `tools/run-all-qemu-gates.sh`.** The runner should still know about it;
  this document is the convention that stops you *routinely* running it, not a mechanism that hides
  it. `./tools/run-all-qemu-gates.sh <pattern>` takes patterns, so run the gates you care about.
- **Any OTHER gate failing is a real regression** from whatever you are doing. Diagnose it; do not
  add it here to make a sweep green.
- When you fix one, delete its section and re-run the full sweep to confirm the count went up.

## Current expected sweep result

**2026-07-30 — gate count moved 68 → 70.** Two new LVGL RK055 gates joined the sweep
(`examples/display/lvgl_rk055_panel_test` and `examples/display/lvgl_rk055_touch_test`). The
expectation is now `70 passed, 0 failed, 0 SKIP` or `69 passed, 1 failed, 0 SKIP`, with the same
single permitted intermittent failure (`dualcore/cm4_audio_test`). The paragraphs below record the
68-gate history and still apply otherwise.

**2026-07-30 — gate count moved 70 → 71.** `examples/display/lvgl_rk055_flip_test` joined the
sweep (LVGL double buffering + page flip on vsync on the RK055). The expectation is now
`71 passed, 0 failed, 0 SKIP` or `70 passed, 1 failed, 0 SKIP`, same single permitted
intermittent failure (`dualcore/cm4_audio_test`).

**2026-07-30 — v5 (vsync ISR fence): gate count unchanged at 71.** No new gate. The flip gate
(`lvgl_rk055_flip_test`) gained a `VSYNC_ISRS=120` assertion (the flip fence is now driven by the
LCDIFv2 vsync interrupt rather than polling), and the touch gate (`lvgl_rk055_touch_test`)
migrated to double buffering — its render golden was re-recorded and came out value-unchanged.

**2026-07-30 — v6 (PXP sync copy): gate count moved 71 → 72.** `examples/display/lvgl_pxp_copy_bench`
joined the sweep. It asserts PXP-vs-CPU copy **correctness** (checksum match per rectangle case);
the timing table is hardware-only and lives in its `transcript_hw_evkb.txt`. The expectation is now
`72 passed, 0 failed, 0 SKIP` or `71 passed, 1 failed, 0 SKIP`, same single permitted intermittent
failure (`dualcore/cm4_audio_test`).

**68 gates** since `tools/run-all-qemu-gates.sh` discovery widened from `run_qemu.sh` to
`run_qemu*.sh` on 2026-07-29 (it had been sweeping 29 of them; the other 38, named for what they
test, were never run by anything) and `examples/display/rk055_touch_test` joined on 2026-07-28.

Two outcomes are acceptable while the section above is unresolved, because **`cm4_audio_test` is
intermittent, not reliably broken**:

- `67 passed, 1 failed, 0 SKIP` — the failure being `dualcore/cm4_audio_test`. Observed on
  2026-07-29 across five consecutive sweeps plus a direct run.
- `68 passed, 0 failed, 0 SKIP` — observed 2026-07-28, 5 runs out of 5, on an idle machine.

Same pinned `cores` (`2f15ff5`) in both cases, so it is not a core-version difference. That the
same gate can be consistently red one day and 5/5 green the next is itself the finding; treat the
section above as "intermittent, cause unidentified" rather than "broken".

**Any OTHER failure is a real regression.** If you see more than one failure, check `uptime` before
you check your diff: gates starved of CPU fail with missing or truncated UART capture, which mimics
a regression convincingly.

That failure mode is now *legible* rather than misleading — gates assert `gate_require_capture`
before grepping, so a starved run says `no UART capture ... QEMU produced no serial output` instead
of blaming the firmware with `banner missing`. The five thinnest-budget gates (`enet_test` at
`sleep 1`; `analog_test`, `dac_test`, `irq_attach_test`, `serial_test` at `sleep 3`) were converted
to bounded poll-for-token loops on 2026-07-29 for the same reason. That reduces the exposure; it has
not been shown to eliminate it, and the trigger is still unidentified — 2x CPU saturation did not
reproduce it.

**Zero SKIPs matters as much as the pass count.** A `SKIP` means a missing ELF, i.e. a gate that
silently never ran — build the example and re-run rather than accepting it.

**The sweep covers 67 gates, up from 29 (2026-07-29).** `run-all-qemu-gates.sh` used to discover
`run_qemu.sh` only, which silently excluded the 38 gates named for what they test
(`run_qemu_usb.sh`, `run_qemu_lwip.sh`, …) — over half the suite, never swept, so nothing was
checking whether they still passed. Discovery is now `run_qemu*.sh`. If you are comparing against
an older transcript, `28 passed` was that narrower set, not a regression.

**2026-07-31 (v7, XRGB8888):** sweep re-measured on branch `lvgl-xrgb-v7` after the
three RK055 LVGL examples migrated to XRGB8888 and the bench went dual-format
(`CASES=28`): still **72 gates** (no gate added or removed; the bench re-pinned
internally). The v7 bench's first silicon run exposed a QEMU PXP model divergence
(byte-preserving 32-bit copies vs. silicon's X:=computed-alpha-0) — fixed
stricter/more-faithful in qemu2 `0df62eb15a`; the bench oracle now asserts the
measured contract. Expectation unchanged: 72/0/0 or the documented
`cm4_audio_test` singleton, zero SKIPs.

**2026-07-31 (v8, PXP compositing):** `pxp_composite_test` joins the sweep: **72 → 73
gates**. Its oracle's blend/colorkey/ROP semantics are SILICON-MEASURED (the RM
contradicts itself twice; the transcript's RESOLVED AMBIGUITIES section is the
authority), and the QEMU AS datapath (qemu2 `4333c645ff`) was written FROM that
measurement — P2-before-P3, inverted deliberately after v7's X-byte finding.
Expectation: 73/0/0 or the documented `cm4_audio_test` singleton, zero SKIPs.

**2026-08-01 (v9, draw-unit economics):** `pxp_draw_bench` joins the sweep: **73 → 74
gates**. The milestone's verdict: draw-unit ADOPTION DECLINED — hardware showed fills
are CPU-won (write-streaming ~630 MB/s beats the PXP), and the census proved neither
LVGL scene generates image tasks, the op class where the PXP wins 12–63×. The bench +
census probe (-DDRAW_CENSUS) stand as the instrument; REVISIT when an image-heavy
scene (large images/sprites/photos) enters the tree. Expectation: 74/0/0 or the
documented `cm4_audio_test` singleton, zero SKIPs.

**2026-08-06 (USB audio input, Stage B):** `usb/usb_audio_capture_test` joins the
sweep: **74 → 75 gates**. Clean baseline re-measured on an idle machine with
`-j 2`: **75 passed, 0 failed, 0 SKIP**.

**2026-08-06 (USB audio duplex, Stage C):** `usb/usb_audio_duplex_test` joins the
sweep: **75 → 76 gates**. Measured at `-j 2`: **75 passed, 1 failed, 0 SKIP** —
the failure being the documented `dualcore/cm4_audio_test` singleton, which
compiles none of that session's sources (UAC2 parser, iTD pool, duplex example).
Expectation is therefore `76/0/0` or `75/1/0` with that one gate red, zero SKIPs
either way.

A counting trap worth one line, because it cost a puzzled minute: `-l` prints a
trailing `(N gate(s))` summary line, so `run-all-qemu-gates.sh -l | wc -l` is one
MORE than the gate count. The runner is still the right thing to ask — that
correction is above and stands — but ask it for the number it prints, not for
the number of lines it prints.

**2026-08-06 (emulated USB audio device):** `usb/usb_descriptor_survey` joins
the sweep: **76 → 77 gates**. First gate in this tree to run the USB audio
stack against an ACTUAL emulated audio device rather than an empty bus —
QEMU's `hw/usb/dev-audio.c` on the OTG2 host bus. It asserts real enumeration
(104-byte config retrieved), the interface walk, bcdADC, FORMAT_TYPE_I
(2 ch, 16-bit, 48000) and the endpoint's synchronisation type.

Two facts about that model, established by measurement and worth not
rediscovering:

- **It offers 48000 Hz only.** `USBAUDIO_SAMPLE_RATE` is a compile-time
  `#define` baked into its descriptor tables, with no property to change it.
  Every streaming example here ships asking for 44100 to match the Audio
  library, so they enumerate against it and are correctly refused by format.
  That is why the gate lives on `usb_descriptor_survey`, which never binds and
  is therefore format-agnostic.
- **Isochronous data does not flow against it.** An OUT sketch built for 48000
  enumerates, claims, selects alt 1, completes the whole control sequence with
  `ctrl=0/0/0`, prints "streaming started" — and sits at `pkts/s=0`. Cause not
  established; the leading suspicion is a split-transaction/TT mismatch, since
  siTD is for a full-speed device behind a high-speed hub and QEMU has this one
  on the root port. So this gate covers enumeration and the descriptor plane
  ONLY. Silicon remains the only proof that audio moves.

Two corrections to how this sweep gets counted and read, both learned the hard
way in that session:

**The gate count must come from the runner, not from `find`.** Discovery used to
descend into `build*` directories, where `-DEVKB_FORCE_FETCH` clones peripheral
libraries that ship gates of their own — four, under `usb_audio_graph_test/
build-fetch/_deps/` in the SPI and SdFat trees. A bare `find` returned 79, which
made the documented 74 look stale when it was exactly right, and the four extras
became permanent SKIPs for images this repo never builds. That last part is the
real harm: this file and CLAUDE.md both treat a non-zero SKIP as proof the sweep
under-reported, so a discovery bug that manufactures four of them permanently
disables the only integrity check on the count. Discovery now prunes `build*`
(`tools/run-all-qemu-gates.sh`); use `-l` to count.

**Load sensitivity is not confined to `cm4_audio_test`.** A `-j 4` sweep run
while the same machine was concurrently building examples, flashing the EVKB and
driving hardware came back `74 passed, 1 failed` — and the failure was
`dualcore/cm4_wire_int_slave_test`, never previously suspect, with
`cm4_audio_test` PASSING in that same run. The failing gate re-ran green in 1 s
on an idle machine and compiles none of the code that sweep was testing
(`err=00000004`, `WIRE_INT_SLAVE_CM4=FAIL` under load; clean idle).

So the rule "any failure other than `cm4_audio_test` is a real regression" is too
narrow as written: it is the DUAL-CORE gates as a class that are load-sensitive,
and which one shows it varies. Re-run a lone dual-core red on an idle machine
before believing it — and check whether the gate even compiles what you changed.
Neither of those excuses a red that survives both tests.

**2026-08-13 (Phase 5b — the RT1062 capstone):** two gate ids join the sweep:
**87 → 89**. `usb/usb_audio_capstone_test` is a new example and the tree's first
**rt1062-ONLY** gate owner — the 1060-EVKB has an on-board codec on the same
board as the USB host port, which the 1170-EVKB does not; the RT1176's capstone
is `dualcore/cm4_graph_usb_capstone`. `audio/audioinput_i2s_test` gains its
rt1062 half.

Expectation moves to **`89/0/0`**, or **`88/1/0`** when the nondeterministic
`rt1176:dualcore/cm4_audio_test` is red. Zero SKIP either way. Measured
2026-08-13 at `-j 2`, twice consecutively at load 4.6 then 4.8:
**`89 passed, 0 failed, 0 SKIP` both times**, `cm4_audio_test` green in both.

★ **A fresh clone sees `rt1062:audio/audioinput_i2s_test` RED**, and that is
expected rather than a regression: its rt1062 half needs the LOCAL-ONLY qemu2
change binding the `sai1-rxinject` chardev on `fsl-imxrt1062` (qemu2
`2141a5d781`) — the RX-side promise Phase 5a deferred — and qemu2 changes stay
on this machine per the GPL one-way firewall. Same situation as the tap,
`usb_descriptor_survey` and `cm4_usb_irq_probe`. The capstone gate needs only
`sai1-tap`, which Phase 5a already bound.

★ **What the capstone gate does NOT prove, and why its tap assertion is
inverted.** QEMU's `usb-audio` advertises exactly one sample rate — 48000
(`hw/usb/dev-audio.c:118`, `USBAUDIO_SAMPLE_RATE`) — while this graph runs at
44100 by construction, because `AudioOutputUSBHost` pins the OUT rate to the
graph rate deliberately (a mismatch there is a permanent 8.8% pitch error, not
a caveat). So `uac1_find_alt()` matches no alternate, `claim()` returns false,
and **the emulated device is never claimed at all**: `out=none` forever, no
`+ Audio` line. The gate therefore proves **graph plumbing, not USB** —
`check_tap.py --expect-silence` demands peak == 0 **AND** ≥128 KiB of tap,
because peak 0 alone is equally what a dead graph produces. An empty tap is
silent too.

★ **`out=none` is asserted as a TRIPWIRE, not as a limitation.** If the model
ever gains 44100 this gate goes red *deliberately*, and whoever sees that red
must re-read what the gate can prove rather than repair it. **Do NOT "fix" the
divergence by moving a rate**: a 48000 graph would ship a deliberate 8.8% pitch
error into the silicon path, and moving the model would break
`usb/usb_audio_uac1_test`, whose gate asserts `rates=48000` from QEMU's
descriptor as an external oracle. The rt1062 USB claim path is already gated by
that example, so re-proving it here would add no coverage.

★ **The unclaimed device hands the clocking design a free proof**, stronger
than any symbol inspection: with nothing claimed,
`AudioOutputUSBHost::frame_consumed()` never fires — yet the graph still
updated ~430×/guest-second for the whole run. Only the SAI TX DMA can be pacing
it. That is the declaration-order clock ownership (`AudioOutputI2S` declared
first wins `update_setup()`) verified by a run rather than by reading
constructor order.

★ **Counter rates are not assertable; direction is.** `in_under`/`out_drop`
climb at ~430/s against guest `millis()`, not the ~344.5/s (44100/128) that
arithmetic predicts — guest time runs fast against wall clock on this model.
Assert that a counter CLIMBS, never how fast. And `out_drop` climbing at the
**full graph rate is CORRECT in QEMU** (nothing drains the FIFO), flatly
contradicting the sketch's "one drop every seven minutes" comment — which is a
*silicon* prediction, for a claimed device whose USB frames drain the FIFO.
The gate must never assert a small `out_drop`.

**Three silicon-only defects this phase found, none of which QEMU could see.**
This is the phase's real yield; the gate is green with all three present.

1. ★ **`USBAudioOut::init()` did not reset its FIFOs** — only the attach paths
   did. Every sketch declares that driver `DMAMEM` (the EHCI cannot reach DTCM
   on RT1062), and `.bss.dma` is a separate NOLOAD section that the Teensy
   startup deliberately never clears (`memory_clear` covers `[_sbss,_ebss]`
   only), so `head`/`tail` began as stale OCRAM. It had never mattered because
   every earlier sketch drove writes from the attach path; the capstone is the
   first firmware whose **graph writes before a device attaches**
   (`AudioOutputUSBHost::update()` runs as soon as its `AudioConnection` makes
   it active). Symptom: MemManage fault ~8 s into every boot — banner, reset,
   forever. Diagnosed from the core's crash report at OCRAM `0x2027FF80` over
   the probe: IPSR=4, CFSR=0x82 (DACCVIOL|MMARVALID), MMFAR=`0x00FEC1A8`,
   PC=`0x2F7A` in `usb_audio_fifo_write`; working back with 32-bit wraparound
   gives head = `0x706F4A94`, i.e. garbage rather than a bad base pointer.
   QEMU zero-fills emulated RAM, so head/tail read 0 there **by accident**.
   Fixed in USBHost_t36 `928bfef`. ★ **Generalise it: any driver placed in
   DMAMEM whose invariants assume zero-init is broken, and QEMU will never tell
   you.**
2. **The sketch never armed the transport.** Selecting an alternate setting
   agrees a format but moves no audio: `beginStreaming()`/`beginRecording()`
   post the isochronous descriptors and `service()` advances the rings.
   Symptom: `out=ready in=ready` while both counters climbed at the full graph
   rate — which reads as a transport failure rather than a missing call.
3. **`AudioInputI2S` never enabled the SAI transmitter on the EVKB**, and RX is
   synchronous to TX on that board, so RX got no bit clock. QEMU's SAI model
   gates RX on the RCSR bits alone and passes either way, so the fix was
   argued, then **A/B-measured on silicon this phase**: without the TX enable
   `MIC peak=(no blocks)` ×30 (RX entirely dead); with it, blocks flowing ×56.
   Fixed in Audio `972f919`.

**The measured silicon numbers** (`transcript_hw_evkb.txt`, 130 consecutive
heartbeats): `out_drop` **delta 0** and `in_under` **delta 1** across 129 s —
one zero-filled block in over two minutes, better than `input_usbhost.h`'s
one-per-~34 s prediction from the bench device's −86 ppm. Both counters'
absolute values accrued *before* the arm. `in_peak=1.0000`: the bare
headphone-out-to-mic-in loopback clips, which is expected (a line output is
~two orders of magnitude hotter than a mic input expects) — still 1 kHz, still
audible, which is what the bar asks.

**One open loose end, recorded honestly:** `audioinput_i2s_test` on the
1060-EVKB shows a flat `MIC peak=0.0001` that does not respond to sound, so no
microphone signal reaches the WM8960's ADC with the driver's current input
routing on that board. That is separate from the clock fix above — the A/B
settles that independently — and does not affect its gate, which uses an
injected waveform.

**2026-08-13 (Phase 5a — RT1062 audio output):** `audio/audiooutput_i2s_test`
gains its rt1062 half: sweep **86 → 87**, again without a new example. First
time the Audio library compiles for `__IMXRT1062__` in this tree (including
`control_wm8960.cpp`, which no build here had ever compiled).

Expectation moves to **`87/0/0`**, or **`86/1/0`** when the nondeterministic
`rt1176:dualcore/cm4_audio_test` is red. Zero SKIP either way.

Measured 2026-08-13: **`87 passed, 0 failed, 0 SKIP`** serially — the runner's
default mode, run as two disjoint category halves (50 + 37) to fit a timeout —
with both halves of this gate green and `cm4_audio_test` green. At `-j 2` the
same tree measured `86/1/0` and, in an earlier run, `85/2/0`, and those reds
are a **harness collision, not firmware**: the two board halves of a two-board
example share their capture paths (`vcom.uart` and `tap.raw` here;
`serial.uart`, `string.uart`, `uac1.uart`, `survey.uart` elsewhere), each half
`rm -f`s them at gate start, and `-j 2` schedules the halves adjacently — so
the later half deletes the earlier half's live capture mid-run. The rt1176
half of THIS gate lost that race in 2 of 2 `-j 2` sweeps (its fixed `sleep 5`
keeps the halves aligned), and `rt1176:usb/usb_descriptor_survey` lost it in
one of them with the smoking gun in its gate output: a complete, correct
captured transcript followed by `grep: …survey.uart: No such file or
directory`. Every such red passed immediately when re-run alone at the same
load. So at `-j 2`, a red `rt1176:` half of a two-board example wearing "no
UART capture" was this collision until proven otherwise. The serial default
never collided.

★ **FIXED later the same day.** Per-run gate artifacts (captures, `-D` debug
logs, the audio tap) now go through gate-lib's `gate_capture_path`, which puts
them in the board's own build directory (`build/` vs `build-rt1062/`) —
per-board by construction, gitignored, and guaranteed to exist for any gate
the runner starts, since that is where the ELF lives. All five two-board gates
converted; `gate-lib.test.sh` and `gate-vacuity.test.sh` pass unchanged (the
vacuity fake QEMU takes its capture path from the `-serial` argument, so it
never cared where the file lived). Verified with two consecutive full `-j 2`
sweeps: **`87 passed, 0 failed, 0 SKIP` both times**, the second starting at
load 8.9, `cm4_audio_test` green in both. ★ Any per-run file a future gate
creates must go through the helper — a bare `"$DIR/<name>"` recreates this bug
for the next two-board example.

A later single-run serial sweep the same day measured `86/1/0`, the red being
one half of this gate in the OTHER known mode — the fixed `sleep 5`'s load
sensitivity, under load ~7–8.6 from concurrent review tooling. Its tap was
healthy (`tap_peak=16383`, `STAGE_TONE=PASS`); only the console token was
missing. Green on idle re-run at load 5.4, and the same flake had already
shown once that day on a single-gate run at load 5.7. Two observations in one
day says the spec's deferred fix — poll the tap for a sample count instead of
sleeping — is worth its follow-up task; a longer sleep is NOT that fix.

★ **FIXED: the gate now polls both liveness signals instead of sleeping.** It
waits until the VCOM holds a `STAGE_SYNTH=` verdict (PASS or FAIL — liveness,
not the assertion) AND the tap has ≥128 KiB, capped at 60 s, breaking early if
QEMU dies. Both conditions in that order because the token arrives BEFORE the
tap finishes accumulating — token-only polling would reap early and starve
`check_tap.py`, which is why the fixed sleep was left in place during Phase 5a
rather than half-fixed. Verified: both halves pass in 3 s (the old sleep made
every pass 5–6 s); a dead QEMU fails by name in ~1 s instead of eating the
cap; and a live QEMU that prints the token but never feeds the tap caps at
60 s then fails as `STAGE_TONE`, not as a vacuous pass.

★ **A fresh clone sees `rt1062:audio/audiooutput_i2s_test` RED**, and that is
expected rather than a regression: its rt1062 half needs the LOCAL-ONLY qemu2
change binding the `sai1-tap` on `fsl-imxrt1062` (qemu2 `6d98ec3b27`), and
qemu2 changes stay on this machine per the GPL one-way firewall. Same
situation as `usb_descriptor_survey` and `cm4_usb_irq_probe`.

★ **`STAGE_TONE` is amplitude-only** — it asserts peak > 4000 in the tap, no
frequency analysis — so it proves non-silent samples reached SAI1's TDR and
nothing about pitch or rate. Silicon carries audibility: the committed
`transcript_hw_evkb.txt` records the tone confirmed by ear on 2026-08-13.

The codecs differ by chip — WM8962 on the 1170-EVKB, WM8960 on the 1060-EVKB —
and the QEMU models are not equally faithful: the 1060 machine has a real
`TYPE_WM8960`, the 1170 a stub that ACKs writes and returns 0 on reads. Do not
assume codec parity between the two halves of this gate.

★★ **THE WEDGE this phase found and fixed, recorded here because any
teensy4-core rt1062 image was one relink away from it.** The rt1062 half
originally produced NO UART output ever in QEMU while the tap filled with
zeros. Root cause: `cores/teensy4` `startup.c` programs a 32-byte NOACCESS MPU
region at address 0x0 (the NULL-pointer trap), and QEMU's PMSAv7 walk
(`target/arm/ptw.c`) refuses to TLB-cache any 1 KB page an MPU region
partially covers — so code linked into ITCM 0x020–0x3FF executes as fresh,
never-cached, single-instruction translation blocks, roughly 1000× slow. This
image happened to link its sine-update loop at 0x3ae: measured ~150k TB
translations/s with the host near 100% in the translator, and thread mode
advanced `delay(5)` by about 1 ms per 5 s of wall clock. The kill-test that
confirmed it: disabling only MPU region 2 over the gdbstub revived the wedged
image in seconds. The fix is a 1 KB code hole at ITCM 0x0 in
`cores/teensy4/imxrt1060_evkb.ld` (cores `5bcae78`, pinned by evkb `c4fdd10`)
— silicon-true, with the MPU region, the QEMU model and the gate all
untouched. **Which image trips this is a link-order lottery**, and it presents
as "no UART capture" — indistinguishable from firmware that never started.

Two consequences of that hole worth knowing:

- It can tick `_itcm_block_count` (FlexRAM banks come in 32 KB granules) and
  silently move a bank from DTCM to ITCM. It DID for `usb_audio_uac1_test`
  (`_estack` 0x20078000 → 0x20070000). The layout is self-consistent by
  construction; that image was re-flashed and re-measured on 2026-08-13 — 45
  consecutive heartbeats at `pkts/s=1000`, identical to the old layout
  (addendum in its `transcript_hw_evkb.txt`).
- The upstream-style scripts `imxrt1062.ld` / `imxrt1062_t41.ld` /
  `imxrt1062_mm.ld` still carry the 32-byte hole, so a Teensy 4.x image booted
  on `mimxrt1060-evk` via `tools/rt1170-qemu.sh` can still lose the same
  lottery. Deliberate — they are upstream Teensy targets this tree does not
  gate — and this entry is the breadcrumb.

Licence audit: **PASS**, with the new build directory walked —
`audiooutput_i2s_test` 123 dep paths, `audiooutput_i2s_test/build-rt1062` 160.
The drift check cannot notice a missing `build-rt1062:` GATES entry (it keys
on example directories, and no `run_qemu*.sh` corresponds to a build
directory), so the entry was verified by name in the audit output.

**2026-08-08 (Phase 4 — UAC1 host on two boards):** `usb/usb_audio_uac1_test`
gains a gate AND a second board in one move: sweep **84 → 86**, two gate ids
(`rt1176:` and `rt1062:`) from a single new `run_qemu.sh`. It had **no** gate at
all before — there was no UAC device in QEMU for the host stack to enumerate
until the emulated `usb-audio` device came into use, so there was nothing to
assert. Measured `86 passed, 0 failed, 0 SKIP` at `-j 2` on a machine at load
6.3, with `rt1176:dualcore/cm4_audio_test` green in that run.

Expectation moves to **`86/0/0`**, or **`85/1/0`** when the nondeterministic
`cm4_audio_test` is red. Zero SKIP either way. The permitted reds are
**unchanged** — this gate is green on both boards.

★ **The gate proves the CONTROL PLANE only, and that is deliberate.** It asserts
enumeration, descriptor parse, interface/alt-setting selection and endpoint
setup. It does **not** assert `SITD PASS` or `pkts/s > 0`, because QEMU never
runs the isochronous descriptor: the iTD sits with `active=1`, `bytes_left`
unchanged and no error flags, forever. `SITD FAIL` is therefore the EXPECTED
output under QEMU. Anyone who "fixes" that by asserting `SITD PASS` will make
the gate permanently red for no reason at all — the firmware is fine and the
model is what is missing.

Silicon carried the rest, and the evidence is committed at
`examples/usb/usb_audio_uac1_test/transcript_hw_evkb.txt`: a GeneralPlus
`1B3F:2008` UAC1 adapter in J47, an audible 1 kHz tone, and `pkts/s=1000`
sustained for **29 consecutive seconds** (`total` 10976 → 38976). 1000 pkts/s is
the theoretical maximum — one packet per 1 ms USB frame — so that is 28000
packets with **none** dropped.

★ **That measurement closed an open question rather than merely passing.** The
board-axis design §1.4 predicted the RT1062 has *less* stall headroom than the
RT1176: the stall ceiling is wall-clock, and at 600 MHz everything between
service points takes 1.66× longer. Phase 3 then left **all** of DMAMEM uncached
on this board, adding a second cost on top. Neither is observable — the transfer
ring is serviced perfectly, at the frame rate, for half a minute. `pkts/s`
remains the detector if some future workload ever starves it; the fix then would
be a dedicated non-cached `USBHOST_DMAMEM` section, **not** reverting the
uncached mapping.

**2026-08-08 (task #74 — String coverage on the second board):**
`framework/string_test` gains its rt1062 half: sweep **83 → 84**, again without
a new example. It passed first time, all 21 assertion groups including
`CTOR_NUM` (which asserts `String(255, HEX) == "ff"`).

Coverage, not a bug hunt: `EVKB_BOARD=rt1062` links `cores/teensy4`, which has
its **own** `WString.cpp` and `nonstd.c`. Other rt1062 builds already *compile*
them — `serial_test`'s `libcores.o.a` has 102 symbols from `WString.cpp` alone —
but nothing **asserted** their behaviour, and compiled is not tested. That gap
mattered more once the board axis turned that core from a reference copy into
something built for real.

Worth recording: the first rt1062 run failed with an **empty capture**, because
the runner still had a bare `-serial file:`. That is the documented LPUART6 trap
and it is worth having seen once — the firmware ran perfectly and printed to
LPUART6, which nothing was bound to, so the failure is indistinguishable from an
image that never started. Fixed by taking the chain from `gate_console` (then
named `gate_serial1`), which is what every two-board gate must do.

Superseded within the day: `gate_serial1` put rt1062 in the SIXTH `-serial` slot
because those sketches printed to `Serial1` = LPUART6 on `cores/teensy4`. That
is the wrong UART on this board — LPUART6 reaches Arduino pins D0/D1, not the
DAPLink VCOM — so the gate and the bench were reading different wires. The
rt1062 sketches now use `Serial6` (== LPUART1, the VCOM), and `gate_console`
emits a plain `-serial file:` for both boards.

Expectation moves to **`84/0/0`**, or **`83/1/0`** when the nondeterministic
`cm4_audio_test` is red. `rt1062:usb/usb_descriptor_survey` went green later the
same day (see its section at the top), so **there is once again only ONE
permitted red**, and it is the dual-core intermittent.

**2026-08-08 (Phase 2 — RT1062 USB host in QEMU):** `usb/usb_descriptor_survey`
gains its rt1062 half: sweep **82 → 83**, again without a new example.

Expectation is **`83/0/0`**, or **`82/1/0`** when
`rt1176:dualcore/cm4_audio_test` is red — which is nondeterministic and not
reliably predicted by machine load (see its table below). Zero SKIP either way.
(Superseded by the 84-gate entry above; kept for the Phase 2 history.)

There are now **two** permitted reds, and they are different in kind — do not
conflate them:

- `rt1062:usb/usb_descriptor_survey` — **red by design, every run, any load.**
  Its own section at the top of this file explains why and what would close it.
  A sweep where this one is GREEN means someone changed the model; go find out
  who and why before celebrating.
- `rt1176:dualcore/cm4_audio_test` — the load-sensitive intermittent, and the
  threshold is sharper than previously recorded (see the table below).

With two permitted reds, **read the gate names in the summary rather than
trusting the count** — `81/2/0` with the wrong two gates red is not a pass.

Measured 2026-08-08 at `-j 2`: **`81 passed, 2 failed, 0 SKIP`** — exactly those
two. `cm4_audio_test` failed in its documented form (`fft_peak_bin` wrong,
`AUDIO_CM4_DET=FAIL`, while `codec_ack=1` and `cm7_audio_isers=0` passed).

Six readings were taken on 2026-08-08. **Do not read a load threshold into
them — one was drawn and it did not survive the next measurement.**

| run | 1-min load | result |
|-----|-----------:|--------|
| in sweep, `-j 2` | 6.8 at start | FAIL |
| individual | 5.5 | FAIL |
| individual | 4.1 | FAIL |
| individual | 3.4 | **PASS** (6 s) |
| individual | 3.4 | **PASS** |
| in sweep, `-j 2` | **8.6 at start** | **PASS** |

The first four readings fell in order and looked exactly like a threshold near
4, and that conclusion was written down here and then falsified within the hour
by the last row: a full sweep starting at load **8.6** — higher than the one
that failed — passed this gate. So load correlates loosely at best, and the
honest description is **nondeterministic**.

What survives: a single red here means nothing on its own, re-run it, and
**four consecutive readings are not a trend either**. What does not survive:
any claim about a specific load number, including the 2026-07-28 entry's
"~4–5" and the threshold this table used to assert.

It was never a Phase 2 regression regardless: this phase's two qemu2 commits
touch only `hw/arm/fsl-imxrt1062.c`, `hw/misc/imxrt1060_anatop.c` and
`include/hw/arm/fsl-imxrt1062.h`, and `cm4_audio_test` has no `boards` sidecar,
so it runs `-M mimxrt1170-evk` — a model none of those files build.

Licence audit: **PASS**, with both rt1062 build directories walked
(`serial_test/build-rt1062` 136 dep paths, `usb_descriptor_survey/build-rt1062`
203). `tools/license-audit.test.sh`: 20/20. The ★★ block above is closed.

Two things worth not rediscovering:

- **The EHCI host was never missing from the 1062 model.** `TYPE_CHIPIDEA`
  derives from `TYPE_SYS_BUS_EHCI` and is shared with the 1170. Only the wiring
  on top was absent. USBHost_t36's README claimed otherwise and was corrected.
- **`-d unimp` did NOT find this phase's bug, and would not have.** The RT1062
  hang was `hw/misc/imxrt1060_anatop.c` modelling the `SET`/`CLR`/`TOG` alias
  words as ordinary storage instead of alias ports onto the base register, so
  every alias write vanished and `ehci.cpp`'s `PLL_USB2` loop spun forever. The
  registers are all *implemented*, so `unimp` printed nothing — the debug log
  was three benign lines. What found it was attaching gdb to QEMU's gdbstub
  (`-s`) and reading the PC, then reading `0x400D8020` and `0x400D8024` live.
  **The tell was in the UART, not the log:** the firmware printed its banner and
  then no 2-second heartbeat at all. That is a hang in `setup()`, not a failure
  to enumerate, and the two look identical if you only read the first line.

**2026-08-08 (the board axis):** `serial/serial_test` becomes the first example
gated on two boards, so the sweep moves **81 → 82** without a new example: the
same gate script runs once for `rt1176` and once for `rt1062`
(MIMXRT1060-EVKB). Gate ids now carry a board prefix.

Expectation is `82/0/0` or `81/1/0` with the documented
`rt1176:dualcore/cm4_audio_test` singleton, zero SKIP either way.

Measured 2026-08-08 at `-j 2` on a machine at load ~4: **`81 passed, 1 failed,
0 SKIP`**, the failure being `rt1176:dualcore/cm4_audio_test`
(`fft_peak_bin` wrong, `AUDIO_CM4_DET=FAIL`) — the documented intermittent, in
its documented form. Both board variants of `serial/serial_test` passed
(`rt1176` 1 s, `rt1062` 2 s).

Three notes on reading this sweep:

- **A red on `rt1062:` and green on `rt1176:` for the same example is not a
  flake** — it is the board axis doing its job, and it means the 1060 build
  genuinely differs.
- **The SKIP signal is now per board**: an example that declares `rt1062` but
  has no `build-rt1062/*.elf` is a SKIP, and the runner prints the exact
  `cmake` line to fix it.
- ★ **`rt1062:serial/serial_test` is RED on a fresh clone**, and that is
  expected rather than a regression. Getting it green needed a qemu2 change
  (`c850405bf9`, wiring `TYPE_IMXRT_SEMC` into the RT1062 SoC, which had
  modelled `semc-ctrl` as an unimplemented stub), and qemu2 changes stay local
  to this machine per the GPL one-way firewall. Same situation as Phase 7.1's
  IRQ-135 split. Without it the Teensy core's unconditional SDRAM bring-up
  polls a done bit that never arrives — 1,000,000 reads of INTR offset `0x3c`,
  bounded but far past any gate timeout, so the capture stays empty.

★★ **The licence audit did NOT pass on this branch when the board axis landed,
and that blocked the phase from closing. RESOLVED the same day — see the Phase 2
entry below.** `EVKB_BOARD=rt1062` links `cores/teensy4`, whose `WString.cpp`,
`IPAddress.cpp`, `Stream.cpp`, `WMath.cpp` and `Time.cpp` were LGPL-2.1.
`tools/license-audit.sh` allows `cores/teensy4/` only while its objects define
no symbols — true for as long as that core was reference-only. Once compiled,
the audit reported `DUAL-LICENSED SOURCE NOT EMPTY` five times and exited 1.
Nothing LGPL ever reached `serial_test.elf` (`--gc-sections` dropped all five),
but that was a property of that example, not of the board. It was a licence
decision, not a harness fix, and it was taken the only acceptable way: the five
were **replaced** with the MIT clean-room versions (`cores` `99f7657`).

**2026-08-07 (the capstone: an audio graph on the CM4 feeding its own USB
stream):** `dualcore/cm4_graph_usb_capstone` joins the sweep: **80 → 81
gates**. Phase 7.4 — the CM4 image adds `AudioStream.cpp`, the Audio library's
`AudioOutputUSBHost` adapter, a sine source and a peak analyser on top of 7.3's
whole USB stack, and the driver's built-in tone generator is gone: the graph is
the only producer. No qemu2 change was needed.

★★ **This gate PASSES while printing TWO FAIL tokens — `STREAM_PACKETS=FAIL`
AND `GRAPH_CLOCKED=FAIL` — and both are correct.** The first is 7.3's, unchanged
(iso data does not flow against QEMU's `usb-audio`). The second is **new, and it
is a consequence of the first**: `AudioOutputUSBHost` makes the USB frame clock
the graph's clock, via `USBAudioOut::onFrameConsumed()`, which the driver calls
once per ring slot it arms. No packets ⇒ no callbacks ⇒ the graph is never
ticked. A graph that is never clocked is not a graph that cannot run.

**So the graph is proved SEPARATELY, and that separation is the design of this
gate rather than a concession.** After `beginStreaming()` the CM4 image pends
`IRQ_SOFTWARE` itself until the FIFO reaches the adapter's own setpoint — the
real engine, the real sine, the real adapter, the real FIFO, with only the tick
supplied locally, and gated on exactly the `queued() < FIFO_TARGET_SAMPLES`
condition `frame_consumed()` applies. It is pure CPU, so it works in both
worlds, and it is what `GRAPH_ALIVE` / `GRAPH_AUDIO` / `GRAPH_NOLEAK` assert.
It also earns its keep on silicon: `beginStreaming()` resets the FIFO and arms
32 slots from it, so without the priming step the first frames of every measured
window underrun while the graph catches up and `unders` — the number that says
whether the graph kept the stream fed — would carry a startup artefact in every
run.

Four world-split tokens here, not two. `TRANSPORT_CLEAN` and the new
`STREAM_FED` (`unders == 0`) both read PASS in QEMU **vacuously**: nothing was
transmitted, so there can be no transmission error, and nothing was consumed, so
the producer cannot have failed to keep up. Presence-checked, never
value-asserted, for the same reason `TRANSPORT_CLEAN` already was.

Two further notes, both of which cost time before they were understood:

- ★★ **`gcyc`/`gcycmax`/`cpu`/`lps` are meaningless in QEMU, and the meaningless
  value LOOKS ALARMING.** These are the numbers Phase 7.4 exists to produce —
  the worst contiguous graph pass, against the CM7-measured knee of 600 µs clean
  / 850 µs fail. qemu2 derives the CM4's DWT CYCCNT from the virtual clock
  scaled by the modelled 400 MHz core and models no M4 pipeline, so a "cycle"
  count there is host emulation time. Consecutive runs of this gate reported
  worst = **776 µs** and then **2111 µs** for identical code, and the `dwt=`
  spin measured 66400 cycles against 7.3's 28800 for the same 200-iteration
  loop. The firmware therefore prints a four-line caveat next to them. Compare
  them with the knee **only** from a silicon transcript; the one legitimate
  in-world use is this gate's `lps` against 7.3's `lps` (67299) as a rough
  instruction-count ratio.
- ★ **An unguarded `AudioAnalyzePeak::read()` reports FULL SCALE when it has
  analysed nothing.** Its reset state is min = +32767, max = −32768 and `read()`
  returns max(|min|,|max|)/32767, so a starved tap reads 1.0 — the starved case
  and the healthy case are indistinguishable and the starved one looks *better*.
  Caught in the first QEMU run, where the streaming window analyses nothing and
  `peak2` came back 32768. The firmware now guards both reads on `available()`
  and sends `PEAK_NONE` (0xFFFFFFFF) instead, so "no blocks arrived" and "blocks
  arrived carrying silence" are different values and different messages.

Two negative controls are checked in. `transcript_qemu_red_vector.txt`: vector
index 60 (IRQ 44, `IRQ_SOFTWARE`) repointed at `Default_Handler`, nothing else
changed — the CM4 reaches `s7=armed` and then hangs, because `Default_Handler`
spins and the first `NVIC_SET_PENDING` never returns, so a misrouted graph
vector is visibly a HANG exactly as a misrouted USB vector was in 7.2.
`transcript_qemu_red_silent.txt`: `GRAPH_AMPLITUDE` set to 0, nothing else
changed — ★ `GRAPH_ALIVE` still **passes**, with `blocks=3` and `fifo=768/768`
identical to the green run, because the adapter really did fill the FIFO, with
silence. Only the peak tap separates them. That is the argument for having a
peak node in the graph at all, and it is why `GRAPH_ALIVE` alone would have been
a gate that a completely silent stream could satisfy.

**2026-08-07 (the CM4 arms an isochronous stream):**
`dualcore/cm4_usb_audio_probe` joins the sweep: **79 → 80 gates**. Phase 7.3 —
the CM4 image links the UAC class driver on top of 7.2's transport core, claims
a USB audio device, negotiates a format, completes the post-claim control
sequence and arms 32 siTDs across all 32 periodic frame slots. No qemu2 change
was needed.

★★ **This gate PASSES while printing `STREAM_PACKETS=FAIL`, and that is
correct.** It is the direct, deliberate consequence of the emulated-device
finding recorded two entries below: **isochronous data does not flow against
QEMU's `usb-audio`**. Everything up to and including "streaming armed" is
green; `pkts` then stays at 0 forever. So packet flow is a **world-split
token** — the firmware prints the verdict, the QEMU gate asserts only that the
token is PRESENT, and SILICON asserts it is PASS. `AUDIO_CM4`, the verdict the
gate does assert, is the **control-plane** verdict and stops at "armed" on
purpose. Precedent: 7.1's `PHY_PLL_CM4`, 3.2's `rdv`, 2C's `systick`.

Do not "strengthen" this gate by asserting `pkts > 0`. That is the same mistake
7.1 made when it asserted `PHY_PLL_CM4=PASS` against a PHY model with no
`PLL_SIC` register — a gate rendered unpassable for a reason with nothing to do
with what it was testing. It has now cost this phase twice.

Three further notes:

- **`TRANSPORT_CLEAN` is presence-checked for the opposite reason.** It reads
  PASS in QEMU, but vacuously: a stream that transmitted nothing cannot have had
  a transmission error. A check that cannot fail is worse than no check, so the
  value assertion belongs to silicon there too.
- ★ **`queued` is what localises the QEMU stall.** It sits at 3840 of 4096
  samples, which says the driver's tone generator is producing into the real
  streaming FIFO and saturating it because nothing drains. The producer half
  works; it is the transport that does not move. `unders=0` agrees.
- **`lps` (service-loop rate) is a characterisation token in BOTH worlds and is
  asserted by neither.** In QEMU it measures the emulator, not the M4 — CYCCNT
  comes from the virtual clock scaled by the modelled 400 MHz CM4 clock, so
  `millis()` tracks wall-clock while the loop runs at TCG speed. It is printed
  because the CM7 budget measurement
  (`usb_audio_duplex_test/transcript_hw_cpu_budget.txt`) established that the
  binding constraint is **contiguous stall length** (600 µs clean, 850 µs fail),
  not throughput — so `1/lps` on silicon is the number that matters, and it
  should be visible rather than assumed.

A negative control is checked in as `transcript_qemu_red_rate.txt`: the same
firmware built with `-DPROBE_RATE_HZ=44100u` and nothing else changed. QEMU's
`usb-audio` offers 48000 only, `uac1_find_alt()` refuses, `claim()` returns
false, and the gate goes red at `DEVICE_CLAIMED` — with `ctrl_timeouts=0`,
which is the distinction the stage-6 packing exists for (a format the device
never offered leaves the watchdog silent; a device that stalled the request
leaves it climbing).

One build note, because it links libgcc where no CM4 image did before: the audio
driver divides 64-bit values by runtime denominators, GCC lowers that to
`__aeabi_uldivmod`/`__aeabi_ldivmod`, and `teensy_add_cm4_image` links
`-nostdlib`. The fix is `GROUP(-lgcc)` in **this image's own `cm4.ld`**, not a
new argument to the shared macro — a per-image need must not change any other
image's command line (the 2B `cmp` discipline). Licence-wise this is the
compiler runtime under the GCC Runtime Library Exception, which
`tools/license-audit.sh` already permits and every CM7 image already links.

**2026-08-07 (the real stack enumerates on the CM4):**
`dualcore/cm4_usb_enum_probe` joins the sweep: **78 → 79 gates**. Phase 7.2c —
the CM4 image links the actual USBHost_t36 transport core, calls
`USBHost::begin()`, and reports the attached device's VID/PID. **No qemu2
change was needed**: 7.1's IRQ-135 split is the only model work this arc
required, so unlike 7.1 there is no red-first-then-model-change story here.

The negative control is checked in instead, as `transcript_qemu_red_vector.txt`:
vector index 151 repointed at `Default_Handler` and nothing else touched. The
CM4 reaches stage 3, takes the interrupt, and spins in the default handler — the
transcript truncates with `STAGES=FAIL` and no VID/PID. Worth keeping because it
shows the shape a misrouted vector actually has: a **hang**, not a quiet zero.
Two more notes on this gate:

- **`vid`/`pid` are the whole oracle, and only the GATE asserts them.** Every
  other token could in principle be produced by firmware talking to itself;
  `46F4:0002` is QEMU's `usb-audio` (`hw/usb/dev-audio.c:43-44`) and the CM4
  image has no knowledge of those numbers. The firmware asserts only
  `vid != 0`, deliberately, so the identical image serves a silicon run with
  whatever device is in J47 — the device-specific oracle belongs to whoever
  knows which device is attached.
- ★ **`claims` is the interrupt assertion, which is why there is no `irqcnt`
  token.** `claim()` is reachable only via `claim_drivers` ←
  `enumeration_receive` ← `followup_Transfer`, and that runs inside
  `USBHost::isr()`. There is no polled path to it.

**2026-08-07 (CM4 takes the USB port):** `dualcore/cm4_usb_irq_probe` joins the
sweep: **77 → 78 gates**. Phase 7.1 — the CM4 self-configures LPCG115, the
USBPHY2 480 MHz PLL, EHCI reset, host mode and port power, then takes USB
OTG2's **IRQ 135** on its own NVIC. HW-verified on the EVKB across two runs.
Needed a qemu2 change (`7c9bd4cbe6`, local-only per the GPL firewall): a
`TYPE_SPLIT_IRQ` fanning 135 to both NVICs, since the model wired both USB
IRQs to the CM7 only. Run **red-first**, with `transcript_qemu_red.txt`
checked in before that change.

Three things about this gate worth not rediscovering:

- **`PHY_PLL_CM4` is a world-split token: FAIL in QEMU, PASS on silicon, and
  the gate asserts only that the token is PRESENT.** qemu2 instantiates stock
  `TYPE_IMX_USBPHY`, an i.MX6-era model whose register file ends at
  `USBPHY_VERSION` (0x80); the RT1176's `PLL_SIC` quad at 0xA0/A4/A8/AC is
  unmodelled, so the lock poll cannot succeed for *either* core. Pre-existing
  and already recorded in HW-verified firmware — `USBHost_t36/ehci.cpp:248`
  literally reads "QEMU: PLL_SIC reads 0 -> times out, proceeds". Do not
  "fix" this by asserting lock in the gate.

- ★★**The device must be HOTPLUGGED, not present at reset — and the first two
  attempts at this gate were VACUOUS in two different ways.** ChipIdea defers
  attach to the guest's PP write (`chipidea.c:230` → `hcd-ehci.c:1066`), so a
  present-from-reset device raises PCD during the firmware's *stage 5* and
  the stage-6 "clear stale status" W1C wipes it before PCE is armed. The
  first red therefore had `irqcnt=0` with `stsraw=0` — no interrupt condition
  ever occurred while anything was listening, so the red said nothing about
  routing and the qemu2 split would not have flipped it. The second attempt
  polled for the firmware's `s6` marker before hotplugging — but the CM7 was
  batching all its mailbox reads before printing, so `s6` only reached the
  UART *after* the window closed and the plug again landed too late, with the
  identical symptom. The gate now streams each token as it arrives and
  hotplugs on `s6`. **`irqcnt=0` is the legitimate negative result here, so
  almost every possible mistake wears its costume** — that is why the gate
  asserts `PLUG_TRANSITION` (a real ccs 0→1 edge) *before* it asserts the
  interrupt, and why the firmware reports a post-window raw `USBSTS`.

- **A delay loop calibrated by counting instructions was off by ~2.3×.** The
  CM4's observation window was written as a `volatile` decrement loop with a
  "~3 cycles/iter" estimate; it is nearer 7 (LDR+SUBS+STR+CMP+B), so a
  nominal 8 s window ran ~18.7 s and collided with the CM7's receive timeout.
  The transcript truncated mid-sequence, which reads as a CM4 hang rather
  than a miscalibrated delay. Delays are now measured with DWT CYCCNT.

## `rt1176:display/vglite_lvgl_test` — software-path gate; the GPU build has its own golden and it lives on silicon

The example is ONE scene (a 4×4 `synthui_knob` grid) with TWO builds:
`build/` (software renderer — what this QEMU gate runs, golden
`0x513C4DB8`) and `build-vglite/` (LVGL's VG_LITE draw unit on the GC355 —
silicon only, golden `0xC3C6171A` in `transcript_hw_evkb.txt`, verified by
framebuffer dump over SWD).

★ **Two golden sets, never reconciled.** Hardware antialiasing is not
LVGL's mask arithmetic, and the GPU build carries `LV_USE_FLOAT=1`, so the
two paths legitimately differ (measured: 0.40% of pixels, all at AA edges,
mean |ΔRGB| 0.79). Copying either golden over the other would destroy the
only evidence that the GPU renders differently at all. When one moves,
re-verify THAT path on its own terms; do not "fix" the other to match.

★ **A green QEMU gate here proves the software path and the one-source
story — nothing about the GPU.** The GPU build is not even a runtime
fallback of the gated ELF (LVGL registers its draw units at compile time;
see the example header): it is a different binary, verified only on the
board. The path split is build-time by design, and the gate's value is
pinning the software renderer while GPU work churns the shared source.

The phase's fps criterion (≥30 fps for the animating grid) was measured
2026-08-17 and NOT met — software 2.83 fps, GPU 2.45 fps, GPU CPU-bound in
per-task path construction (transcript, Task 9 section). The gate is
correctness-only; no gate asserts a frame rate.
