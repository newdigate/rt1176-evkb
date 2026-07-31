# v8: PXP alpha-surface compositing + color keying — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The PXP library composites an overlay (AS) onto a source (PS) in one hardware
pass — all four alpha modes, invert, the 12-op ROP table, both colorkeys, four AS
formats — proven by a software oracle whose semantics are **measured on silicon first**,
then encoded into the QEMU model, with the oracle-checked frames held on the RK055 glass.

**Architecture:** Spec `docs/superpowers/specs/2026-07-31-rt1176-pxp-composite-design.md`
(the contract — read it first). **P2 (silicon) precedes P3 (model)**: the RM contradicts
itself at least twice (see "Known RM contradictions" below), so the oracle is corrected to
measured truth before the model exists. One example (`pxp_composite_test`) carries the
gate, the hardware truth table, and the eye ritual on the same frames.

**Tech stack:** PXP sibling repo (`~/Development/PXP`, MIT, fluent `PXPOp` API), custom
QEMU (`~/Development/qemu2`, `hw/dma/imxrt_pxp.c`), evkb example + gate conventions,
LinkServer + hardened SW4 capture flow (NEVER a LinkServer op with the VCOM held — see
memory `mac-kernel-panic-ioserialfamily` and `tools/rt1170-flash.sh`).

**Repos touched:** evkb (branch `pxp-composite-v8`), PXP (master), qemu2 (master).

## Known RM contradictions the P2 measurement MUST resolve (record each verdict in the transcript)

1. **Blend direction** — RM 52.3.1.12 prose: alpha 0xFF = opaque AS; its own equation:
   `R = á*PS.r + (1-á)*AS.r` with "á=0xFF → PS passes, AS discarded". Opposite meanings.
2. **AS-key result** — `AS_CTRL[ENABLE_COLORKEY]` field description: AS-key hit → pixel
   transparent, PS used; RM 52.3.1.13: "if AS is within AS color key range, then AS is
   passed". Opposite meanings.
3. **Effective-alpha arithmetic** — `á = (Gá*Eá + 0x80)/128`, capped at 128: which modes
   this formula governs (multiply only? always?), and the exact rounding/cap behavior
   across 0..255 (the gradient case).
4. **Output X byte with AS armed** — v7 measured X:=0 with the engine unconfigured;
   RM 52.3.2.2 says OUT alpha comes from `OUT_CTRL[ALPHA]`; the Porter-Duff figure
   defines an "alpha output" equation. What silicon writes with the LEGACY engine armed
   is the `ALPHA_OUT` case's job.

