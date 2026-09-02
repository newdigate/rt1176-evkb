# GC355 / VGLite — behaviour reference

What the Vivante **GC355 GPU2D** on the i.MX RT1176 actually does, as opposed
to what NXP's VGLite driver *reports* it did. One row per feature: **verdict ·
safe usage · evidence**.

> **The discipline: every row cites the conformance case id that establishes
> it.** A row with no case id is visibly a claim without evidence — that is the
> point of the column, not a formality. Every GC355 defect this tree has hit
> shares one property: the driver returned `VG_LITE_SUCCESS`, the error
> counters read zero, and the picture was wrong. Nothing in the API surface
> distinguishes a working feature from a broken one, so the only currency here
> is a probe case that looked at pixels.

## ✅ SILICON STATUS: **MEASURED 2026-09-02** (Phase 2 boot; supersedes 2026-08-30)

Transcripts: `examples/display/vglite_conformance/transcript_hw_evkb.txt` is
**boot 2**, the one `expected_silicon.txt` is checked against;
`transcript_hw_evkb-boot1.txt` is the first boot, committed so the
two-boot claims below can be checked rather than taken on trust.
VGLite pin **`2e17773`** (`evkb.cmake`), Series `gc355/0x0_1216`,
`vgc_chip_id=0x00000355`, target 128×128 BGRA8888, tessellation 64×64,
`vgc_timeouts=0 vgc_irqs=111`.

★ **The pin is recorded because the checker's whole drift rationale is "the
driver moved under us".** Attributing a future red to an SDK re-vendor or a
pin bump should not need git archaeology.

Result: **`cases=20 ok=15 broken=5 repeat_differs=2`**.

