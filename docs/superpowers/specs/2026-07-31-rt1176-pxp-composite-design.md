# RT1176 → PXP alpha-surface compositing + color keying (v8: PXP Phase 3) — Design

**Date:** 2026-07-31
**Status:** validated design, ready for an implementation plan
**Fulfils:** the PXP README's deferred-phases entry "Alpha-surface compositing,
Porter-Duff blending, colour-key" — the compositing and colour-key parts; the
Porter-Duff engine stays deferred (§2). Builds on the PXP Phase-1 BitBlit bring-up
and the v6/v7 display milestones, all complete, hardware-verified and merged.

---

## 1. Goal

The PXP grows its second input: the Alpha Surface (AS). After v8 the library
composites an overlay onto a source in one hardware pass — per-pixel alpha, global
override/multiply alpha, alpha-invert, both colorkey directions, and the 12-op ROP
table — and every behavior is proven by an independent software oracle in QEMU and
on silicon, with the oracle-checked frames held on the RK055 glass for the eye.

**Facts this design stands on, verified in source:**

1. Every needed register already exists in the core header
   (`cores/imxrt1176/imxrt1176.h:1904-1923`): `OUT_AS_ULC/LRC`, `AS_CTRL/BUF/PITCH`,
   `AS_CLRKEYLOW/HIGH`, `PS_CLRKEYLOW/HIGH`, plus `PXP_AS_FORMAT_MASK` ([7:4]).
2. `AS_CTRL` semantics mapped from RM 52.6.22: `FORMAT[7:4]` (ten encodings),
   `ALPHA_CTRL[2:1]` = Embedded/Override/Multiply/ROPs, `ALPHA[15:8]`,
   `ENABLE_COLORKEY[3]`, `ROP[19:16]` (12 ops), `ALPHA_INVERT[20]`.
3. Two distinct "green-screens": **AS colorkey** — overlay pixels in
   `AS_CLRKEYLOW..HIGH` are transparent (the PS pixel shows); **PS colorkey** — the
   `PS_CLRKEYLOW..HIGH` range on the source side. Exact precedence semantics
   (both keys enabled, key×alpha interaction) are transcribed from RM 52.3.2 into
   the oracle at plan stage and **verified against silicon before the model encodes
   them**.
4. The QEMU PXP model has no AS datapath (one reset value only) and its register
   map lacks `OUT_AS_ULC/LRC`, `AS_CTRL/BUF/PITCH`, `AS_CLRKEYHIGH`,
   `PS_CLRKEYLOW/HIGH` — v8's model work is real and additive.
5. v7's measured contract (output X byte = computed alpha; 0 with the alpha engine
   unconfigured) directly extends: **with AS enabled, the computed alpha is the
   blend's** — silicon gets first say on what the model writes (§4, §7).
6. `PXPOp`'s fluent API (`PXP.h:130-150`) has a clean slot for the overlay calls;
   the three format namespaces (OUT/PS/AS) are already documented at `PXP.cpp:23`.

---

## 2. Scope

**In (v8):**

- Library: AS programming, all four `ALPHA_CTRL` modes, `ALPHA_INVERT`, the full
  12-op ROP table, AS colorkey and PS colorkey.
- AS formats: **ARGB8888 (0x0), RGBA8888 (0x1), RGB565 (0xE), RGB888/X (0x4)**.
- QEMU model: the AS datapath — fetch + per-format unpack, placement rect, legacy
  blend equation, both colorkeys, ROPs. Everything the library can program;
  everything else stays loudly unmodelled.
- One example: `examples/display/pxp_composite_test` — QEMU gate + hardware ritual
  on the same firmware and the same oracle-checked frames.

**Out — named, not silently dropped:**

- **The Porter-Duff engine** (`PXP_PORTER_DUFF_CTRL`) — an independent second blend
  block; its own milestone if ever wanted.
- The six remaining AS formats (ARGB1555, ARGB4444, RGBA5551, RGBA4444, RGB555,
  RGB444) — no producer in the tree; the format validator rejects them loudly.
- AS combined with rotation/decimation — the op validator enforces RM 52.3.4.1;
  v8 measures the ROT_0 compositing space only.
- LVGL integration of compositing — a consumer milestone.
- Timing claims — no DWT table; compositing is a capability here, not a measured
  adoption decision. A future consumer that needs numbers builds its bench.

---

## 3. Library API (`~/Development/PXP`)

`PXPOp` grows six fluent calls, all inert unless `.overlay()` is called:

