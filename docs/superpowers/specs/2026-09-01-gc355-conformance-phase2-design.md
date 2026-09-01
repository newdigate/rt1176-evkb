# GC355 conformance Phase 2 — colour & blend — design

Date: 2026-09-01
Status: approved (brainstorm 2026-09-01)
Tracking: Linear **NEW-32**.
Supersedes, for Phase 2 only, the case list in
`docs/superpowers/specs/2026-08-30-gc355-conformance-design.md` §5.
Prior phases: Phase 1 (harness + paths/contours/winding), Phase 1b (the
disjoint-vs-nested 2×2), and the coverage check — all merged.

## 1. Why this phase, and why not the one the original spec described

The original §5 Phase 2 list is seven cases, four of them gradient. Two facts
found while scoping this phase moved the priority:

**Every one of the fifteen Phase 1 cases renders with `VG_LITE_BLEND_NONE`.
Both shipping compositors use `VG_LITE_BLEND_SRC_OVER` — exclusively, twelve
call sites.** So the conformance matrix has never once tested the blend mode
the production code actually uses, while the gradient APIs it *would* have
tested are already routed around by solid strips in both widgets.

The quirk table in the Phase 1 spec also lists *"SRC_OVER of AA paths is not
idempotent — double-composited edges drift"* with no case behind it. That
entry is **misleading as written**: a 50 %-coverage edge composited twice gives
`0.5s + 0.5(0.5s + 0.5d)` = `0.75s + 0.25d`. The drift is correct alpha
compositing, true of every conforming implementation. A case confirming
"twice ≠ once" would confirm nothing about this GPU.

So Phase 2 is **colour and blend**. Gradients are deferred; the four gradient
claims stay in `docs/gc355-vglite-quirks.md` explicitly marked as claims
without evidence.

## 2. Scope

Five new cases; matrix 15 → 20. Sweep count unchanged (same gate, more cases
inside it). One bench boot.

Blend surface is **`SRC_OVER` and `NONE` only**. The driver defines sixteen
modes and LVGL's backend uses ten, but LVGL-on-GC355 is silicon-only with its
own golden and is not the shipping path here. Other modes become a later phase
if a consumer needs them.

Also folded in, sharing the boot: the **`path/evenodd-vs-nonzero` pass-1
coverage gap** left by the coverage work. That pass is an EVEN_ODD hole, and
hole-cutting is the current leading hypothesis for why nested multi-contour
paths mis-cover, so it is the one uncovered arm that would test it.

## 3. The cases

| # | id | setup | what it settles |
|---|---|---|---|
| 1 | `color/solid-word-order` | pure red, opaque, `BLEND_NONE` | **The bootstrap control.** Exactly one channel saturated and the rest zero — *and* that it is the channel every downstream predicate assumes |
| 2 | `color/premultiplied-srcover` | white @ α=0x80 over **black**, `SRC_OVER` | ≈128 **or** 255 — the two readings below, reported. ≈64 is a double-premultiply defect |
| 3 | `blend/srcover-arithmetic` | white @ α=0x80 over **grey 0x40**, `SRC_OVER` | ≈160 **or** 255. Non-zero backdrop, so it exercises the destination term too |
| 4 | `blend/srcover-double` | case 3's draw, twice | **Reading-agnostic**: the second composite must land where the formula predicts *from the measured first*. Retires the idempotence entry as arithmetic rather than a quirk |
| 5 | `blend/none-honours-alpha` | white @ α=0x80 over grey, `BLEND_NONE` | Recorded, not judged — either answer is defensible |

### ★★ The driver's own header is inconsistent about whether `SRC_OVER` is premultiplied

Found while building the reference rasteriser, and it means cases 2–4 must **not**
pre-judge the answer. From `~/Development/VGLite/inc/vg_lite.h`:

- `:451` — "S and D represent source and destination **non-premultiplied** RGB color channels"
- `:457` — section heading: "**Non-premultiplied** Blending modes"
- `:460` — `VG_LITE_BLEND_SRC_OVER = 1` → `RGB: S + D*(1 - Sa)` — **no `*Sa`**, which is the *premultiplied* operator
- `:481` — `VG_LITE_BLEND_NORMAL_LVGL = 11` → `RGB: S*Sa + D*(1 - Sa)` — the *non*-premultiplied operator
- `:137` — `#define VG_LITE_BLEND_PREMULTIPLY_SRC_OVER VG_LITE_BLEND_NORMAL_LVGL`

So the names and the formulas are **inverted against each other**: the mode filed
under "non-premultiplied" carries the premultiplied formula, and the one aliased
"PREMULTIPLY" carries the non-premultiplied one. Two readings are therefore both
defensible for mode 1, which is what the compositors pass:

| | reading **A** (`S*Sa + D*(1-Sa)`) | reading **B** (`S + D*(1-Sa)`, header-literal) |
|---|---|---|
| case 2, over black | **128** | **255** |
| case 3, over grey 64 | **160** | 255 + 32 → clamps to **255** |

**Cases 2 and 3 admit both and report which**, as `model=A` / `model=B` in
`detail=`. A third value is `broken` — ≈64 in case 2 is the double-premultiply
defect, and anything else means neither reading holds.

★ **The two cases must agree.** If case 2 reports A and case 3 reports B, the
hardware is not implementing either formula consistently, and that is a bigger
finding than which formula it is. They are separate cases rather than one, so
the transcript shows both readings side by side and a human can see the
disagreement; nothing enforces it mechanically, because doing so would need
cross-case state the harness forbids.

★ **Case 4 sidesteps the question entirely, and that is why it is worth having.**
Rather than pinning an absolute value it renders once, **measures** the result,
renders again, and asserts the second lands where the same formula predicts
*from the measured first*. That holds under either reading — under A, 160 → 207;
under B, 255 → 255 — so it tests the *operator's self-consistency* without
depending on which operator it is. It is also exactly what the design said case 4
was for: "the double-composite becomes a DERIVED prediction from the same
formula".

Cases 2 and 3 are complementary, not redundant: `SRC_OVER` over black is
degenerate because `dst*(1-a)` vanishes, so case 2 alone cannot distinguish a
correct blend from one that ignores the destination.

Case 5 earns its place for a reason that only appeared while scoping: **all
fifteen Phase 1 cases use `BLEND_NONE` with an opaque colour.** If `BLEND_NONE`
silently honours alpha we have never seen it, and it changes how every Phase 1
result should be read the moment anyone passes a non-opaque colour through it.
Both outcomes are defensible, so the case records rather than judges — the
`path/degenerate-zero-area` pattern.

**Concretely, "records rather than judges" means:** the case reports `pixel=ok`
for *either* of the two defensible readings (α honoured ≈128, or α ignored 255)
and `broken` for anything else — a third value would mean `BLEND_NONE` is doing
something neither model predicts, which IS a finding. The measured value goes
in `detail=` so the answer is on the record whichever way it lands. Its
`expected_silicon.txt` line pre-registers which of the two we believe, so a
flip between them still shows as drift and still needs a reason.

## 4. The bootstrap problem, and how case 1 solves it

Writing colour predicates requires knowing the memory word order. **The word
order is what Phase 2 exists to measure.** That is circular, and it is the
central design problem of this phase.

Three ways out were considered:

- **Order-agnostic predicates everywhere** — never name a channel, test only
  relationships invariant under permutation. No bootstrap problem, but a red
  ramp's endpoint genuinely *is* red and expressing that without naming a
  channel gets contorted.
- **Runtime-resolved channel map** — case 1 stores the discovered index, later
  cases read it. Self-correcting, but it introduces exactly the cross-case
  state the harness forbids; Phase 1 deleted `s_fmt_fill` for this reason, and
  it would make results depend on table order.
- **Identity case first, compile-time mapping downstream** — CHOSEN.

`color/solid-word-order` fills **opaque** pure red via `VGC_ABGR(0xFF,0,0)` and
asserts **two** things:

1. **exactly two channels saturated and two zero** — order-agnostic, valid
   without knowing the answer; and
2. **that the saturated colour channel is the one `VGC_ABGR` and every
   downstream predicate assume**, and that the other saturated one is alpha.

★ **Two, not one — this phrasing was wrong in an earlier draft and would have
cost a bench cycle.** The fill is *opaque* pure red, so the memory word has red
AND alpha saturated and green AND blue zero. A reader who took "exactly one
saturated" literally would write `== 1`, and case 1 would report `broken` on
correct silicon — the instrument inventing a defect, in the one case that gates
the interpretation of every colour case below it. Note also that the counts
alone are **necessary but not sufficient**: `sat==2 && zero==2` is equally
satisfied by two saturated colour channels with zero alpha, so it is the named
half (`vgc_ch(px, VGC_A) == 0xFF`) that closes the case.

So a single case both *measures* the identity and *validates the assumption the
rest of the phase rests on*, with no shared state: the guard lives inside the
case rather than in a global. If it breaks, every colour verdict below it is
suspect — the role `path/single-contour-rect` plays for geometry, and stated in
the same terms.

Downstream predicates name channels normally.

★ **The mapping is not merely assumed — `vglite_probe` already measured it.**
That example cleared a `VG_LITE_BGRA8888` target (the same format as this
scratch) with `0xFF204060` and read `0xFF604020` back: the driver took the
argument as ABGR (B=0x20, G=0x40, R=0x60) and memory returned 0x60 in bits
23:16. That *is* "red is byte 2 of a BGRA8888 memory word". So
`color/solid-word-order` is a **re-confirmation on this scratch buffer in this
boot**, not the origin of the claim — and each channel macro should cite
`vglite_probe`'s measurement as its justification, with the case as the
standing check.

## 5. The predicate layer

New `examples/display/vglite_conformance/vgc_color.h`, a sibling to
`vgc_predicates.h` rather than an extension of it: different question,
different validity conditions, and `vgc_predicates.h` is already a coherent
~100 lines whose central predicate is explicitly **invalid** for coloured fills
(a pure-red fill has green=0 and reads as UNFILLED for every pixel — an
instrument fabricating a defect).

- `vgc_ch(px, i)` — byte *i* of the word. The order-agnostic accessor
  everything else is built from.
- `vgc_saturated_channels(px)` / `vgc_zero_channels(px)` — counts, so case 1
  can assert identity without naming anything.
- `vgc_near(a, b, tol)` — scalar comparison, so every tolerance is visible at
  its call site rather than buried in a helper.
- `VGC_R` / `VGC_G` / `VGC_B` / `VGC_A` — compile-time byte indices, each
  carrying `color/solid-word-order` in its comment as its justification.

Host-unit-tested with demonstrated mutants, like `vgc_predicates.h`. That file
had **four surviving mutants** on its first pass, all closed only because the
suite was mutation-tested; the same discipline applies here.

## 6. Sampling and tolerances

**Sampling.** Every colour case reads a **solid interior pixel**, well clear of
any edge, so coverage is exactly 1.0 and cannot confound the alpha term. The
`cover=` machinery from the coverage work is `n/a` throughout — filled area is
not the question these cases ask.

**Tolerance policy: generous first, justified by the plausible rounding models,
with the exact measured value always printed; tighten after the boot.** This is
a deliberate response to a mistake made earlier in this project: the
antialiasing tolerance `k=1/2` was extrapolated from a single data point and
proved ~13× more generous than needed. A tolerance invented from one
measurement is a guess wearing a number.

| case | arithmetic | plausible results | tol |
|---|---|---|---|
| 1 | saturation test | `0xFF` / `0x00` exactly | none — exact |
| 2 | `255 × 0.5` | 127.5 → 127 or 128 | ±4 |
| 3 | `255 × 0.5 + 64 × 0.5` | 159.5 → 159 or 160 | ±4 |
| 4 | `255 × 0.5 + 159.5 × 0.5` | 207.25 → 207 | ±6 |
| 5 | recorded, not judged | — | — |