**TWO BOOTS, EVERY VERDICT IDENTICAL.** Three *detail* numbers move between
them, all in cases already known to vary — `two-disjoint-bars` fill 1322/1316
(a BROKEN case whose stray pixels are the misparse), `four-nested-rings`
6875/6931 and `evenodd` `nzfill` 6421/6423 (both `repeat=differs`). Notably
`eofill` agrees **exactly**: the nondeterminism lives in the no-hole pass (★ RETRACTED as a general claim 2026-09-02: a fourth boot read the hole-cutting pass at `short:459` after three boots at `short:308` — BOTH passes vary boot to boot; the hole-cutting one had merely not varied yet).
That distinction matters because this tree has seen a GC355 defect hide behind
exactly one boot (NEW-20's winding-2 track, ten boots and ten checksums), so a
single-boot GPU result is not trusted here.

Verify a fresh transcript against the pre-registered expectation with
`tools/vglite-conformance-check.sh`; it fails on drift in **either** direction.

★★ **BUT KNOW WHAT THE CHECKER CANNOT SEE.** It compares only
`<id> <pixel> <repeat>`. A case that returns `ok` under more than one
behaviour is invisible to it. Phase 2's colour cases were built that way
deliberately — before the boot, the driver's header supported two readings of
`SRC_OVER` — and were **pinned to the measured reading afterwards** precisely
so a flip reddens the checker instead of passing silently. If you ever widen a
case to admit two answers, it stops being drift-protected the moment you do.

**Three Phase 1 predictions were WRONG**: two VERDICT predictions
(`path/two-contour-ring-nonzero` and `path/evenodd-vs-nonzero`, both predicted
`broken`, measured `ok`) and one REPEAT prediction (`path/evenodd-vs-nonzero`,
predicted `same`, measured `differs`).
★ **`path/evenodd-vs-nonzero` has since moved AGAIN** — Phase 2 gave its
EVEN_ODD pass a coverage check for the first time and it is now `broken`
(`eocover=short:308`). Its Phase 1 `ok` was on the strength of the other pass
alone.

**Phase 2 is built and measured** (five colour/blend cases, §"Colour & blend").
Phases 3 and 4 have no cases; their sections are marked accordingly and their
rows are claims carried over from working code, not probe results.

## Re-running it

★ **Flash the `.hex`, not the `.elf`.** `LinkServer flash … load` REFUSES this
example's ELF — `Flash operation exited with code -11`, 0 of 38060 bytes written,
4/4 reproducible — while other images flash clean either side of it. It is
LinkServer's ELF program-header path, not the board; the failure presents as a
dead board, which is why it is here in the recipe.

```sh
cd examples/display/vglite_conformance
cmake --build build
# flash, press SW4 once, capture -> transcript_hw_evkb.txt
../../../tools/vglite-conformance-check.sh transcript_hw_evkb.txt
```

The checker **fails on drift in EITHER direction**. A quirk that quietly
*disappears* matters as much as one that appears: it means the driver moved
under us (an SDK re-vendor, a `Series/<chip>/<rev>` switch, a pin bump), and
the safe-usage rules that two shipping compositors are built on may no longer
be describing the machine. The fix for a red is never `cp transcript expected`
— see that file's header for the obligation each direction carries.

## Where the pieces are

| Piece | Path |
|---|---|
| The probe (20 cases — 15 path, 5 colour/blend — plus 1 opt-in dangerous) | `examples/display/vglite_conformance/` |
| Design spec | `docs/superpowers/specs/2026-08-30-gc355-conformance-design.md` |
| Pre-registered expectation | `examples/display/vglite_conformance/expected_silicon.txt` |
| Drift checker | `tools/vglite-conformance-check.sh` |
| The PRIOR question — does the GPU init and render **at all** | `examples/display/vglite_probe/` |
| Vendored driver | `~/Development/VGLite` (pinned in `evkb.cmake`) |

`display/vglite_probe` answers a question this document assumes has already
been answered: whether `vg_lite_init()` succeeds and the GC355 puts a shape on
glass. Nothing below is meaningful if that one is red. (Its QEMU gate asserts
the GPU-**absent** fallback — QEMU has no GC355 model — so the rendering half
is silicon-only, in its `transcript_hw_evkb.txt`.)

The probe's own QEMU gate (`examples/display/vglite_conformance/run_qemu.sh`)
is likewise a GPU-absent gate: it asserts `vgc_engine=absent` and that every
case reports `pixel=skip`. It proves the harness runs and cannot invent a
verdict; it says nothing about the GC355.

---

## Paths, contours & winding — Phase 1

**Verdicts below are MEASURED** (2026-09-02, two boots, every verdict identical). Where a
cell says *Prediction refuted*, the pre-registered expectation was wrong and
`expected_silicon.txt` carries the reason for the change.

| Feature | Verdict | Safe usage | Case |
|---|---|---|---|
| single-contour filled path | **OK** — `fill=6400`, exactly the analytic area | the only path shape to rely on | `path/single-contour-rect` |
| four DISJOINT contours in ONE path, ordinary CLOSE | **BROKEN** — `runs=1` of 4, `fill=1393` (one bar plus antialiasing) | **one contour per path, one `vg_lite_draw` per contour** | `path/multi-contour-disjoint` |
| the same, with CLOSE slots padded `0x01010101` | **OK** — `runs=4`, `fill=5120`. **The encoding is the only variable.** | see [the discriminator](#the-discriminator-a-padded-close-slot) below | `path/multi-contour-close-padded` |
| two DISJOINT contours in one path, ordinary CLOSE | **BROKEN** — `runs=1` of 2. Two fail exactly as four do | **one contour per path**, or pad the CLOSE slot | `path/two-disjoint-bars` |
| four NESTED contours in one path, ordinary CLOSE | **BROKEN** — structure right (`runs=4`, all four honoured) but **~1150 STRAY PIXELS** (6931/6875 vs an analytic 5760), varying between boots | do **not** use it | `path/four-nested-rings` |
| hole cut by a reversed inner contour (non-zero) | **BROKEN** — sample points say the hole is right (`rim=1 centre=0`) but `fill=4607` vs an analytic 5376: **`cover=short:769`, 14 % of the ring missing** | don't; draw a filled plate then an inset plate in the backdrop colour — its control measures EXACT | `path/two-contour-ring-nonzero`, control `path/two-draws-ring` |
| `VG_LITE_FILL_EVEN_ODD` vs `NON_ZERO` across nested contours | **BROKEN** — both rules honoured (`eoc=0 nzc=1`) but the EVEN_ODD pass, which **cuts a hole**, is `eocover=short:308`. `repeat=differs` | do not cut holes in one path — see the hole-cutting note | `path/evenodd-vs-nonzero` |
| fill rules on ONE self-intersecting contour | **OK** — pentagram centre empty under EVEN_ODD, filled under NON_ZERO | both rules honoured — this is the fill-rule usage that works | `path/self-intersecting` |
| path coordinate formats S8 / S16 / S32 / FP32 | **OK** — all four `fill=1830`, and `same_px=1`: the four renders are BIT-IDENTICAL, not merely equal in area | all four usable; **the opcode is one BYTE at the base of a format-width slot**, not a value of the format's type, so each format needs its own typed array | `path/format-s8`, `path/format-s16`, `path/format-s32`, `path/format-fp32`, agreement: `path/format-agreement` |
| degenerate (zero-area) geometry | **OK** — `fill=0`, nothing drawn | safe to emit; nothing, or a hairline on the degenerate row — do not rely on which | `path/degenerate-zero-area` |
| a path with no `VLC_OP_END` | **UNPROBED in the default build** | never emit one; refuse a truncated path rather than drawing it | `path/unterminated` — opt-in `-DVGC_DANGEROUS=ON` only |

Notes on three of those rows:

* **`path/format-agreement` is not a fifth format case.** The four formats must
  agree *with each other*, not merely each be plausible alone — four
  identically-wrong renders satisfy the four rows above and fail only here.
* **`path/two-draws-ring` is the safe-usage control**, not a probe: the same
  ring drawn as plate + inset plate in two single-contour draws. That is what
  both shipping compositors already do, so a broken verdict there would mean
  the ground under SynthUI moved.
* **`path/unterminated` is excluded from the default build on purpose.** A
  path with no `VLC_OP_END` may run the front end off the end of the buffer;
  the case exists so the outcome is *recorded* if someone chooses to look, not
  so it rides every boot.

### The discriminator: a padded CLOSE slot — **ANSWERED 2026-08-30**

**The padded encoding renders; the ordinary one does not.**

```
path/multi-contour-disjoint      pixel=broken   runs=1, expect=4, fill=1393
path/multi-contour-close-padded  pixel=ok       runs=4, expect=4, fill=5120
```

Identical geometry, identical predicate, identical column; the *only*
difference is that the second pads its contour-boundary `VLC_OP_CLOSE` slots to
`0x01010101`. `expected_silicon.txt` records this as the pair's
`narrow-rule-a-zero-padded-CLOSE-slot-terminates-the-path` outcome, and both
boots produced it.

The background that made this experiment worth running follows.

`path/multi-contour-disjoint` and `path/multi-contour-close-padded` are the
two arms of **one experiment**: identical geometry, identical predicate,
identical column — the *only* variable is how the contour-boundary `VLC_OP_CLOSE`
is encoded.

**Why the padding is interesting.** NXP's own driver carries a workaround for
this exact chip. `~/Development/VGLite/VGLite/vg_lite_path.c:556-570`, inside
`vg_lite_append_path()`, guarded by `#if (CHIPID == 0x355)`, special-cases
**precisely a CLOSE followed by a MOVE** —

```c
if ((i < seg_count) && cmd[i] == VLC_OP_CLOSE &&
    (cmd[i + 1] == VLC_OP_MOVE || cmd[i + 1] == VLC_OP_MOVE_REL)) {
    ...
    else if (data_size == 4) {
        *(uint32_t*)(pathc + offset) = 0x01010101;
```

— i.e. **exactly at a contour boundary** — and writes the CLOSE as
`0x01010101` across the whole element instead of as a single byte.

`CHIPID` **is** `0x355` for the Series this tree selects: `evkb.cmake` sets
`EVKB_VGLITE_SERIES` to `gc355/0x0_1216`, and
`VGLite/Series/gc355/0x0_1216/vg_lite_options.h:31` defines `CHIPID 0x355`. So
the vendor's workaround is compiled in — it is simply never reached.

**Why it has never been exercised here.** An opcode occupies one byte at the
base of its slot, so every path this tree builds writes CLOSE as a bare
`int32_t` and its slot reads `01 00 00 00`. `VLC_OP_END` is `0x00`. **Every
contour boundary this tree has ever emitted therefore carries three END bytes
inside the CLOSE slot** — a complete and mundane mechanism for "the path
stopped after the first contour", and the one the vendor gates on this chip.

Nothing in this tree calls `vg_lite_append_path()`. Grepped: the only other
hits are LVGL's ThorVG shim (`LVGL/lvgl/src/others/vg_lite_tvg/`), which
*implements* the API for a PC simulator and is not built here
(`LV_USE_VG_LITE_THORVG` is 0). So the one-contour rule was measured on the
**unpadded encoding only**.

**The two admissible outcomes, and what each would mean:**

| Joint outcome | Meaning |
|---|---|
| `disjoint` broken, `close-padded` broken | The wide rule — *the GC355 renders only a path's first contour* — is confirmed on a **second** encoding and is that much stronger. |
| `disjoint` broken, `close-padded` **OK** | The real rule is far **narrower**: *a zero-padded CLOSE slot terminates the path*. Both shipping compositors (`synthui_rotary_knob_gpu.cpp`, `synthui_fader_gpu.cpp`) could then **drop their one-contour workarounds** by changing an encoding. |

`expected_silicon.txt` expresses this pair as **ONE answer with two admissible
joint outcomes** (`pair contour-encoding …`), so the good result does not
arrive as a red gate — a plain expected `broken` would have the checker
punishing the discovery it was built to catch. `disjoint=broken` is pinned in
*both* tuples, so the mechanism cannot silence drift on that arm.

> **Keep following one-contour-per-path** (that was true before the boot and
> is still the guidance after it — see *What to do meanwhile* below). It is the
> conservative reading of the current evidence and it is known to work — it is
> what both compositors ship.

---

### What IS and IS NOT established

**Settled.** The CLOSE encoding matters, decisively — see the numbers above.
Four disjoint bars in one path render as one contour with the ordinary
`01 00 00 00` slot and as all four with `01 01 01 01`.

**Refuted.** *"This GC355 renders only the FIRST CONTOUR of a path"*, **as
stated**, is wrong. Two nested-contour paths using the **ordinary** CLOSE
encoding rendered **both** contours correctly — the non-zero ring cut its hole
(`rim=1 centre=0`) and the same-winding nest honoured both fill rules
(`eoc=0 nzc=1`). A truncate-at-the-first-CLOSE story explains the
four bars and explains **neither** of these, because both carry the same
CLOSE-then-MOVE boundary.

**ANSWERED, 2026-09-01, two boots — DISJOINTNESS is the variable.**

|  | 2 contours | 4 contours |
|---|---|---|
| **disjoint** | **BROKEN** (structure, `runs=1`) | **BROKEN** (structure, `runs=1`) |
| **nested** | **BROKEN** (`cover=short:769`) | **BROKEN** (`cover=stray:1171`) |

Two disjoint contours fail exactly as four do; four nested contours work
exactly as two do. The rule is neither *"only the first contour renders"* nor
*"more than two contours breaks"* — it is **DISJOINT contours in one path,
under the ordinary zero-padded CLOSE encoding**. Padding the slot to
`0x01010101` fixes the disjoint case.

### ★★ But the second boot changed the conclusion

`path/four-nested-rings` read `repeat=same` (fill 6931) on boot 1 and
**`repeat=differs`** (fill 6875) on boot 2. **One boot would have recorded that
nested contours render cleanly.** They render *correctly* — `runs=4` every time
— but **not deterministically**, and the nondeterminism is itself intermittent.
Two of the three nested cases in this matrix now show it (`evenodd-vs-nonzero`
has `repeat=differs` on all four boots on record).

**So this result does NOT license nested multi-contour paths.** A structurally
correct but nondeterministic path is unsafe for a delta-rendering compositor —
which is exactly how NEW-20's winding-2 track defect presented. Both
compositors keep one-contour-per-path, now for two independent reasons.

`expected_silicon.txt` records that cell's repeat as **`unstable`**, a third
state the checker accepts only on `repeat` (never on a pixel verdict), only
with a written reason, and always while *printing which way the run landed* —
so nothing is hidden. Pinning `same` would red half the future runs and
pinning `differs` the other half; either teaches a reader to ignore the
checker.

### ★★★ With coverage checked, all four cells of the 2×2 are BROKEN

`pixel=ok` used to mean *the structure is right*. Now it means *the picture is
right*, and that changed the Phase 1b conclusion:

|  | 2 contours | 4 contours |
|---|---|---|
| **disjoint** | **BROKEN** (structure, `runs=1`) | **BROKEN** (structure, `runs=1`) |
| **nested** | **BROKEN** (`cover=short:769`) | **BROKEN** (`cover=stray:1171`) |

*"Nested is OK"* was an artefact of checking only structure. Two of the three
nested cases pass their sample-point predicates while drawing the wrong number
of pixels — **in opposite directions**: two nested contours draw 769 **too few**,
four draw ~1150 **too many**.

**The one construction that measures exactly is `path/two-draws-ring`** — the
same ring geometry, built as two single-contour paths and two draws, `fill=5376`
exact on both boots, sitting beside a single-path version of the identical ring
that is 769 px short. So one-contour-per-path is no longer a conservative guess
that happened to work: it is **directly measured against its own counterexample**,
and it is what both compositors already do.

★★★ **CONFIRMED 2026-09-02: HOLE CUTTING is the variable, not nesting.**
Phase 2 closed the gap that could test it — `evenodd`'s pass 1 *is* an EVEN_ODD
hole and had never been coverage-checked. It came back `eocover=short:308`,
**identical on both boots**, while pass 2 (same winding, no hole) is exact.

| render | cuts a hole? | coverage |
|---|---|---|
| `two-contour-ring-nonzero` (opposite winding) | yes | `short:769` |
| `evenodd` pass 1 (EVEN_ODD) | yes | **`short:308`** |
| `four-nested-rings` (alternating) | yes | `stray:1171` |
| `evenodd` pass 2 (same winding) | **no** | `ok` |

**Every hole-cutting render mis-covers; the one that does not is exact.** Note
also that the two boots disagree on `nzfill` (6421 vs 6423) and agree **exactly**
on `eofill` — the nondeterminism lives in the no-hole pass (★ RETRACTED as a general claim 2026-09-02: a fourth boot read the hole-cutting pass at `short:459` after three boots at `short:308` — BOTH passes vary boot to boot; the hole-cutting one had merely not varied yet), not the
hole-cutting one.

### ★★ Stray coverage — what the fill numbers say, and why `pixel=ok` got stricter

Comparing measured `fill` against exact analytic area across both boots:

| case | shape | analytic | silicon | excess |
|---|---|---|---|---|
| `single-contour-rect` | 1 axis-aligned rect | 6400 | **6400** | **0** |
| `multi-contour-close-padded` | 4 bars, padded CLOSE | 5120 | **5120** | **0** |
| `multi-contour-disjoint` | 4 bars, ordinary CLOSE | 1280 (bar 0) | 1393 | **+113** |
| `two-disjoint-bars` | 2 bars, ordinary CLOSE | 1280 (bar 0) | 1322 | **+42** |
| `four-nested-rings` | 4 nested rects | 5760 | 6931 / 6875 | **+1171 / +1115** |

The first two are controls and they are **exact** — this pipeline costs zero
antialiasing on axis-aligned integer rects. (A diagonal does cost: the triangle
measures 1830 against 1800.) So those excesses are **stray geometry — pixels
drawn that are not in the path** — and **the excess scales with how much path
data follows the first CLOSE**.

**That is not truncation.** A path that stopped at the zero-padded CLOSE would
measure exactly 1280. It is the parser **continuing and misreading**, which
also explains the nondeterminism: a desynchronised parse reads whatever is in
memory.

Consequently `pixel=ok` now requires the structural predicate **and** fill
within tolerance of the analytic area — so it means *the picture is right*, not
merely *the structure is right*. Four cases that reported no fill at all now do.

★ **Tolerance is `k × perimeter`, never a percentage of area.** Rasterisation
error lives on the boundary. A 5 % area band would have put a false `broken` on
`path/self-intersecting` — a *control* — because a pentagram carries 474 px of
all-diagonal boundary on only 2792 px of area. Axis-aligned `k=1/8`,
antialiased `k=1/2`, the latter from the one measured rate (the triangle's +30
is all on its hypotenuse, so 30/84.85 = 0.354 px per unit of diagonal).

★ The host suite gained a **fourth arm** — a stray-ink rasteriser drawing the
right shape plus 400 px that are not in the path. Without it the coverage
check's failing branch was executed by nothing, so the whole check could have
been hard-wired to pass with arms 1–3 still green. Demonstrated: hard-wiring it
leaves arms 1–3 green and fails 14 cases by name in arm 4.

### Still open

The **mechanism**. "Disjoint" describes the geometry; it does not explain why
the tessellator drops it. One concrete clue: bar 0 renders **1393 px inside
the four-bar path but 1322 px inside the two-bar path** — the same 80×16 bar,
71 px apart. The two paths differ only in their bounding box, from which the
driver derives its tessellation window, so the tile grid and its edge
antialiasing differ. Plausible; not established.

Both cells were **pre-registered before the boot** (`two-disjoint-bars = broken`,
`four-nested-rings = ok`) and both verdicts held. Only the `repeat` prediction
was wrong, and that is the finding above. They were deliberately NOT a `pair:`:
all four combinations were coherent, and a pair admitting every combination of
its members would have made any result green and the experiment worthless.

**What to do meanwhile.** Keep following one-contour-per-path in
`synthui_rotary_knob_gpu.cpp` and `synthui_fader_gpu.cpp`. It is the
conservative reading, it is known to work, and nothing above licenses relaxing
it — the padded-CLOSE result says a *different* construction also works, not
that the current one is unnecessary.

### Nondeterminism: `path/evenodd-vs-nonzero`

The only case in the matrix reporting `repeat=differs`. Its two **identical**
back-to-back renders produce different pixels — and they do so
**reproducibly**: byte-identical on both boots. So this is not boot-to-boot
noise but a repeatable in-boot difference between two identical draw
sequences, which is a stranger and more tractable thing.

It matters even though the pixels are *right*: a nondeterministic path is
unsafe to build a delta-rendering compositor on, which is exactly how NEW-20's
winding-2 track defect presented before it was found. Both shipping
compositors guard against this class with a delta-equality check run per boot;
nothing here suggests relaxing that.

## Known, but not yet probed

Carried over from the port's and the compositors' `★` comments. Each is real,
each was paid for on silicon, and **none has a conformance case yet** — which
is exactly what the Case column is for.

| Quirk | What it looks like | Source | Case |
|---|---|---|---|
| Command buffers must be **64-byte aligned** | The GPU starts executing and **hangs in the front end**. Every driver call still returns `VG_LITE_SUCCESS`; no completion interrupt, no pixels. Measured before the fix: allocations at `…0x470` (`% 64 == 48`), and after a submit `AQHiIdle` (0x004) read `0x7FFFFFFE` — bit 0, the Front End, busy forever. | `VGLite/port/baremetal/vg_lite_hal.c:95-110` (the allocator header is padded to the alignment for this reason) | **(none)** — a Phase 1 case would have to *provoke a hang* to prove it, which is not a thing a one-boot matrix can recover from. Structurally prevented in the port instead. |
| The target buffer must be `vg_lite_map`'d | **Every draw "succeeds" and changes nothing.** | `SynthUI/src/vglite/synthui_rotary_knob_gpu.cpp:398-403` (measured by `vglite_probe`) | **(none)** — it is a precondition of the whole harness: an unmapped target would make *every* case read broken, so it cannot be isolated as a row. |
| A wait could consume a **stale IRQ flag** | The ISR OR-accumulates, so a late or extra interrupt from an earlier submission leaves a flag a later wait consumes instantly — the driver "finishes" a frame while the GPU is still executing. Presented as **per-boot-varying checksums** (ten boots, ten grid sums) with `rk_gpu_err=0` throughout. | **Fixed** in the port (VGLite `2e17773`): the wait now requires the flag **AND** `AQHiIdle` bit 0 clear. `VGLite/port/baremetal/vg_lite_os.c:138-168` | **(none, by design)** — the probe cannot re-create the defect, but it **witnesses the fix** on every boot via the `vgc_timeouts=` / `vgc_irqs=` line. A non-zero `vgc_timeouts` means a wait gave up rather than lied. |

---

## Colour & blend — Phase 2

### ✅ MEASURED 2026-09-02, two boots — every verdict identical

**Why this phase, and not the gradients the original spec listed:** every one of
the fifteen Phase 1 cases renders with `VG_LITE_BLEND_NONE`, while **both
shipping compositors use `VG_LITE_BLEND_SRC_OVER` exclusively** — twelve call
sites. The matrix had never tested the blend mode production uses.

| Feature | Verdict | Safe usage | Case |
|---|---|---|---|
| memory word order of a `BGRA8888` target | **OK** — `px=0xFFFF0000`, red is byte 2 | ARGB in memory; `vg_lite_color_t` is ABGR. Confuse them and the colour is wrong while every status says success | `color/solid-word-order` |
| `SRC_OVER` source term over black | **OK — reading B**, `v=255 a=255` | see below: it is the **premultiplied** operator | `color/premultiplied-srcover` |
| `SRC_OVER` over a non-zero backdrop | **OK — reading B**, and it **agrees with the case above** | pass a **premultiplied** source | `blend/srcover-arithmetic` |
| `SRC_OVER` self-consistency across two composites | **OK** — `v1=255 v2=255 pred=255` | the operator is self-consistent | `blend/srcover-double` |
| `BLEND_NONE` with a non-opaque source | **OK** — `v=255 a=128`, source written **raw**, alpha row `A: Sa` | behaves exactly as documented; all fifteen Phase 1 cases are unaffected | `blend/none-honours-alpha` |

### ★★★ `SRC_OVER` is the PREMULTIPLIED operator here — and the FADER feeds it non-premultiplied colour

Both cases report **reading B** and **they agree**, which was the consistency
requirement. So the hardware implements `S + D*(1-Sa)` — exactly what
`inc/vg_lite.h:461` literally says, despite `:458` filing mode 1 under
*"Non-premultiplied Blending modes"*. `a=255` rules out alpha-ignoring.

**This is actionable, not academic — for ONE of the two compositors.**
`synthui_fader_gpu.cpp`'s `abgr_a()` packs an unscaled RGB with a separate alpha
and hands it straight to `SRC_OVER`:

```
abgr_a(0x1B1F22u, 115u)           // a shadow
abgr_a(0xFFFFFFu, pal->gloss_opa) // a gloss highlight
abgr_a(color, opa)                // the tick runs
```

Under `S + D*(1-Sa)` the source contributes at **full** intensity whatever its
alpha, so those draws do not produce the intended blend — a white gloss at
partial opacity saturates rather than reading as a sheen.

★ **`synthui_rotary_knob_gpu.cpp` is NOT affected**, and the difference is
structural rather than luck: it has no `abgr_a` at all. Every colour it draws
goes through `abgr(hex)`, which forces `0xFF000000` — **always opaque**. At
α=255, `S + D*(1-Sa)` reduces to `S`, which is correct under either reading.

★ **FIXED 2026-09-02** (SynthUI `d995e63`), one line in `abgr_a`: premultiply
RGB by `a/255` before packing. Measured on silicon over **two boots × four
render passes, all eight bit-identical** — `fd_crc` `0x141D0A41` →
`0x814F4047`, `delta==fresh` `0xE929A5E4` → `0xE9A9A2B5`, `fd_delta_eq=PASS`
on every pass. `fd_damage` and `mfps_med=30448` did not move: the cost is
three multiplies per **draw call**, not per pixel.

Two details worth carrying to any similar fix:

* **Scope is provably three draws.** Eight of the ten `abgr_a` call sites pass
  `a=255`, and `(v*a + 127)/255` is exact at both rails, so those are
  bit-identical rather than merely close — checked exhaustively, not argued.
* **It was fixed in the COLOUR, not by switching to blend mode 11.** Mode 1's
  behaviour is MEASURED on this silicon; mode 11's is not, and this header's
  blend naming is demonstrably unreliable. Do not "simplify" it to the
  untested mode.

★ **Nothing in this tree's gates can see this code**, which is why the fix
ships with a host test (SynthUI `tests/fader_color_test.c`, 69017 checks,
demonstrated RED against three mutants) rather than a gate: the QEMU gate runs
the **software** engine, and the GPU goldens live only in a hand-pressed
hardware transcript.

★ **A sensitivity limit worth knowing.** Under reading B a *saturated white*
source clamps to 255 in cases 2–4, so their colour-channel tolerances are doing
nothing and cannot separate reading B from other behaviours that also saturate.
**The alpha row carries the discrimination** (`a=255` vs `128`). A non-saturated
source would make the colour channel informative again — the obvious next case,
not a defect in these.

### ★★ The driver's own header is inconsistent about whether `SRC_OVER` is premultiplied

Found while building the reference rasteriser. From `~/Development/VGLite/inc/vg_lite.h`:

- `:452` — "S and D represent source and destination **non-premultiplied** RGB color channels"
- `:458` — section heading "**Non-premultiplied** Blending modes"
- `:461` — `VG_LITE_BLEND_SRC_OVER = 1` → `RGB: S + D*(1 - Sa)` — **no `*Sa`**, the *premultiplied* operator
- `:481` — `VG_LITE_BLEND_NORMAL_LVGL = 11` → `RGB: S*Sa + D*(1 - Sa)` — the *non*-premultiplied operator
- `:137` — `#define VG_LITE_BLEND_PREMULTIPLY_SRC_OVER VG_LITE_BLEND_NORMAL_LVGL`

**Names and formulas are inverted against each other.** So two readings of mode 1
are both defensible, and the cases report which rather than pre-judging:

| | reading **A** = `S*Sa + D*(1-Sa)` | reading **B** = `S + D*(1-Sa)` |
|---|---|---|
| over black | 128 | 255 |
| over grey 0x40 | 160 | 287 → clamps to 255 |

★ **Cases 2 and 3 must agree on which.** A disagreement means the hardware
implements neither formula consistently — a bigger finding than which it is.

★ **The alpha row is the one part with no ambiguity**, and it is what catches a
GPU that discards alpha: `:462` gives `A: Sa + Da*(1-Sa)`, which over an opaque
backdrop is 255 under **both** readings, while alpha-ignoring leaves 128. With a
saturated white source, reading B is otherwise *observationally identical* to
writing the source raw — so without the alpha check those cases could not tell
a conforming GPU from one that ignores alpha at all. `blend/none-honours-alpha`
deliberately does **not** judge alpha: `BLEND_NONE`'s row is `A: Sa`
(`:459-460`), so a raw write leaving 128 is correct there.

### The one entry this phase retires rather than confirms

The Phase 1 quirk table lists *"SRC_OVER of AA paths is not idempotent —
double-composited edges drift"*. **That is arithmetically correct compositing**
— a 50 %-coverage edge composited twice gives `0.75s + 0.25d` — and true of
every conforming implementation. A case confirming "twice ≠ once" would confirm
nothing about this GPU. `blend/srcover-double` instead asserts the second
composite lands where the same formula predicts **from the measured first**,
which holds under either reading.

## Gradients — linear, MEASURED

### ✅ PROBED 2026-09-02 — six cases, two boots, every line byte-identical across them.

Spec: `docs/superpowers/specs/2026-09-02-gc355-conformance-gradients-design.md`.
Cases in `vgc_cases_grad.cpp`; host suite `tests/cases_grad_test.cpp` (191
checks, six arms). Radial was deliberately left out: no consumer needs it.

**The central fact, read from the driver and then confirmed in pixels:**
`vg_lite_draw_linear_grad` computes the gradient's per-pixel parameter from
`grad->matrix` **alone** and applies `path_matrix` **only to the geometry** —
the two are never composed. `vg_lite_update_linear_grad` (`vg_lite.c:7690-7710`)
transforms the line into **screen space** and overwrites both `grad->matrix`
and `grad->linear_grad`. So an EXT gradient does not follow a moved path.

| Feature | Verdict | Safe usage | Evidence |
|---|---|---|---|
| **EXT linear gradient, static** | **ok** | Set the line in screen coordinates, `update`, draw. | `grad/ext-linear-static` — `l=240 m=126 r=11`, within one unit of the model |
| **EXT gradient under a moved path** | **broken** | *Never* move a gradient by `path_matrix` alone: the ramp stays at its old screen position (left sample reads 189 where red is ≥200). | `grad/ext-linear-moved` |
| **Re-calling `update` after a move** | **broken — and idempotent** | Does *not* fix the move (profile identical to the cell above) and **leaks one ramp image** per call: the driver never frees the previous one. Do not "just update again". | `grad/ext-linear-reupdate`, `leak=1`; the prediction changed from "double transform" to "idempotent" *before* the boot, by algebra, and held |
| **Clear + set at the new position + update** | **ok** | **The prescribed usage**: re-specify the gradient line in screen space per placement. What a moving widget must do. | `grad/ext-linear-rebuilt` — identical to static |
| **Ramp byte order** | **ok** | The driver packs A,B,G,R into an `ABGR8888` image and the sampler reads it back in that order. | `grad/ramp-word-order` — exact opaque red at every sample |
| **Legacy `vg_lite_draw_grad`** | **ok** ★ | **Works, deterministically, with a correct matrix**: `identity; translate(x,y); scale(w/1024)` (the driver's helpers post-multiply). Colours are the driver's own `0xAARRGGBB` — **not** `vg_lite_color_t` ABGR (`vg_lite_context.h:95-99`). | `grad/legacy-linear` — a textbook ramp, `repeat=same`, two boots identical |
| `vg_lite_set_grad(count=0)` | as documented | Returns `SUCCESS` with count 0; `update_grad` then substitutes black@0 → white@255, count 2. | `grad/legacy-linear` detail `c0=0,c0n=2,c0k=FF000000,c0w=FFFFFFFF` |

★ **The legacy row is a REFUTED prediction, and the retraction is the phase's
headline.** This document carried *"Legacy `vg_lite_draw_grad` is GC255-only —
on our GC355 it rendered solid black with a per-boot-varying checksum"* on the
strength of one sighting during the fader work. Pre-registered `broken`,
measured **ok** on two boots. The earlier black was that caller's matrix: an
identity maps the 1024-px ramp across an 80-px rect and samples ~6 % of it. NXP
gates the call on `chip_id == 0x255` in `vglite_layer.c` for their own reasons;
this silicon does not need the gate. The claim is retired, not softened.

★ **What this licenses.** Linear gradients are usable on this GC355 through
*either* API, provided the line (EXT) or the pattern matrix (legacy) is
re-specified in screen space whenever the path moves. Nothing here licenses
caching a ramp across placements — which is exactly the shape the
"placement-dependent" claim had, now with a mechanism and a measurement. The
guard layer's gradient helpers, refused in Phase 4 for want of a probe, now
have one.

★ **Two prior cells moved on the repeat axis during these boots**, both
already-broken misparse cases: `path/two-disjoint-bars` read `repeat=differs`
for the first time in six boots (now `unstable`, with the reason on its line),
and `path/evenodd-vs-nonzero`'s hole-cutting pass read `short:459` after three
boots of `short:308`. A misparse's determinism depends on the bytes that follow
the path in memory, and these boots ran a different image. See the correction
in the path section above.

## Images, blits & scissor — Phase 3, MEASURED

### ✅ PROBED 2026-09-02 — six cases, three boots, every line byte-identical across them. All six predictions held.

Spec: `docs/superpowers/specs/2026-09-02-gc355-conformance-phase3-design.md`.
Cases in `vgc_cases_blit.cpp`; host suite `tests/cases_blit_test.cpp` (seven
arms).

**The scissor is two mechanisms, and only one survives the fullscreen regime.**
`vg_lite_set_scissor` writes only context state (`vg_lite_image.c:263`). Right
and bottom then go to hardware — register `0x0A13` in `set_render_target`
(`vg_lite.c:3626`), every regime. Left and top exist *only* as a clamp on the
tessellation window inside `vg_lite_draw` (`vg_lite_path.c:1217`), and that
block is skipped when the target fits the tess buffer.

| Feature | Verdict | Safe usage | Evidence |
|---|---|---|---|
| **Scissor, multi-tile regime** (tess buffer smaller than the target — what both compositors run in) | **ok** | `vg_lite_set_scissor(x, y, right, bottom)`, right/bottom exclusive; all four edges clip. Disable with `(-1,-1,-1,-1)` after use. | `scissor/basic` — `L=1,T=1,R=1,B=1,in=1` |
| **Scissor, fullscreen regime** (tess buffer ≥ target) | **broken — left and top only** | **Never call `vg_lite_init()` with the panel's own size** if you scissor. Right and bottom still clip, left and top do not — half a clip, which is the most misleading failure available. | `scissor/tess-fullscreen` — `L=0,T=0,R=1,B=1`, three boots |
| **Blit, BGRA8888, 64-B stride** | ok | Natural layout. | `blit/basic` |
| **Blit with a padded stride** (data 64 B + 64 B pad per row) | ok | The rotary bench's rotor layout, unsheared. | `blit/stride-64` |
| **Blit with a stride not a multiple of 64 B** | **refused by the driver** | The 64-byte rule is `_check_source_aligned` (`vg_lite.c:1383`, on under `gcFEATURE_VG_16PIXELS_ALIGNED`): 64 for 32-bpp, 32 for 16-bpp, 16 for 8-bpp. Returns `VG_LITE_INVALID_ARGUMENT` before any command is built; nothing reaches the GPU. Pad the stride. | `blit/stride-unaligned` — `rc=1`, nothing drawn |
| **RGB565 source** | ok | Red in the **low** five bits (`0x001F`); 5-bit channels expand by **replication** (`0x1F` → 255). Pinned in code. | `blit/formats` — `order=low`, 255 |

★ **The stride rule is a driver check, not a hardware behaviour.** The address
checks (`srcbuf_align_check`) are compiled out on this chip; the *stride* check
is in. So "the GC355 refuses an unaligned source" is precisely true, and it is
the driver doing the refusing — which is why the case is safe in the default
build.

★ **Left out, deliberately:** A8/L8 sources (no consumer; their blend path
takes the `color` argument down a special branch) and `vg_lite_scissor_rects`
(the mask-layer scissor, unused here).

★ **The fullscreen prediction was sharpened before the boot** from the design
spec's "expect BROKEN" to "left and top lost, right and bottom kept", by
reading the two mechanisms — and measured exactly so. Arm 4 of the host suite
(a GPU that clips all four in fullscreen) is what proves the case could have
said otherwise.

## Guard layer — Phase 4

### ✅ BUILT 2026-09-02 — `VGLite/port/vglite_guard.h` (VGLite `4b75168`).

**Our port code, never the vendored driver.** It came last on purpose: writing
it first would have encoded the beliefs the probe existed to test, and three of
those beliefs did not survive the boot.

**What it enforces, and the case that licenses each rule:**

| Rule | Established by |
|---|---|
| exactly one `VLC_OP_MOVE` per path | `path/multi-contour-disjoint`, `path/two-disjoint-bars` (dropped); `path/four-nested-rings`, `path/evenodd-vs-nonzero` (nondeterministic); `path/two-contour-ring-nonzero` (mis-covers by 769 px) |
| the prescribed construction is exact | `path/two-draws-ring` — `fill=5376` exactly, beside a single-path ring 769 px short |
| a trailing `VLC_OP_END` is required | Phase 1: unterminated data hangs the front end while every call returns `VG_LITE_SUCCESS` |

Statuses: `ok`, `empty-path`, `no-contour`, `multi-contour`, `truncated`,
`no-end`, `trailing-data`, `bad-opcode`. **Structural faults are reported
before the contour count**, deliberately — a walk that has lost the opcode
boundary cannot be trusted to have counted `MOVE`s, and a confident wrong
answer is worse than a vague right one.

★ **The pure/driver split is the point, not tidiness.** The validator needs
only `<stdint.h>`, because **no gate in this tree can see GPU code** — every
QEMU gate runs the software engine, and the GPU goldens live only in
hand-pressed hardware transcripts. Host suite `VGLite/tests/run.sh`, **58
checks**, an arm per status, DEMONSTRATED RED against four mutants (stop
refusing multi-contour; accept unterminated paths; wrong `CUBIC` operand
count; hard-wire the validator to `OK`).

★ **DELIBERATELY NOT BUILT: gradient helpers.** The design spec conditioned
them on *"if the probe confirms it unusable for moving geometry"* — and **the
probe never tested gradients**. Phase 2 was redirected to colour and blend once
scoping found the matrix had never exercised the blend mode production uses.
The gradient rows in this document come from **reading NXP's source**, not from
a boot. Building helpers on them would put an unmeasured belief into the one
layer whose ordering exists to prevent exactly that. A recorded gap, not an
omission: if a later phase probes gradients, the helpers belong here and not
before.

★ **No colour helper either.** The `SRC_OVER` premultiply lives in SynthUI's
`src/synthui_fader_color.h`, host-testable with no driver dependency (69017
checks). Moving it here would make that test depend on VGLite; duplicating it
would create two copies of a measured constant.

### Acceptance — nothing moved (SynthUI `44a1c58`)

Both compositors were retrofitted, and **a guard that alters a rendered pixel
has changed behaviour rather than constrained it**, so the acceptance test is
that every golden holds:

| | measured |
|---|---|
| fader, GPU, silicon | `fd_crc=0x814F4047`, `delta==fresh=0xE9A9A2B5`, `fd_delta_eq=PASS`, `fd_gpu_err=0` |
| knob, GPU, silicon | all six `KNOB_SUM_*`, `KNOB_DELTA_SEQ=FULL=0x7C9EC8DB`, `EQ=PASS`, `MAXAREA=3050`, `rk_gpu_err=0`, `irqs=64` |
| fader / knob, sw, QEMU | `fd_crc=0xAB66DE0D`, `KNOB_GRID_SUM_SW=0x579E5810` |

Six consumer gates green (`synthui_fader_test`, `synthui_knob_test`,
`acid_box`, `vglite_lvgl_test`, `rotary_knob_bench`, `synthui_step_test`).

★ **`gpu_err=0` on both widgets is the reading that earns the layer.** The
guard validated every path either compositor builds and refused none — so it
constrains without changing, which is only interesting because it *could* have
refused something and did not. Neither compositor can currently trip it: every
path both build is single-contour by construction. That is the point. It exists
so a future edit reintroducing a second `VLC_OP_MOVE` fails loudly rather than
silently losing geometry on glass, which is exactly how that defect presented
the first time.