```cpp
PXPOp &overlay(const PXPSurface &as);          // AS buffer/pitch/format
PXPOp &overlay(const PXPSurface &&) = delete;  // dangling-temporary guard, as source()
PXPOp &overlayAt(uint16_t x, uint16_t y);      // OUT_AS_ULC; LRC derives from as dims
PXPOp &overlayAlpha(PXPAlphaMode m,            // Embedded | Override | Multiply | Rops
                    uint8_t value = 0xFF,      // ALPHA field (Override/Multiply)
                    bool invert = false);      // ALPHA_INVERT
PXPOp &overlayColorKey(uint32_t low, uint32_t high);  // AS key (overlay transparent)
PXPOp &sourceColorKey(uint32_t low, uint32_t high);   // PS key
PXPOp &rop(PXPRop op);                         // legal only with PXPAlphaMode::Rops
```

`PXPSurface` is unchanged; a new `pxpAsFormat()` maps `PXPFormat` into AS_CTRL's
namespace (4-bit field), returning not-supported for everything outside the four.
Validation in `_program()`:

- overlay rect fully inside the output extent, else `PXP_ERR_CONFIG`;
- `rop()` without `Rops` mode, or `Rops` without `rop()` → `PXP_ERR_CONFIG`;
- unsupported AS format → `PXP_ERR_FORMAT`;
- AS with rotate/decimate per RM 52.3.4.1 restrictions → `PXP_ERR_CONFIG`;
- when no `.overlay()` was called, the FULL AS register set is still written to its
  idle state every op — a half-armed AS from a previous op must be impossible
  (risk §8.2). The exact idle encoding is pinned at plan stage from the SDK's
  disable idiom and verified by the existing PXP gates staying green.

---

## 4. QEMU model (`~/Development/qemu2/hw/dma/imxrt_pxp.c`)

The per-pixel loop gains an AS stage between PS fetch and output write: when the
pixel lies inside `OUT_AS_ULC..LRC` and the AS is armed, fetch + unpack the AS
pixel (four formats, RM expansion rules), apply AS colorkey, then the blend
selected by `ALPHA_CTRL` (embedded / override / multiply, with invert) or the ROP;
PS colorkey applies on the source side per the silicon-verified precedence. New
registers in the model's map: `OUT_AS_ULC/LRC`, `AS_CTRL/BUF/PITCH`,
`AS_CLRKEYHIGH`, `PS_CLRKEYLOW/HIGH`.

**The model is written FROM the silicon measurement (P2 before P3, §7)** — v8
inverts v7's order because there is no prior model to lean on and v7 proved
silicon authors the contract. This includes the output X byte with AS enabled
(the blend's computed alpha — a dedicated `ALPHA_OUT` case dumps composited X
bytes on silicon first) and every rounding convention. Unmodelled leftovers
(Porter-Duff, the six formats, `OUT_CTRL[ALPHA_OUTPUT]`) keep their loud traps.

---

## 5. The example: `pxp_composite_test`

RK055 up via `Display.begin()`. PS = a position-dependent synthetic background
CPU-rendered into the framebuffer; AS overlays = per-format synthetic patterns in
extmem with varied alpha content and the key color in known regions. Per case:
program the op, run, then the **software oracle** — an independent per-pixel C++
compositor implementing the (silicon-verified) semantics — computes the expected
frame; token `CASE n=<name> EXPECT=0x… GOT=0x… MATCH`, whole-framebuffer FNV sums;
`CASES=<n>` pinned exactly (count fixed at plan stage, ~45–50):

- 4 formats × {embedded, override×2 values, multiply, invert, AS-key, PS-key,
  both-keys} where the behavior is format-meaningful;