**Documented facts to build on (verified in RM/source):**
- Color-key hit condition: all three channels within `low..high` on the 24-bit expansion;
  **disable encoding: low=0xFFFFFF, high=0x000000** (RM: "program low to 0xff and high
  to 0x00 … range test never true").
- PS-key hit → AS passes, no blending; both keys hit → `PS_BACKGROUND` passes.
- AS idle idiom (no enable bit): degenerate `OUT_AS_ULC > OUT_AS_LRC` (the SDK
  convention) + both colorkeys at the disable encoding. `_program()` writes ALL of this
  every op, armed or idle.
- `OUT_AS_ULC/LRC` share the output-frame coordinate space with `OUT_PS_ULC/LRC`
  (`imxrt1176.h:1904-1905`); `outputAt` retargets `OUT_BUF`, so AS coordinates are
  relative to the output window — the validator requires the AS rect ⊆ output extent.
- AS_CTRL FORMAT namespace ([7:4]): ARGB8888=0x0, RGBA8888=0x1, RGB888X=0x4, RGB565=0xE.

---

### Task 0: Branch

- [ ] From `/Users/nicholasnewdigate/Development/rt1170/evkb` on a clean `master`:

```bash
git checkout -b pxp-composite-v8
```

---

### Task 1 (P1): Library API + validation + idle-AS discipline

**Files:**
- Modify: `~/Development/PXP/PXP.h` (enums + six fluent calls + op state)
- Modify: `~/Development/PXP/PXP.cpp` (`pxpAsFormat()`, `_program()` AS block, validation)

- [ ] **Step 1:** Read `PXP.h` and `PXP.cpp` in full (the op state block, `_program()`,
  the three format-namespace helpers at the top of the .cpp).

- [ ] **Step 2 (PXP.h):** add beside the existing enums:

```cpp
enum PXPAlphaMode : uint8_t {        /* AS_CTRL[ALPHA_CTRL], RM 52.6.22 */
    PXP_ALPHA_EMBEDDED = 0,          /* per-pixel alpha from the AS format   */
    PXP_ALPHA_OVERRIDE = 1,          /* AS_CTRL[ALPHA] replaces every pixel  */
    PXP_ALPHA_MULTIPLY = 2,          /* AS_CTRL[ALPHA] scales per-pixel      */
    PXP_ALPHA_ROPS     = 3,          /* the ROP table drives the ALU         */
};

enum PXPRop : uint8_t {              /* AS_CTRL[ROP], RM 52.6.22 */
    PXP_ROP_MASKAS = 0x0,  PXP_ROP_MASKNOTAS = 0x1, PXP_ROP_MASKASNOT = 0x2,
    PXP_ROP_MERGEAS = 0x3, PXP_ROP_MERGENOTAS = 0x4, PXP_ROP_MERGEASNOT = 0x5,
    PXP_ROP_NOTCOPYAS = 0x6, PXP_ROP_NOT = 0x7,
    PXP_ROP_NOTMASKAS = 0x8, PXP_ROP_NOTMERGEAS = 0x9,
    PXP_ROP_XORAS = 0xA,  PXP_ROP_NOTXORAS = 0xB,
};
```

and in `PXPOp` (public, beside `decimate()`):

```cpp
    /* Phase 3: composite an overlay (AS) onto the source.  Inert unless
     * overlay() is called; when it is not, _program() still writes the full
     * AS register set to its IDLE state (degenerate rect + colorkeys at the
     * never-true encoding) so a previous op can never leave the AS half-armed.
     * Semantics measured on silicon (v8 transcript) -- see README Phase 3. */
    PXPOp &overlay(const PXPSurface &as)   { _as = &as; return *this; }
    PXPOp &overlay(const PXPSurface &&) = delete;   /* dangling-temporary guard */
    PXPOp &overlayAt(uint16_t x, uint16_t y) { _as_x = x; _as_y = y; return *this; }
    PXPOp &overlayAlpha(PXPAlphaMode m, uint8_t value = 0xFF, bool invert = false)
        { _alpha_mode = m; _alpha_value = value; _alpha_invert = invert; return *this; }
    PXPOp &overlayColorKey(uint32_t low, uint32_t high)
        { _as_key_low = low; _as_key_high = high; _as_key = true; return *this; }
    PXPOp &sourceColorKey(uint32_t low, uint32_t high)
        { _ps_key_low = low; _ps_key_high = high; _ps_key = true; return *this; }
    PXPOp &rop(PXPRop r)                   { _rop = r; _rop_set = true; return *this; }
```

with matching private state (`_as = nullptr`, `_as_x/_as_y = 0`,
`_alpha_mode = PXP_ALPHA_EMBEDDED`, `_alpha_value = 0xFF`, `_alpha_invert = false`,
`_as_key/_ps_key/_rop_set = false`, key values, `_rop`).

- [ ] **Step 3 (PXP.cpp):** add `pxpAsFormat()` beside the other two namespace helpers
  (returning `PXP_FMT_NA` for everything outside the four supported), and extend
  `_program()`:

```cpp
    /* === AS (Phase 3) — written EVERY op, armed or idle ==================== */
    if (_as) {
        uint32_t as_fmt = pxpAsFormat(_as->format);
        if (as_fmt == PXP_FMT_NA)          return PXP_ERR_FORMAT;
        if (_rot != PXP_ROT_0 || _decx || _decy)
            return PXP_ERR_CONFIG;          /* v8 measures ROT_0 compositing only */
        if (_rop_set != (_alpha_mode == PXP_ALPHA_ROPS))
            return PXP_ERR_CONFIG;          /* rop() iff Rops mode */
        /* AS rect must sit inside the output extent (shared coordinate space
         * with OUT_PS_ULC/LRC -- outputAt has already retargeted OUT_BUF). */
        if ((uint32_t)_as_x + _as->width  > out_w ||
            (uint32_t)_as_y + _as->height > out_h)
            return PXP_ERR_CONFIG;
        PXP_AS_BUF    = (uint32_t)_as->data;
        PXP_AS_PITCH  = _as->pitchBytes;
        PXP_AS_CTRL   = (as_fmt << 4)
                      | ((uint32_t)_alpha_mode << 1)
                      | ((uint32_t)_alpha_value << 8)
                      | (_as_key ? (1u << 3) : 0)
                      | (_rop_set ? ((uint32_t)_rop << 16) : 0)
                      | (_alpha_invert ? (1u << 20) : 0);
        PXP_OUT_AS_ULC = ((uint32_t)_as_x << 16) | _as_y;   /* match OUT_PS_ULC packing -- READ the existing OUT_PS code and mirror it exactly */
        PXP_OUT_AS_LRC = (((uint32_t)_as_x + _as->width  - 1) << 16)
                       |  ((uint32_t)_as_y + _as->height - 1);
        PXP_AS_CLRKEYLOW  = _as_key ? _as_key_low  : 0x00FFFFFFu;
        PXP_AS_CLRKEYHIGH = _as_key ? _as_key_high : 0x00000000u;
    } else {
        if (_rop_set)                       return PXP_ERR_CONFIG;
        PXP_AS_CTRL       = 0;
        PXP_OUT_AS_ULC    = 0xFFFFFFFFu;    /* degenerate: ULC > LRC = disarmed */
        PXP_OUT_AS_LRC    = 0x00000000u;
        PXP_AS_CLRKEYLOW  = 0x00FFFFFFu;    /* never-true key range (RM 52.3.1.13) */
        PXP_AS_CLRKEYHIGH = 0x00000000u;
    }
    PXP_PS_CLRKEYLOW  = _ps_key ? _ps_key_low  : 0x00FFFFFFu;
    PXP_PS_CLRKEYHIGH = _ps_key ? _ps_key_high : 0x00000000u;
```

**NOTE the ULC packing comment**: the RM packs X in the high halfword for these
coordinate registers, but READ how the existing code writes `PXP_OUT_PS_ULC` and mirror
that exactly (the QEMU model's `PXP_COORD_X_SH` agrees with it) — a swapped packing
would place the overlay transposed and P2 would catch it, but get it right first.

- [ ] **Step 4:** update `PXP.h`'s header comment + `README.md`'s feature list ("Phase 3:
  compositing" section listing what is in/out; the deferred list shrinks to Porter-Duff,
  the six formats, NEXT queue, cross-format conversion).

- [ ] **Step 5 (regression at idle):** rebuild + run the three PXP gates and one PXP-using
  display gate — from `evkb/examples/display/`: `pxp_blit_test` (`./run_qemu_pxp.sh`),
  `pxp_decimate_test` (`./run_qemu_pxp_decimate.sh`), `pxp_yuv_test`
  (`./run_qemu_pxp_yuv.sh`), `lvgl_rk055_flip_test` (`./run_qemu.sh`). All must PASS —
  the idle-AS writes must be invisible. (QEMU ignores the still-unmodelled AS registers;
  that is fine for idle proof — the armed path is P2/P3's subject.)

- [ ] **Step 6:** commit:

```bash
cd ~/Development/PXP && git add PXP.h PXP.cpp README.md && \
git commit -m "Phase 3 API: overlay/alpha/colorkey/ROP programming + idle-AS discipline"
```

---

### Task 2 (P2a): The example + the RM-transcribed oracle

**Files:**
- Create: `examples/display/pxp_composite_test/{CMakeLists.txt,pxp_composite_test.cpp,toolchain/rt1170-evkb.toolchain.cmake}`

Model the CMakeLists + toolchain on `lvgl_pxp_copy_bench`'s (inlined toolchain, plain
`cmake -B build`), importing `MipiDisplay soc panels/rk055` + `PXP` (NO LVGL — this
example doesn't need it; background is CPU-drawn).

- [ ] **Step 1 (structure):** `setup()` brings up `Display.begin()` (RK055), prints
  `PXP_COMPOSITE_BEGIN`, allocates AS buffers in extmem (one per format, modest sizes:
  default overlay 160×120 at 4 B/px worst case), fills PS background CPU-side into the
  framebuffer with a position-dependent pattern, then runs the case table.

- [ ] **Step 2 (the oracle):** an independent per-pixel compositor,
  `oracle_composite(...)`, implementing the RM semantics with every contested rule
  behind a named function whose comment carries a `MEASURED:` tag to be filled by P2b:

```cpp
/* MEASURED: pending (P2b).  RM 52.3.1.12 contradicts itself on blend
 * direction; this function encodes the CURRENT best reading (prose: a=0xFF
 * -> AS opaque) and P2b flips it if silicon disagrees. */
static inline uint8_t blend_channel(uint8_t ps, uint8_t as, uint32_t a) { ... }
/* MEASURED: pending (P2b).  effective-alpha per mode; the (Ga*Ea+0x80)/128
 * cap-128 formula's applicability is a P2b question. */
static inline uint32_t effective_alpha(PXPAlphaMode m, uint8_t emb, uint8_t glob) { ... }
/* MEASURED: pending (P2b).  AS-key hit result: field description says PS
 * shows; 52.3.1.13 says AS passes.  Encoded: PS shows. */
```

Key rules already documented (not contested): key-hit = all three channels in range on
the 24-bit expansion; PS-key hit → AS passes unblended; both-keys hit → `PS_BACKGROUND`;
RGB565→24 expansion replicates upper bits (RM 52.3.1.22 rule 3, alpha:=0xFF);
RGB888X alpha:=0xFF; RGBA8888 alpha at low byte.

- [ ] **Step 3 (case table):** a `static const Case[]` covering, with exact count pinned
  (fill it in and print `CASES=<count>`):
  - formats × behaviors: ARGB8888 {embedded, override 0x80, override 0x00, multiply 0x80,
    invert-embedded, AS-key, PS-key, both-keys}; RGBA8888 {embedded, AS-key};
    RGB565 {override 0x80, AS-key (the green-screen), PS-key}; RGB888X {override 0x80}
    — plus an assert-style case: RGB565 embedded == opaque (RM rule 3).
  - the 12 ROPs on RGB565 AS over a fixed PS block.
  - geometries: centered, +13/+7 odd offset, edge-hugging bottom-right; one full-frame
    override case.
  - the gradient case: 256×1 strip per alpha 0..255 embedded (ARGB8888), oracle across
    the full range — the rounding probe.
  - `ALPHA_OUT`: composite ARGB8888 embedded alpha onto the frame at 32 bpp output...
    NOTE: the framebuffer here is the panel's — which at defaults is RGB565 (2 B/px,
    no X byte!). For the X-byte measurement the case composites into a scratch
    XRGB8888 extmem buffer as OUT instead (`PXPSurface` at 4 B/px), then CPU-dumps
    8 dest words: `ALPHA_OUT dst <w0> <w1> ...` — raw print, no oracle compare.
  Per ordinary case: run op → oracle into a scratch expected-frame → compare the
  composited sub-rect byte-for-byte + whole-frame FNV both ways →
  `CASE n=<name> EXPECT=0x%08lX GOT=0x%08lX MATCH|MISMATCH`. All cases composite INTO
  the live framebuffer (PS = framebuffer content, OUT = framebuffer) so the glass shows
  them; re-fill the PS pattern between cases.
- [ ] **Step 4 (ritual frames):** after the case table, four held frames (6 s each,
  `delay(6000)`, prompts printed): `FRAME=alpha_sprite`, `FRAME=greenkey_sprite`,
  `FRAME=fade` (override sweep 0→255 over 3 s), `FRAME=xor_rop`; then
  `Display.sampleUnderrun(10)` + `UNDERRUNS=%lu/%lu`, `PXP_COMPOSITE_DONE`. The holds
  run unconditionally (QEMU's gate ceiling accounts for them in Task 4).
- [ ] **Step 5:** build; run informally under QEMU (`tools/rt1170-qemu.sh` or a direct
  qrun invocation — NOT a gate yet): expect AS cases to MISMATCH/garbage (the model has
  no AS) — record that output; it is the "model missing" baseline, not a failure.
- [ ] **Step 6:** commit (example only — deliberately NO run_qemu script yet, so the
  sweep stays green at every commit; the gate lands with the model in Task 4):

```bash
git add examples/display/pxp_composite_test/ && \
git commit -m "pxp_composite_test: case matrix + RM-transcribed oracle (contested rules tagged MEASURED:pending)"
```

---

### Task 3 (P2b, NEEDS USER): silicon first — measure, resolve, correct

**Files:**
- Modify: `examples/display/pxp_composite_test/pxp_composite_test.cpp` (oracle corrections)
- Create: `examples/display/pxp_composite_test/transcript_hw_evkb.txt`

- [ ] **Step 1:** flash with the hardened flow (kill readers → `LinkServer flash load` +
  `verify` VCOM-free → kill probe daemons → attach `tools/rt1170-console.py` capture →
  **ask the user to press SW4**). Capture the full run.
- [ ] **Step 2:** for each MISMATCH: diagnose against the four contested rules; flip the
  tagged oracle function(s) (`MEASURED: pending` → `MEASURED: <date> <verdict>`), and
  ONLY those — a mismatch not explained by a contested rule is a real finding: STOP and
  investigate (systematic-debugging) before touching anything else. Rebuild, re-flash,
  re-run (user presses SW4 each time). Iterate until **every case MATCHes on silicon
  twice consecutively**.
- [ ] **Step 3:** record the `ALPHA_OUT` dump verdict (what silicon writes in X with the
  legacy engine armed) as a `MEASURED:` fact in the example comment.
- [ ] **Step 4:** write `transcript_hw_evkb.txt`: header (date, firmware, hardened-flow
  note), the verbatim final capture, and a **RESOLVED AMBIGUITIES** section giving each
  contested rule's measured verdict with the RM citations it settles.
- [ ] **Step 5:** commit:

```bash
git add examples/display/pxp_composite_test/ && \
git commit -m "pxp_composite_test: silicon truth table -- contested RM rules resolved by measurement"
```

---

### Task 4 (P3): QEMU model from the measurement + the gate

**Files:**
- Modify: `~/Development/qemu2/include/hw/dma/imxrt_pxp.h` (new register offsets)
- Modify: `~/Development/qemu2/hw/dma/imxrt_pxp.c` (AS datapath)
- Create: `examples/display/pxp_composite_test/run_qemu_pxp_composite.sh`
- Create: `examples/display/pxp_composite_test/transcript_qemu.txt`

- [ ] **Step 1 (header):** add `PXP_OUT_AS_ULC 0x90`, `PXP_OUT_AS_LRC 0xA0`,
  `PXP_AS_CTRL 0x150`, `PXP_AS_BUF 0x160`, `PXP_AS_PITCH 0x170`,
  `PXP_AS_CLRKEYHIGH 0x190`, `PXP_PS_CLRKEYHIGH 0x140` beside the existing map.
- [ ] **Step 2 (model):** in the per-pixel loop, after PS fetch: compute `in_as` from the
  OUT_AS rect (degenerate → never); when armed, fetch + unpack the AS pixel (four
  formats, RM expansion rules), apply AS colorkey, PS colorkey, both-keys→background,
  then the blend/ROP **exactly as the Task-3 transcript's RESOLVED AMBIGUITIES section
  states** — cite the transcript in the code comment for each formerly-contested rule.
  Output X byte per the `ALPHA_OUT` verdict. Unsupported AS formats → LOG_UNIMP + treat
  as disarmed (loud, not silent garbage). vmstate: registers already live in `regs[]` —
  confirm no new fields.
- [ ] **Step 3:** rebuild QEMU; run the example under qrun informally → all cases should
  now MATCH. Fix model until they do (the oracle is FROZEN — it is silicon's truth; a
  disagreement is a model bug by definition now).
- [ ] **Step 4 (gate):** `run_qemu_pxp_composite.sh` modeled on the bench's gate:
  poll-for-`PXP_COMPOSITE_DONE` with a ceiling measured from an actual run (the four
  6 s holds + fade make this gate slow — measure, add 50% margin), per-case anchored
  greps (`^CASE n=<name> ` + ` MATCH `), `MISMATCH` sweep, `^CASES=<count>$` pin,
  `ALPHA_OUT` presence check, guest-error log check, `PASS:` line.
- [ ] **Step 5 (negative, measured red):** sabotage one oracle constant (e.g. the blend
  rounding bias) in the working tree → build → gate must go RED naming the failing
  cases; record the red output; revert the sabotage BY HAND-EDITING BACK (no
  `git checkout` — hold-discipline habit), rebuild, gate green ×2. Refresh
  `transcript_qemu.txt` from the green run.
- [ ] **Step 6:** commits:

```bash
cd ~/Development/qemu2 && git add include/hw/dma/imxrt_pxp.h hw/dma/imxrt_pxp.c && \
git commit -m "pxp: AS datapath -- blend/colorkey/ROP per the v8 silicon truth table"
cd /Users/nicholasnewdigate/Development/rt1170/evkb && \
git add examples/display/pxp_composite_test/ && \
git commit -m "pxp_composite_test: QEMU gate green against the measured-truth model, negative test red-first"
```

- [ ] **Step 7:** regression: the three PXP gates + `lvgl_rk055_flip_test` +
  `lvgl_rk055_touch_test` + `lvgl_pxp_copy_bench` against the new QEMU — all PASS
  (the disarmed-AS path must be byte-identical to today).

---

### Task 5 (P4, NEEDS USER): the eye on the oracle's frames

**Files:**
- Modify: `examples/display/pxp_composite_test/transcript_hw_evkb.txt` (final section)

- [ ] **Step 1:** re-flash the FINAL firmware (hardened flow, SW4). The user watches the
  four held frames and reports each: the soft-edged sprite (alpha halo, no hard border),
  the green-keyed sprite (hard silhouette, NO green fringe), the fade (smooth ramp of
  overlay opacity), the XOR block (obviously inverted colors). Capture the console too —
  all cases must MATCH on this final firmware, `UNDERRUNS=0/10`.
- [ ] **Step 2:** append the ritual section (verbatim quotes + capture) to
  `transcript_hw_evkb.txt`; commit:

```bash
git add examples/display/pxp_composite_test/transcript_hw_evkb.txt && \
git commit -m "pxp_composite_test: on-glass ritual -- the oracle's frames confirmed by eye"
```

---

### Task 6 (P5): Wrap

**Files:**
- Modify: `tools/license-audit.sh` (one `GATES` entry — red-first: run the audit BEFORE
  adding the entry, observe the drift check go red naming the new gate, then add)
- Modify: `docs/KNOWN-BROKEN-GATES.md` (dated note: sweep 72 → **73**)
- Modify: `CLAUDE.md` (the sweep-count line: 72 → 73 everywhere it appears — the file
  itself demands re-measuring this line when gates are added)
- Modify: `evkb.cmake` (PXP pin bump)
- Modify: spec (AS-SHIPPED blockquote for every measured verdict that differed from the
  spec's best-reading assumptions)

- [ ] **Step 1:** push PXP + qemu2; bump the PXP pin in `evkb.cmake`; force-fetch proof
  (`cmake -B build-ff -DEVKB_FORCE_FETCH=ON` on the new example; build; delete).
- [ ] **Step 2:** audit red-first for the `GATES` entry, then green.
- [ ] **Step 3:** build every gate-owning example that is missing an ELF, then
  `./tools/run-all-qemu-gates.sh` → expect **73 passed, 0 failed, 0 SKIP** (or the
  documented `cm4_audio_test` singleton).
- [ ] **Step 4:** KNOWN-BROKEN-GATES dated note; CLAUDE.md count update; spec AS-SHIPPED
  re-sync; PXP README final state check.
- [ ] **Step 5:** wrap commit on the branch; then the finishing-a-development-branch flow
  (final whole-branch review first, as always).

---

## Self-review notes (already applied)

- Spec §7's P2-before-P3 order is structural in Tasks 2→3→4; the gate deliberately lands
  in Task 4 so the sweep is green at every commit.
- Spec §5's `ALPHA_OUT` case redirected to an XRGB8888 scratch OUT buffer — the panel
  framebuffer at defaults is RGB565 and has no X byte to measure (caught in planning).
- Spec §8.2's idle-AS discipline is Task 1 Step 3's else-branch + Step 5's regression.
- The four contested-rule functions are the ONLY oracle knobs Task 3 may turn; anything
  else red is a stop-and-investigate.
- CLAUDE.md's gate-count line update is explicitly listed (72 → 73) — the v6 lesson about
  stale baselines.
