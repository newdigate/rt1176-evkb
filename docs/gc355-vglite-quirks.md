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

## ⚠ SILICON STATUS: **NOT YET MEASURED**

**Phase 1 has not been run on the board.** Every verdict in the Phase 1 table
below is the **PRE-REGISTERED EXPECTATION** from
`examples/display/vglite_conformance/expected_silicon.txt` — a prediction
written *before* the boot, so that the boot can confirm or refute a stated
claim rather than rubber-stamp whatever the board printed.

**Do not quote a verdict from this document as a measurement.** Until this
banner is replaced with a transcript date, the Verdict column says what we
*expect*, not what the GC355 *does*. The words `expected` and `UNKNOWN` in that
column are load-bearing. After the Phase 1 boot each cell is replaced with the
measured answer and this section is replaced with the transcript's date.

Phases 2, 3 and 4 have no cases built at all; their sections are marked
accordingly and their rows are claims carried over from working code, not
probe results.

## Re-running it

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
| The probe (13 path cases + 1 opt-in dangerous) | `examples/display/vglite_conformance/` |
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

**Verdicts below are PRE-REGISTERED EXPECTATIONS, not measurements.** See the
status banner.

| Feature | Verdict | Safe usage | Case |
|---|---|---|---|
| single-contour filled path | expected OK | the only path shape to rely on | `path/single-contour-rect` |
| several disjoint contours in ONE path | expected BROKEN | **one contour per path, one `vg_lite_draw` per contour** | `path/multi-contour-disjoint` |
| the same, with CLOSE slots padded `0x01010101` | **UNKNOWN — the discriminator** | see [the discriminator](#the-discriminator-a-padded-close-slot) below | `path/multi-contour-close-padded` |
| hole cut by a reversed inner contour (non-zero) | expected BROKEN | don't; draw a filled plate, then an inset plate in the backdrop colour | `path/two-contour-ring-nonzero`, control `path/two-draws-ring` |
| `VG_LITE_FILL_EVEN_ODD` vs `NON_ZERO` across nested contours | expected BROKEN | the fill rule cannot rescue a dropped contour | `path/evenodd-vs-nonzero` |
| fill rules on ONE self-intersecting contour | expected OK | both rules honoured — this is the fill-rule usage that works | `path/self-intersecting` |
| path coordinate formats S8 / S16 / S32 / FP32 | expected OK | all four usable; **the opcode is one BYTE at the base of a format-width slot**, not a value of the format's type, so each format needs its own typed array | `path/format-s8`, `path/format-s16`, `path/format-s32`, `path/format-fp32`, agreement: `path/format-agreement` |
| degenerate (zero-area) geometry | expected OK | safe to emit; nothing, or a hairline on the degenerate row — do not rely on which | `path/degenerate-zero-area` |
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

### The discriminator: a padded CLOSE slot

**This is the most important open question in this document.** Two hypotheses
fit every observation this tree has made, and one boot separates them.

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

> **Until that boot: keep following one-contour-per-path.** It is the
> conservative reading of the current evidence and it is known to work — it is
> what both compositors ship.

---

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

## Gradients & colour — Phase 2

### ⚠ NOT YET PROBED. Nothing in this section has a case.

Spec case ids awaiting implementation: `grad/legacy-linear`,
`grad/ext-linear-static`, `grad/ext-linear-moved`, `grad/ramp-word-order`,
`color/solid-word-order`, `color/premultiplied-srcover`, `blend/modes`
(design spec §4, "Gradients & colour").

What the shipping compositors currently assert **without a probe case** —
each a claim awaiting evidence:

| Claim (no case yet) | Basis | Case |
|---|---|---|
| Legacy `vg_lite_draw_grad` is **GC255-only**. On our GC355 it rendered **solid black** and produced a **per-boot-varying checksum** on silicon, while every `vg_lite_*` call returned `VG_LITE_SUCCESS`. NXP's own `vglite_layer.c` calls it only when `chip_id == 0x255`. | `SynthUI/src/vglite/synthui_fader_gpu.cpp:79-82` | `grad/legacy-linear` — **not built** |
| The EXT ramp is **placement-dependent**. `vg_lite_update_linear_grad()` transforms the gradient line by `grad->matrix`, derives a screen-space length from it, then **overwrites both** `grad->matrix` and `grad->linear_grad` and allocates a new ramp surface **without freeing the previous one**. So a moving widget cannot cache a ramp — it must rebuild (and leaks if it does not clear). | `SynthUI/src/vglite/synthui_fader_gpu.cpp:83-99` | `grad/ext-linear-static`, `grad/ext-linear-moved` — **not built** |
| `vg_lite_color_t` is **ABGR** (red in the low byte), while ramp image words are **ARGB**. | `synthui_rotary_knob_gpu.cpp:219`, `synthui_fader_gpu.cpp:233` — measured by `vglite_probe` | `grad/ramp-word-order`, `color/solid-word-order` — **not built** |
| `vg_lite_set_grad()` returns success with `count=0` and **silently substitutes** a black→white ramp. | design spec §4 | `grad/legacy-linear` — **not built** |

Also unverified here and worth a case when Phase 2 lands: whether src-over is
applied **premultiplied** (50 % white over black should read ≈128, not 64 or
255) — `color/premultiplied-srcover`.

**Until then, the safe usage is what the fader ships:** solid interpolated
strips built from the same `emit_rect` / `finish_path` / `vg_lite_draw`
machinery as every other shape, which is proven on this silicon and
deterministic by construction (no ramp memory to sample). Note that **LVGL is
not a safe reference for the EXT API** — it contains the same two mistakes and
ships `LV_VG_LITE_DISABLE_LINEAR_GRADIENT_EXT` to route around the path.

---

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