- the 12 ROPs (RGB565 AS suffices — bitwise ops don't interact with unpack);
- placement geometries: centered, odd offsets, edge-hugging (the v6 lesson);
- a full-0..255 gradient-alpha case to expose rounding conventions (risk §8.3);
- the `ALPHA_OUT` X-byte measurement case.

Oracle cost is bounded: per-case oracle over the composited sub-rect plus the
whole-frame sum for OOB detection; modest case regions except a few full-frame
ones (risk §8.4).

**Hardware ritual:** same firmware, checksums must reproduce; the final cases hold
frames on glass — a soft-edged ARGB8888 sprite over the background, a green-keyed
RGB565 sprite (hard silhouette, no fringe), an override-alpha fade sequence, an
XOR-ROP block. `UNDERRUNS` printed per the v7 convention (vacuous in QEMU, stated).
Flash flow: the hardened VCOM-free + SW4 order (memory
`mac-kernel-panic-ioserialfamily`).

---

## 6. Verification

| Claim | QEMU | Hardware |
|---|---|---|
| every behavior × format × geometry composites per the measured semantics | oracle-match per case, `CASES` pinned | same tokens on silicon (and measured FIRST, §7 P2) |
| the model's blend math is silicon's | model written from the P2 truth table | any divergence = a named red case |
| output X byte with AS enabled | model writes the measured value | the `ALPHA_OUT` case |
| transparency/keying looks right | vacuous (stated) | the four held frames + the operator's report |
| nothing existing regressed | full sweep 72 → **73** with this gate; all PXP + display gates green | rituals unchanged elsewhere |

Negative test (gate-vacuity pattern): a sabotaged oracle constant must go red by
case name — measured red before the green is trusted. The audit's `GATES` table
gains one entry (red-first, as designed).

---

## 7. Decomposition

| | Milestone | Gate |
|---|---|---|
| P1 | Library API + validation; no model work. Existing PXP gates stay green (idle-AS discipline). | gates green |
| P2 | Example + oracle; **silicon first**: flash on the EVKB (hardened SW4 flow), measure the full case matrix incl. `ALPHA_OUT`; correct the oracle to the measured truth; record the table. | hardware transcript |
| P3 | QEMU model implements the measured semantics; gate green in QEMU; negative test red-first. | `CASES` pinned, all MATCH |
| P4 | On-glass ritual: the eye on the four held frames; transcripts finalized. | transcript + operator quotes |
| P5 | Wrap: sweep 72 → **73**, audit `GATES` entry, PXP README Phase-3 rows updated, pin bumps, docs, memory. | sweep + audit |

---

## 8. Risks

1. **RM ambiguities in blend/key semantics** (key precedence, multiply rounding,
   ROP alpha handling). Mitigated structurally: P2's silicon-first order means the
   oracle is corrected to measured truth before the model exists; ambiguities
   become documented facts, not assumptions.
2. **AS idle state** — `AS_CTRL` has no explicit enable bit; a half-armed AS would
   corrupt every existing single-source op. Mitigated: `_program()` writes the full
   AS set every op; the entire existing PXP + display gate population is the
   regression net.
3. **Rounding conventions** (integer ÷255 variants). The gradient-alpha case is
   designed to expose off-by-one across the full range on silicon.
4. **Gate runtime** — ~50 per-pixel-oracle cases. Bounded by sub-rect oracles +
   modest case regions; the gate's poll ceiling is set from measured QEMU runtime
   at plan stage, not guessed.
5. **QEMU model growth** — the per-pixel loop gains real branching. The model
   change is additive (AS disarmed = today's path, byte-identical), and the whole
   existing gate population must stay green before the new gate is trusted.

---

## 9. Licence firewall

All new code MIT in its home repos (PXP, qemu2, evkb). One new `GATES` entry,
red-first. No new dependencies, no vendored binaries.

---

## 10. Open questions for the plan stage

1. The AS idle/disable encoding (no enable bit): pin from the NXP SDK's
   `PXP_SetAlphaSurfacePosition`/disable idiom and verify by regression.
2. The exact case list and its pinned count (~45–50), including which
   behavior×format cells are meaningful (e.g. embedded alpha on RGB565 = fully
   opaque per RM rule 3 — assert that, don't skip it).
3. RM 52.3.2's precedence text for both-keys-enabled — transcribe, then confirm
   on silicon in P2.
4. Whether `overlayAt` placement interacts with `outputAt` placement (both move
   output-stage rects) — read the RM's ULC/LRC coordinate space definitions
   before writing the validator.
5. The gate's poll ceiling from measured runtime.

---

> **AS SHIPPED (Task-3 re-sync): the measured verdicts.** The silicon-first P2
> resolved every contested rule, several against the RM's text: blend =
> `(a*AS + (256-a)*PS) >> 8` (plain a, /256, truncating) with **exact AS
> pass-through at a==0xFF** — the RM's prose direction was right, its equation
> the typo; multiply effective alpha = `(Ga*Ea + 128) >> 8` (round-nearest
> /256 — the RM's `(Ga*Ea+0x80)/128` cap-128 formula fit 14/512 measured
> points); AS-key hit → PS shows (the field description was right, §52.3.1.13
> the typo); PS-key hit → AS unblended; both → PS_BACKGROUND; all 12 ROPs as
> tabulated.  The output X byte with the engine armed is a nonzero per-pixel
> computed alpha whose formula was deliberately left underived (no consumer;
> the gate checks the ALPHA_OUT line's presence only — a stated asymmetry).
> The case count pinned at **31** (§5's ~45–50 estimate over-counted; the
> enumerated behavior×format cells yield 31 with nothing dropped).  Full
> record: `examples/display/pxp_composite_test/transcript_hw_evkb.txt`.
