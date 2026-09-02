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
`eofill` agrees **exactly**: the nondeterminism lives in the no-hole pass.
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
on `eofill` — the nondeterminism lives in the no-hole pass, not the
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

★ **The fix is one line in `abgr_a`** (premultiply RGB by `a/255` before
packing). It **moves the fader's goldens** and belongs to Phase 4's guard layer,
not to a probe. Recorded here, deliberately not acted on.

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

## Gradients — deferred

### ⚠ NOT YET PROBED. Nothing in this section has a case.

Deferred by the Phase 2 design (2026-09-01), which went to colour and blend
instead — the surface both compositors actually use, and which no case had ever
touched. Still unbuilt: `grad/legacy-linear`, `grad/ext-linear-static`,
`grad/ext-linear-moved`, `grad/ramp-word-order`.

★ Two entries that used to sit here are now **built** and appear in the Colour
& blend section above: the ABGR/ARGB word order (`color/solid-word-order`) and
premultiplied src-over (`color/premultiplied-srcover`).

What the shipping compositors currently assert **without a probe case** —
each a claim awaiting evidence:

| Claim (no case yet) | Basis | Case |
|---|---|---|
| Legacy `vg_lite_draw_grad` is **GC255-only**. On our GC355 it rendered **solid black** and produced a **per-boot-varying checksum** on silicon, while every `vg_lite_*` call returned `VG_LITE_SUCCESS`. NXP's own `vglite_layer.c` calls it only when `chip_id == 0x255`. | `SynthUI/src/vglite/synthui_fader_gpu.cpp:79-82` | `grad/legacy-linear` — **not built** |
| The EXT ramp is **placement-dependent**. `vg_lite_update_linear_grad()` transforms the gradient line by `grad->matrix`, derives a screen-space length from it, then **overwrites both** `grad->matrix` and `grad->linear_grad` and allocates a new ramp surface **without freeing the previous one**. So a moving widget cannot cache a ramp — it must rebuild (and leaks if it does not clear). | `SynthUI/src/vglite/synthui_fader_gpu.cpp:83-99` | `grad/ext-linear-static`, `grad/ext-linear-moved` — **not built** |
| `vg_lite_set_grad()` returns success with `count=0` and **silently substitutes** a black→white ramp. | design spec §4 | `grad/legacy-linear` — **not built** |

## Images, blits & scissor — Phase 3

### ⚠ NOT YET PROBED. Nothing in this section has a case.

Spec case ids awaiting implementation (design spec §4, "Images/blits &
scissor"): `blit/basic`, `blit/stride-64`, `blit/stride-unaligned`,
`blit/formats`, `scissor/basic`, `scissor/tess-fullscreen`.

Two things about the harness that Phase 3 depends on and that Phase 1 already
fixed deliberately:

* **Phase 1 runs in the multi-tile regime on purpose.** The tessellation buffer
  is **64×64 against a 128×128 target** (`vgc_harness.h:28-37`), i.e. *smaller*
  than the target — the same regime both shipping compositors run in (256×256
  tess against a 720×1280 panel). A tess buffer ≥ the target puts the driver
  into `ts_is_fullscreen == 1`, which is a **different code path**, so a Phase 1
  matrix measured there would not describe the shipping configuration.
* **`scissor/tess-fullscreen` probes the other regime** — the one where the
  driver **skips left/top scissor clamping** because `ts_is_fullscreen != 0`.
  That is why calling `vg_lite_init()` with the panel's own dimensions defeats
  per-widget scissoring; stated in `SynthUI/src/vglite/synthui_fader_gpu.h:27-33`
  and expected BROKEN, pinning the precondition rather than inferring it.

The 64-byte **source-stride** rule for blits is stated in the rotary's comments
and has never been tested here — `blit/stride-64` and `blit/stride-unaligned`
exist to settle it.

---

## Guard layer — Phase 4

### ⚠ NOT YET BUILT.

`VGLite/port/vglite_guard.h` — **our port code, never the vendored driver**.

It comes **last** on purpose: it enforces only what the probe **CONFIRMED**.
Writing it first would be encoding the beliefs this whole exercise exists to
test — and the discriminator above is a live example of a belief that might not
survive the boot.

Planned content (design spec **§8**):

* a path builder enforcing **one contour per path** (asserts a single
  `VLC_OP_MOVE`, refuses a truncated path, requires the `VLC_OP_END`);
* a checked `init_path` wrapper;
* gradient helpers that enforce the EXT ordering, or refuse the API outright if
  the probe confirms it unusable for moving geometry;
* a shared `VGLITE_GUARD_TRY` error-counting macro replacing the copy-pasted
  `GPU_TRY` in both compositors.

Both existing compositors (`synthui_rotary_knob_gpu.cpp`,
`synthui_fader_gpu.cpp`) are then retrofitted onto it, and **their goldens must
not move** — two widgets, both engines, QEMU and silicon. That is the
acceptance test, and it is a strong one.