±4 covers `/255` vs `/256` scaling and either rounding direction with room to
spare. Case 4's ±6 is wider because error compounds through two composites.

After the boot these narrow to what the hardware actually does — recorded as a
**deliberate narrowing with the measurement behind it**, which is not the same
act as re-goldening a checksum and must not be written up as one.

## 7. Greyscale in cases 2–4 is deliberate

White and grey are channel-symmetric, so a channel permutation is **invisible**
to cases 2–4. That is the intent: a word-order fault then surfaces in **exactly
one place**, case 1, rather than reddening four cases at once with no obvious
first cause.

Asymmetric colours would make each case an independent word-order check. The
trade is diagnosis, and this tree already settled it once — `single-contour-rect`
is one control whose failure invalidates what sits below it, said plainly, and
the same shape is used here.

## 8. Testing

The host geometry suite's reference rasteriser currently writes white and knows
nothing about colour or blending. It gains arbitrary colours and a `SRC_OVER`
implementation. This is the bulk of the work in this phase.

Three negative arms, the colour analogues of the existing first-contour-only
arm:

| arm | models | must break |
|---|---|---|
| alpha-ignoring | writes `src` regardless of α | cases 2, 3, 4 — case 1 stays ok |
| double-premultiply | applies α twice | case 2, at ≈64, its named failure mode |
| channel-permuting | swaps R and B | **case 1 only** — asserted, not assumed (see §7) |

**Circularity guard.** If the model implements the same formula the case
expects, arm 1 proves only that the predicate reads what the model wrote. So
every expected value is derived **twice, independently** — by hand in the
expectation file, and by the model from the formula — and the two must agree.
This is the discipline that validated the pentagram in Phase 1b (analytic
2792.30 against the model's 2792).

★ **Any new check needs an arm that exercises its FAILING branch.** The
coverage work found its three existing arms only ever reached `cover=ok` and
`cover=n/a`, so the whole check could have been hard-wired to pass with every
arm green; a fourth arm closed it and now fails fourteen cases by name when the
check is disabled. Phase 2's colour checks need the same treatment, and the
arms above are chosen to provide it.

## 9. Gate, expectation and drift

No change to the gate's shape. It asserts the honest negative as before — the
count moves 15 → 20, all five new ids join the by-name loop, and the anchored
summary updates. `expected_silicon.txt` gains five pre-registered lines, each
with a written reason, per the standing discipline that a `broken` or
multi-outcome expectation must say what was believed at the time.

The committed `transcript_hw_evkb.txt` predates these cases and is refreshed by
the boot.

## 10. Non-goals

- **Gradients.** Deferred. `grad/legacy-linear`, `grad/ext-linear-static`,
  `grad/ext-linear-moved` and `grad/ramp-word-order` remain unbuilt, and the
  quirks doc keeps marking those four claims as evidence-free.
- **The other fourteen blend modes.** Out until a consumer needs them.
- **Any change to the shipping compositors.** Phase 4's guard layer owns that,
  and only guards what the probe has confirmed.
- **Identifying the multi-contour mechanism.** Still open from Phase 1b; the
  `evenodd` pass-1 coverage extension folded in here is a probe of the leading
  hypothesis, not an attempt to settle it.

## 11. Risks

- **A tolerance is wrong and a control false-fails.** Mitigated by §6's
  generous-first policy and by case 1 having no tolerance at all (it is a
  saturation test, not a comparison).
- **The reference rasteriser and the case share a wrong assumption about the
  blend formula.** Mitigated by §8's twice-derived expected values.
- **`BLEND_NONE` turns out to honour alpha**, which would mean every Phase 1
  case ran under a blend mode we had mischaracterised. Case 5 exists precisely
  to surface that; the Phase 1 results are unaffected either way because every
  one of them used an opaque colour, but the *documentation* of what
  `BLEND_NONE` does would need correcting.
