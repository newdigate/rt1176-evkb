# db pipeline sync copy acceleration — implementation plan (NEW-55)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the db pipeline's 260 ms CPU double-buffer sync copy with a byte-preserving accelerated one, chosen by a silicon probe of three candidates, installed for every `lvgl_mipi_panel_create_db()` consumer.

**Architecture:** Three arms are added to the existing `lvgl_pxp_copy_bench` (newlib `memcpy`, PXP with the `OUT_CTRL[ALPHA_OUTPUT]` override, a one-channel eDMA 2-D copy in vertical bands), each with its own byte contract, timed with the panel scanning out. The fastest byte-preserving arm under one refresh period is installed from `create_db()`; the nine synthui goldens and every delta-equality guard must not move, the two rk055 goldens move once, and the fader/knob GPU equality guards on silicon can veto.

**Tech Stack:** LVGL 9.4 port (`~/Development/LVGL/port`), PXP library (`~/Development/PXP`), `teensy-cores/imxrt1176` `DMAChannel`, qemu2's `imxrt_pxp.c` model, the gate/vacuity harness.

**Spec:** `docs/superpowers/specs/2026-09-18-db-sync-copy-acceleration-design.md`
**Branch:** `new-55-db-sync-copy` (already created, spec committed at `940c036`).

---

## Ground rules that apply to every task

- Run gates as `./run_qemu.sh`, never `sh run_qemu.sh`. One QEMU at a time; never run the sweep, the vacuity suite and the audit concurrently.
- `~/Development/LVGL`, `~/Development/PXP`, `~/Development/qemu2` are separate repos. Commit each in its own repo; the evkb repo pins them in `evkb.cmake` (Tasks 2 and 8). A pin must name a PUSHED SHA.
- Every Serial line must stay under 120 characters: the core's printf truncates at 128 bytes (NEW-53 finding A hit this). Print longer lines in two calls.
- The bench and the transition numbers are silicon claims; QEMU durations are fiction and never asserted.
- Bench sessions (Tasks 5 and 7) need the operator at the board: `tools/rt1170-flash.sh <hex>`, then SW4. Detach the console before any LinkServer op (`tools/rt1170-flash.sh --unlock`). Read captures with `tr -d '\r\000'`.

---

## File map

| file | change |
|---|---|
| `~/Development/qemu2/hw/dma/imxrt_pxp.c` | model `OUT_CTRL[ALPHA_OUTPUT]` (Task 1) |
| `~/Development/PXP/PXP.h`, `PXP.cpp`, `README.md` | `PXPOp::alphaOut()` (Task 2) |
| `~/Development/LVGL/port/lvgl_edma_bands.h` | NEW, pure band planner (Task 3) |
| `~/Development/LVGL/port/lvgl_edma_copy.h`, `.cpp` | NEW, eDMA copy handler (Task 3) |
| `~/Development/LVGL/port/tests/lvgl_edma_bands_test.c`, `tests/run.sh` | NEW suite + 3 mutants (Task 3) |
| `examples/display/lvgl_pxp_copy_bench/{lvgl_pxp_copy_bench.cpp,CMakeLists.txt,run_qemu.sh,transcript_qemu.txt,transcript_hw_evkb.txt}` | the probe (Tasks 4, 5) |
| `tools/gate-vacuity.test.sh` | fake-qemu creates the `-D` file; a bench section (Task 4) |
| `~/Development/LVGL/port/lvgl_pxp_copy.cpp`, `.h` | alpha override on 32-bit copies (Task 6a) |
| `~/Development/LVGL/port/lvgl_mipi_panel.cpp`, `.h` | install from `create_db()`, sync counters (Task 6) |
| 15 `examples/display/*/CMakeLists.txt` | compile the handler source (Task 6) |
| `examples/display/lvgl_rk055_{flip,touch}_test/*` | drop the explicit install, goldens (Task 6) |
| `examples/display/synthui_led_button_test/{synthui_led_button_test.cpp,run_qemu.sh}` | `led_button_sync` witness (Task 6) |
| `evkb.cmake`, `CLAUDE.md`, `examples/README.md` | pins, floor line, docs (Tasks 2, 8) |

---

### Task 1: QEMU PXP model — `OUT_CTRL[ALPHA_OUTPUT]`

**Files:**
- Modify: `~/Development/qemu2/hw/dma/imxrt_pxp.c:331-338` (the trap) and `:631-636` (the final pixel write)

- [ ] **Step 1: Replace the unmodelled trap with the decode**

At `imxrt_pxp.c:331`, replace:

```c
    /* OUT_CTRL[ALPHA_OUTPUT] (bit 23): override output alpha with
     * OUT_CTRL[ALPHA].  Nothing exercises it and modelling it unmeasured
     * would be speculation -- trap loudly instead of silently writing the
     * measured computed-alpha-0 below. */
    if (s->regs[PXP_OUT_CTRL / 4] & (1u << 23)) {
        qemu_log_mask(LOG_UNIMP,
                      "imxrt_pxp: OUT_CTRL[ALPHA_OUTPUT] not modelled\n");
    }
```

with:

```c
    /* OUT_CTRL[ALPHA_OUTPUT] (bit 23, RM 52.6.3): when set, byte 3 of every
     * 32-bit output pixel is OUT_CTRL[ALPHA] ([31:24]) instead of the
     * pipeline's computed alpha (which is 0 with the alpha engine
     * unconfigured -- measured, see the PS copy path below).  Applied at
     * the single pixel write at the end of this function so every path
     * (fill, PS copy, AS composite, rotation) honours it.  Modelled for
     * NEW-55: the LVGL port's sync copy uses ALPHA=0xFF to make a PXP copy
     * byte-identical to the CPU copy of an LVGL buffer (whose X byte is
     * always 0xFF).  MODELLED FROM THE RM, NOT YET MEASURED: the silicon
     * reading is taken in Task 5's lvgl_pxp_copy_bench pxp_aff arm.  If that
     * arm reads MISMATCH, silicon does not honour the bit and this model is
     * what is wrong (spec 2026-09-18-db-sync-copy-acceleration §4.2). */
    const bool alpha_override = s->regs[PXP_OUT_CTRL / 4] & (1u << 23);
    const uint8_t alpha_value = (s->regs[PXP_OUT_CTRL / 4] >> 24) & 0xFF;
    /* The bit with a 16-bit OUT format (ARGB1555/ARGB4444 carry alpha in
     * the pixel, not in byte 3) stays unmodelled -- and loud. */
    if (alpha_override && out_bpp != 4) {
        qemu_log_mask(LOG_UNIMP,
                      "imxrt_pxp: OUT_CTRL[ALPHA_OUTPUT] with a %u-byte OUT "
                      "format not modelled (16-bit alpha formats carry alpha "
                      "in the pixel, not in byte 3)\n", out_bpp);
    }
```

(QEMU style forbids mixed declarations: put the two `const` declarations in the function's declaration block beside `out_bpp`, with the comment; the `LOG_UNIMP` goes where the old trap was.) Once Task 5 has measured the bit, replace the "NOT YET MEASURED" sentence with the measured citation, the way `:473-479` cites the v7 DIAG dump.

- [ ] **Step 2: Apply it at the pixel write**

At the `dma_memory_write` near `:636`, insert before the write:

```c
            /* OUT_CTRL[ALPHA_OUTPUT]: override byte 3 of a 32-bit output.
             * out_bpp == 4 is exact here: the OUT FORMAT namespace has only
             * 0x00/0x04 at 4 bpp (RM 52.6.3 has no OUT 0x24 -- pxp_bpp()'s
             * 0x24 arm is the PS-only RGBA8888 encoding). */
            if (out_bpp == 4 && alpha_override) {
                pix[3] = alpha_value;
            }
```

so the block reads:

```c
            /* OUT_CTRL[ALPHA_OUTPUT]: override byte 3 of a 32-bit output. */
            if (out_bpp == 4 && alpha_override) {
                pix[3] = alpha_value;
            }
            /*
             * Written at an offset from the window origin, not from ULC: the
             * firmware retargets OUT_BUF for placement.
             */
            dma_memory_write(&address_space_memory,
                             out_buf + oy * out_pitch + ox * out_bpp,
                             pix, out_bpp, MEMTXATTRS_UNSPECIFIED);
```

Also update the comment at `:475-477` ("OUT_CTRL[ALPHA_OUTPUT]=1 (alpha override) is not modelled and is trapped loudly at the top of this function.") to: `OUT_CTRL[ALPHA_OUTPUT]=1 overrides this byte at the pixel write below.`

- [ ] **Step 3: Rebuild qemu2**

Run: `ninja -C ~/Development/qemu2/build qemu-system-arm 2>&1 | tail -3`
Expected: links `qemu-system-arm` with no error. Note the binary's mtime changes — every gate now runs the new model.

- [ ] **Step 4: Control gates before trusting the new binary**

Run, from the evkb repo:
```bash
(cd examples/display/lvgl_pxp_copy_bench && ./run_qemu.sh | tail -1)
(cd examples/display/lvgl_rk055_flip_test && ./run_qemu.sh | tail -1)
(cd examples/display/synthui_led_button_test && ./run_qemu.sh | tail -1)
```
Expected: all three `PASS`. The bit is clear in every existing op, so nothing may move — three green controls are the proof the rebuild changed nothing it should not have.

- [ ] **Step 5: Throwaway RED — prove the SET path is live before anything depends on it**

In `~/Development/PXP/PXP.cpp` `_program()`, temporarily OR `(1u << 23) | (0xFFu << 24)` into the `PXP_OUT_CTRL` write (uncommitted), rebuild the bench and run its gate: the 32-bit cases must read MISMATCH by name (the v6 oracle expects X:=0) while the 565 cases still MATCH. Revert (`git -C ~/Development/PXP checkout PXP.cpp`), rebuild, gate PASS again. This exercises the bit position, the shift, the placement after the AS block and the `out_bpp == 4` gate — everything except silicon — and separates a model bug from a library bug before Task 4 lands both on one gate.

- [ ] **Step 6: Commit and push qemu2**

```bash
cd ~/Development/qemu2 && git add hw/dma/imxrt_pxp.c && git commit -m "imxrt_pxp: model OUT_CTRL[ALPHA_OUTPUT] -- byte 3 of a 32-bit output is OUT_CTRL[ALPHA] when set, the measured computed-alpha 0 otherwise (NEW-55)" && git push
git log --oneline -1
```
Record the SHA: it is the new floor for the db gates (Task 8).

---

### Task 2: PXP library — `PXPOp::alphaOut()`

**Files:**
- Modify: `~/Development/PXP/PXP.h:196-232` (setter + fields), `~/Development/PXP/PXP.cpp:222-253` (`_program()`), `~/Development/PXP/README.md`
- Modify: `evkb.cmake:113` (pin)

- [ ] **Step 1: Add the setter and its state**

In `PXP.h`, after the `rop()` setter (`:197-198`), add:

```cpp
    /* OUT_CTRL[ALPHA_OUTPUT] (RM 52.6.3): write `alpha` into byte 3 of every
     * 32-bit output pixel instead of the pipeline's computed alpha.  The
     * computed alpha is 0 whenever the alpha engine is unconfigured (a plain
     * copy, a fill, a rotate -- MEASURED on the EVKB, lvgl_pxp_copy_bench
     * v7), so without this a 32-bit PXP copy is never byte-preserving.  With
     * alphaOut(0xFF) a copy of a buffer whose X byte is 0xFF everywhere --
     * every LVGL-rendered XRGB8888 buffer -- IS byte-identical to a CPU copy
     * (silicon-verified, NEW-55).  Rejected (PXP_ERR_CONFIG) with a 16-bit
     * output format, which has no alpha byte to override: an explicit
     * error, never a silent no-op. */
    PXPOp &alphaOut(uint8_t alpha)
        { _alpha_out = alpha; _alpha_out_set = true; return *this; }
```

In the private section, after `bool _fillOnly = false;` (`:215`), add:

```cpp
    bool         _alpha_out_set = false;   /* alphaOut() called */
    uint8_t      _alpha_out = 0xFF;
```

- [ ] **Step 2: Program it in `_program()`**

In `PXP.cpp`, after the `dbpp` check (`:230-231`, `if (!_fillOnly && (_src->bytesPerPixel() == 0 || _src->pitch == 0)) return PXP_ERR_CONFIG;`), add:

```cpp
    /* alphaOut() needs a 32-bit output: a 16-bit format has no alpha byte. */
    if (_alpha_out_set && dbpp != 4u)        return PXP_ERR_CONFIG;
```

Replace `:253`:

```cpp
    PXP_OUT_CTRL   = (uint32_t)out_fmt & PXP_OUT_FORMAT_MASK;
```

with:

```cpp
    /* OUT_CTRL: [4:0] format; [23] ALPHA_OUTPUT + [31:24] ALPHA only when the
     * op asked for the override (RM 52.6.3).  Every other op keeps writing
     * the computed alpha -- 0 with the engine unconfigured -- which is what
     * every existing golden measured. */
    PXP_OUT_CTRL   = ((uint32_t)out_fmt & PXP_OUT_FORMAT_MASK)
                   | (_alpha_out_set ? ((1u << 23) | ((uint32_t)_alpha_out << 24)) : 0u);
```

- [ ] **Step 3: README line**

In `~/Development/PXP/README.md`, in the op-builder section (find the `rop(` or `overlayAlpha(` entry), add:

```
- `alphaOut(alpha)` — write `alpha` into byte 3 of every 32-bit output pixel
  (`OUT_CTRL[ALPHA_OUTPUT]`). Without it the PXP writes its COMPUTED alpha,
  which is 0 for any op with the alpha engine unconfigured (copy, fill,
  rotate) — measured on silicon, so a plain 32-bit copy is never
  byte-preserving. `alphaOut(0xFF)` makes a copy of an LVGL XRGB8888 buffer
  byte-identical to a CPU copy. PXP_ERR_CONFIG with a 16-bit output.
```

- [ ] **Step 4: Build a consumer to prove it compiles**

Run: `cd examples/display/lvgl_pxp_copy_bench && cmake --build build 2>&1 | grep -E "error|warning: unused" ; ./run_qemu.sh | tail -1`
Expected: no errors; `PASS` (nothing calls the setter yet).

- [ ] **Step 5: Commit, push, pin**

```bash
cd ~/Development/PXP && git add PXP.h PXP.cpp README.md && git commit -m "PXPOp::alphaOut(): OUT_CTRL[ALPHA_OUTPUT] override for 32-bit outputs -- the computed alpha is 0 with the engine unconfigured, so a plain copy was never byte-preserving (NEW-55)" && git push && git log --oneline -1
```
Then in `evkb.cmake:113` replace the PXP SHA `5658e34885ff3a5cb5516a178ba60743e62a7517` with the new full SHA and append to the comment: `Bumped 2026-09-18 (alphaOut, NEW-55).` Commit in evkb: `git add evkb.cmake && git commit -m "evkb.cmake: PXP pin -> <sha> (PXPOp::alphaOut, NEW-55)"`.

---

### Task 3: eDMA copy — band planner, handler, host suite

**Files:**
- Create: `~/Development/LVGL/port/lvgl_edma_bands.h`
- Create: `~/Development/LVGL/port/lvgl_edma_copy.h`, `~/Development/LVGL/port/lvgl_edma_copy.cpp`
- Create: `~/Development/LVGL/port/tests/lvgl_edma_bands_test.c`
- Modify: `~/Development/LVGL/port/tests/run.sh`

- [ ] **Step 1: Write the failing host test**

Create `~/Development/LVGL/port/tests/lvgl_edma_bands_test.c`:

```c
/* Host test for lvgl_edma_bands.h -- the pure band planner behind
 * lvgl_edma_copy.cpp.  No LVGL, no target.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdint.h>
#include "lvgl_edma_bands.h"

static int fails = 0, checks = 0;
#define CHECK(name, cond) do { checks++; if (!(cond)) { fails++; printf("FAIL: %s\n", name); } } while (0)

int main(void)
{
    lvgl_edma_band_t b[LVGL_EDMA_MAX_BANDS];
    int n;

    /* A full 720-px XRGB8888 row (2880 B, stride 2880): three 960-B bands,
     * 32-byte bursts, skip = stride - band. */
    n = lvgl_edma_plan_bands(0x80000000u, 0x80400000u, 2880u, 2880u, 2880u, 1280u, b, LVGL_EDMA_MAX_BANDS);
    CHECK("full-row: three bands", n == 3);
    CHECK("full-row: band 0 bytes", n == 3 && b[0].bytes == 960u);
    CHECK("full-row: band 2 bytes", n == 3 && b[2].bytes == 960u);
    CHECK("full-row: band 1 src", n == 3 && b[1].src == 0x80000000u + 960u);
    CHECK("full-row: band 1 dst", n == 3 && b[1].dst == 0x80400000u + 960u);
    CHECK("full-row: rows", n == 3 && b[0].rows == 1280u);
    CHECK("full-row: mloff is stride minus band", n == 3 && b[0].mloff == 2880 - 960);
    CHECK("full-row: 32-byte bursts", n == 3 && b[0].xfer == 32u && b[2].xfer == 32u);
    for (int i = 0; i < n; i++) {
        CHECK("every band <= 1023 B (10-bit NBYTES)", b[i].bytes <= 1023u);
        CHECK("every band a multiple of its transfer size", (b[i].bytes % b[i].xfer) == 0u);
    }

    /* 719 px (2876 B) at x=1 (src offset 4): 960/960/956; the odd origin and
     * the 956-B tail both force 4-byte transfers. */
    n = lvgl_edma_plan_bands(0x80000004u, 0x80400004u, 2880u, 2880u, 2876u, 1u, b, LVGL_EDMA_MAX_BANDS);
    CHECK("719px: three bands", n == 3);
    CHECK("719px: tail band 956", n == 3 && b[2].bytes == 956u);
    CHECK("719px: unaligned origin -> 4-byte", n == 3 && b[0].xfer == 4u);
    CHECK("719px: 956 not a burst multiple -> 4-byte", n == 3 && b[2].xfer == 4u);

    /* 16x16 at +13+7 (src offset 13*4 = 52): one band, 4-byte transfers. */
    n = lvgl_edma_plan_bands(0x80000000u + 52u, 0x80400000u + 52u, 2880u, 2880u, 64u, 16u, b, LVGL_EDMA_MAX_BANDS);
    CHECK("+13: one band", n == 1);
    CHECK("+13: 4-byte (52 is not 32-aligned)", n == 1 && b[0].xfer == 4u);
    CHECK("+13: mloff", n == 1 && b[0].mloff == 2880 - 64);

    /* 1x1280 (4 B per row, 1280 rows): one band, skip almost a whole row. */
    n = lvgl_edma_plan_bands(0x80000000u + 2876u, 0x80400000u + 2876u, 2880u, 2880u, 4u, 1280u, b, LVGL_EDMA_MAX_BANDS);
    CHECK("1x1280: one band", n == 1);
    CHECK("1x1280: mloff", n == 1 && b[0].mloff == 2876);
    CHECK("1x1280: rows", n == 1 && b[0].rows == 1280u);

    /* RGB565 full row (1440 B, stride 1440): two bands 960 + 480. */
    n = lvgl_edma_plan_bands(0x80000000u, 0x80400000u, 1440u, 1440u, 1440u, 640u, b, LVGL_EDMA_MAX_BANDS);
    CHECK("565 row: two bands", n == 2);
    CHECK("565 row: 960 + 480", n == 2 && b[0].bytes == 960u && b[1].bytes == 480u);
    CHECK("565 row: bursts", n == 2 && b[0].xfer == 32u && b[1].xfer == 32u);

    /* Unservable shapes return 0. */
    CHECK("strides differ", lvgl_edma_plan_bands(0x80000000u, 0x80400000u, 2880u, 2884u, 2880u, 2u, b, LVGL_EDMA_MAX_BANDS) == 0);
    CHECK("zero rows", lvgl_edma_plan_bands(0x80000000u, 0x80400000u, 2880u, 2880u, 2880u, 0u, b, LVGL_EDMA_MAX_BANDS) == 0);
    CHECK("row wider than stride", lvgl_edma_plan_bands(0x80000000u, 0x80400000u, 2880u, 2880u, 2884u, 2u, b, LVGL_EDMA_MAX_BANDS) == 0);
    CHECK("rows past CITER's 15 bits", lvgl_edma_plan_bands(0x80000000u, 0x80400000u, 2880u, 2880u, 2880u, 40000u, b, LVGL_EDMA_MAX_BANDS) == 0);
    CHECK("more bands than the caller holds", lvgl_edma_plan_bands(0x80000000u, 0x80400000u, 2880u, 2880u, 2880u, 2u, b, 2) == 0);

    /* Register packing: SMLOE|DMLOE, MLOFF at [29:10], NBYTES at [9:0]. */
    CHECK("nbytes_reg 960/1920", lvgl_edma_nbytes_reg(960u, 1920) == 0xC01E03C0u);
    CHECK("ssize code 32", lvgl_edma_ssize_code(32u) == 5u);
    CHECK("ssize code 4", lvgl_edma_ssize_code(4u) == 2u);
    CHECK("ssize code 2", lvgl_edma_ssize_code(2u) == 1u);
    CHECK("ssize code 1", lvgl_edma_ssize_code(1u) == 0u);

    printf("lvgl_edma_bands_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Run it to see it fail to compile**

Run: `cd ~/Development/LVGL/port && cc -std=c99 -Wall -Wextra -Werror -I . -o /tmp/edma_t tests/lvgl_edma_bands_test.c`
Expected: `fatal error: 'lvgl_edma_bands.h' file not found`.

- [ ] **Step 3: Write the planner**

Create `~/Development/LVGL/port/lvgl_edma_bands.h`:

```c
/* lvgl_edma_bands.h - PURE band planner for an eDMA 2-D memory copy.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * A rectangle copy is `rows` minor loops of `row_bytes` with a skip of
 * `stride - row_bytes` on both sides -- the eDMA minor-loop offset
 * (NBYTES_MLOFFYES with SMLOE|DMLOE).  With offsets enabled NBYTES is 10
 * bits (<= 1023 B), so a 2880-B XRGB8888 row does not fit one minor loop:
 * the row is split into VERTICAL BANDS of <= LVGL_EDMA_BAND_MAX_BYTES, each
 * its own TCD run to major completion (CITER = rows).  960 is the largest
 * multiple of 32 under 1024, so a full-width band keeps 32-byte bursts.
 * One MLOFF field serves both SADDR and DADDR, which is why the strides
 * must be equal (they are for LVGL's two framebuffers).
 * No LVGL, no target: host-tested in tests/lvgl_edma_bands_test.c with
 * mutants that must go red. */
#pragma once
#include <stdint.h>

#define LVGL_EDMA_BAND_MAX_BYTES 960u
#define LVGL_EDMA_MAX_BANDS      4     /* 2880 B is 3 bands; 4 leaves slack for a tail */
#define LVGL_EDMA_CITER_MAX      32767u  /* CITER/BITER without E_LINK: 15 bits */

typedef struct {
    uint32_t src, dst;     /* band origins (byte addresses) */
    uint16_t bytes;        /* one minor loop = one band-row */
    uint16_t rows;         /* major loop count */
    int32_t  mloff;        /* skip after each minor loop, both sides */
    uint8_t  xfer;         /* transfer size: 32, 4, 2 or 1 bytes */
} lvgl_edma_band_t;

/* Largest transfer size that divides the band and aligns BOTH origins and the
 * stride.  A 32-byte burst on a 4-aligned origin is an eDMA configuration
 * error (SADDR must be SSIZE-aligned), so the check is on every operand. */
static inline uint8_t lvgl_edma_xfer_for(uint32_t src, uint32_t dst, uint32_t bytes, uint32_t stride)
{
    if (!(src % 32u) && !(dst % 32u) && !(bytes % 32u) && !(stride % 32u)) return 32u;
    if (!(src % 4u)  && !(dst % 4u)  && !(bytes % 4u)  && !(stride % 4u))  return 4u;
    if (!(src % 2u)  && !(dst % 2u)  && !(bytes % 2u)  && !(stride % 2u))  return 2u;
    return 1u;
}

/* TCD ATTR SSIZE/DSIZE encoding (RM eDMA TCD_ATTR): 0=8-bit 1=16 2=32 5=32-byte burst. */
static inline uint8_t lvgl_edma_ssize_code(uint8_t xfer)
{
    return xfer == 32u ? 5u : xfer == 4u ? 2u : xfer == 2u ? 1u : 0u;
}

/* NBYTES_MLOFFYES: [31] SMLOE, [30] DMLOE, [29:10] MLOFF (signed), [9:0] NBYTES. */
static inline uint32_t lvgl_edma_nbytes_reg(uint16_t bytes, int32_t mloff)
{
    return (1u << 31) | (1u << 30) | (((uint32_t)mloff & 0xFFFFFu) << 10) | ((uint32_t)bytes & 0x3FFu);
}

/* Plan the bands for a rows x row_bytes rectangle.  Returns the band count,
 * or 0 when the shape cannot be served (the caller then uses the CPU copy):
 * zero rows or bytes, rows past CITER, unequal strides, a row wider than
 * the stride, a skip outside MLOFF's 20 signed bits, or more bands than
 * `max`. */
static inline int lvgl_edma_plan_bands(uint32_t src, uint32_t dst,
                                       uint32_t src_stride, uint32_t dst_stride,
                                       uint32_t row_bytes, uint32_t rows,
                                       lvgl_edma_band_t *out, int max)
{
    if (rows == 0u || row_bytes == 0u || rows > LVGL_EDMA_CITER_MAX) return 0;
    if (src_stride != dst_stride) return 0;
    if (row_bytes > src_stride) return 0;
    int n = 0;
    uint32_t off = 0u;
    while (off < row_bytes) {
        if (n >= max) return 0;
        uint32_t bytes = row_bytes - off;
        if (bytes > LVGL_EDMA_BAND_MAX_BYTES) bytes = LVGL_EDMA_BAND_MAX_BYTES;
        const int32_t mloff = (int32_t)src_stride - (int32_t)bytes;
        if (mloff < 0 || mloff > 0x7FFFF) return 0;
        out[n].src   = src + off;
        out[n].dst   = dst + off;
        out[n].bytes = (uint16_t)bytes;
        out[n].rows  = (uint16_t)rows;
        out[n].mloff = mloff;
        out[n].xfer  = lvgl_edma_xfer_for(src + off, dst + off, bytes, src_stride);
        n++;
        off += bytes;
    }
    return n;
}
```

- [ ] **Step 4: Run the test to see it pass**

Run: `cd ~/Development/LVGL/port && cc -std=c99 -Wall -Wextra -Werror -I . -o /tmp/edma_t tests/lvgl_edma_bands_test.c && /tmp/edma_t`
Expected: `lvgl_edma_bands_test: 37 checks, 0 failed`, exit 0.

- [ ] **Step 5: Wire the suite and its three mutants into `tests/run.sh`**

Append to `~/Development/LVGL/port/tests/run.sh`, before the final exit (read the file's tail first; its rotation arms end with a `[ $fail -eq 0 ]`-style exit — put this block before it and fold its result into `fail`):

```sh
# --- lvgl_edma_bands.h: the eDMA band planner (NEW-55) ----------------------
cc -std=c99 -Wall -Wextra -Werror -I "$here" -o "$out/edma_test" "$here/tests/lvgl_edma_bands_test.c"
"$out/edma_test"

edma_arm() { # <name> <sed-expr>
    mkdir -p "$out/$1"
    sed "$2" "$here/lvgl_edma_bands.h" > "$out/$1/lvgl_edma_bands.h"
    if cmp -s "$here/lvgl_edma_bands.h" "$out/$1/lvgl_edma_bands.h"; then
        echo "FAIL: mutant $1 did not apply (sed matched nothing)"; fail=1; return
    fi
    cp "$here/tests/lvgl_edma_bands_test.c" "$out/$1/"
    if ! cc -std=c99 -Wall -Wextra -Werror -o "$out/$1/t" "$out/$1/lvgl_edma_bands_test.c" 2> "$out/$1/cc.log"; then
        echo "FAIL: mutant $1 did not compile"; cat "$out/$1/cc.log"; fail=1; return
    fi
    if "$out/$1/t" > "$out/$1/log" 2>&1; then
        echo "FAIL: mutant $1 PASSED the suite -- the tests cannot see it"; fail=1
    elif ! grep -q "^FAIL:" "$out/$1/log"; then
        echo "FAIL: mutant $1 died without naming a check"; fail=1
    else
        echo "PASS: mutant $1 red by name: $(grep -m1 '^FAIL:' "$out/$1/log")"
    fi
}
# A band allowed past the 10-bit NBYTES field.
edma_arm edma_band_1024 's/#define LVGL_EDMA_BAND_MAX_BYTES 960u/#define LVGL_EDMA_BAND_MAX_BYTES 1024u/'
# A 32-byte burst chosen without checking the origins' alignment.
edma_arm edma_burst_unaligned 's/if (!(src % 32u) \&\& !(dst % 32u) \&\& !(bytes % 32u) \&\& !(stride % 32u)) return 32u;/if (!(bytes % 32u) \&\& !(stride % 32u)) return 32u;/'
# The skip computed from the band width instead of the stride.
edma_arm edma_skip_from_width 's/const int32_t mloff = (int32_t)src_stride - (int32_t)bytes;/const int32_t mloff = (int32_t)bytes;/'
```

- [ ] **Step 6: Run the whole port suite**

Run: `~/Development/LVGL/port/tests/run.sh; echo "rc=$?"`
Expected: the rotation suite as before, `lvgl_edma_bands_test: 37 checks, 0 failed`, three `PASS: mutant edma_* red by name` lines, `rc=0`.

- [ ] **Step 7: Write the handler header**

Create `~/Development/LVGL/port/lvgl_edma_copy.h`:

```cpp
/* lvgl_edma_copy.h - eDMA-backed lv_draw_buf copy handler for the i.MX RT1176.
 *
 * C++ ONLY, like the rest of the port.  Compiled by every example that
 * compiles lvgl_mipi_panel.cpp (create_db installs it -- NEW-55); needs no
 * library beyond the core's DMAChannel.
 *
 * SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include "lvgl.h"

/* Claim one eDMA channel and arm its DMAMUX always-on request.  false when
 * no channel is free or the L1 D-cache is ON (a DMA copy of a cached buffer
 * moves stale bytes; this core enables only the I-cache -- NEW-36).
 * Idempotent. */
bool lvgl_edma_copy_begin(void);

/* Synchronous rectangle copy: `rows` rows of `row_bytes`, both sides at their
 * own stride (which must be EQUAL -- one MLOFF field serves both).  Bands of
 * <= 960 B, each run to major completion, 32-byte bursts where alignment
 * allows.  false on any error, a bounded-wait timeout (100 ms, after which the
 * channel is disabled so a wedged transfer cannot write late) or an
 * unservable shape -- the caller then copies with the CPU.  Byte-preserving
 * by construction: no format, no alpha engine, every byte moves. */
bool lvgl_edma_copy_rect(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t row_bytes, uint32_t rows);

/* Install the eDMA-backed buf_copy handler on LVGL's GLOBAL draw-buf handlers
 * (lv_draw_buf_get_handlers()).  Saves the default CPU copy and CHAINS to it
 * for anything not the accelerated shape: RGB565 or XRGB8888, same format
 * and size both sides, height >= 2 rows, area >= threshold_px, equal strides,
 * areas inside their buffers.  Every fallback and every error is counted --
 * degraded loud, correct always.  Install AFTER lv_init. */
void lvgl_edma_copy_install(uint32_t threshold_px);

uint32_t lvgl_edma_copies();
uint32_t lvgl_edma_copy_fallbacks();
/* The subset of fallbacks caused by the eDMA itself (error bit or timeout):
 * 0 on any healthy run; adopting gates pin it. */
uint32_t lvgl_edma_copy_errors();
```

- [ ] **Step 8: Write the handler**

Create `~/Development/LVGL/port/lvgl_edma_copy.cpp`:

```cpp
/* lvgl_edma_copy.cpp - eDMA-backed lv_draw_buf copy handler (see the header).
 *
 * SPDX-License-Identifier: MIT */
#include "lvgl_edma_copy.h"
#include "lvgl_edma_bands.h"
#include "lvgl_private.h"   /* lv_draw_buf_handlers_t: buf_copy_cb lives here */
#include <Arduino.h>
#include <DMAChannel.h>

static lv_draw_buf_copy_cb_t s_default_copy = nullptr;
static DMAChannel *s_ch = nullptr;
static bool s_begun = false;
static uint32_t s_copies = 0, s_fallbacks = 0, s_errors = 0, s_threshold_px = 0;

bool lvgl_edma_copy_begin(void)
{
    if (s_begun) return true;
    /* A DMA copy of a write-back-cached buffer moves whatever is in SDRAM,
     * not what the CPU last wrote.  This core never sets DC (NEW-36 enabled
     * only IC); if that ever changes this handler must add cache maintenance
     * before it is allowed to run, so refuse rather than assume. */
    if (SCB_CCR & SCB_CCR_DC) return false;
    /* Heap-allocated, not static: DMAChannel's constructor claims a channel,
     * and a static instance would do that before setup(). */
    s_ch = new DMAChannel();
    if (!s_ch->TCD) { delete s_ch; s_ch = nullptr; return false; }   /* no free channel */
    /* DMAMUX always-on request: a software START serves ONE minor loop on
     * classic eDMA, so a rows-deep major loop needs the request line held
     * asserted; DREQ (set per band below) drops ERQ at major completion. */
    s_ch->triggerContinuously();
    s_begun = true;
    return true;
}

static bool run_band(const lvgl_edma_band_t *b)
{
    DMABaseClass::TCD_t *t = s_ch->TCD;
    const uint8_t code = lvgl_edma_ssize_code(b->xfer);
    s_ch->clearComplete();          /* DONE must be clear before the channel can start */
    s_ch->clearError();
    t->SADDR         = (const void *)(uintptr_t)b->src;
    t->SOFF          = b->xfer;
    t->ATTR_SRC      = code;        /* SMOD 0, SSIZE */
    t->ATTR_DST      = code;        /* DMOD 0, DSIZE */
    t->NBYTES_MLOFFYES = lvgl_edma_nbytes_reg(b->bytes, b->mloff);
    t->SLAST         = 0;
    t->DADDR         = (void *)(uintptr_t)b->dst;
    t->DOFF          = b->xfer;
    t->CITER         = b->rows;
    t->DLASTSGA      = 0;
    t->CSR           = DMA_TCD_CSR_DREQ;
    t->BITER         = b->rows;
    s_ch->enable();                 /* ERQ: the always-on request starts it now */
    const uint32_t t0 = micros();
    while (!s_ch->complete()) {
        if (s_ch->error()) { s_ch->disable(); s_ch->clearError(); return false; }
        if ((uint32_t)(micros() - t0) > 100000u) { s_ch->disable(); return false; }
    }
    asm volatile("dsb" ::: "memory");   /* the CPU reads the destination next */
    return true;
}

bool lvgl_edma_copy_rect(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t row_bytes, uint32_t rows)
{
    if (!s_begun) return false;
    lvgl_edma_band_t bands[LVGL_EDMA_MAX_BANDS];
    const int n = lvgl_edma_plan_bands((uint32_t)(uintptr_t)src, (uint32_t)(uintptr_t)dst,
                                       src_stride, dst_stride, row_bytes, rows,
                                       bands, LVGL_EDMA_MAX_BANDS);
    if (n == 0) return false;
    for (int i = 0; i < n; i++) {
        if (!run_band(&bands[i])) return false;
    }
    return true;
}

static uint32_t fmt_bpp(lv_color_format_t cf)
{
    switch (cf) {
    case LV_COLOR_FORMAT_RGB565:   return 2u;
    case LV_COLOR_FORMAT_XRGB8888: return 4u;
    default:                       return 0u;
    }
}

static bool area_fits(const lv_draw_buf_t *buf, const lv_area_t *area, uint32_t bpp)
{
    if (area->x1 < 0 || area->y1 < 0) return false;
    if (area->x2 >= (int32_t)buf->header.w) return false;
    if (area->y2 >= (int32_t)buf->header.h) return false;
    return buf->header.stride >= buf->header.w * bpp;
}

/* Signature: lv_draw_buf_copy_cb_t, lv_draw_buf.h. */
static void edma_copy_cb(lv_draw_buf_t *dest, const lv_area_t *dest_area,
                         const lv_draw_buf_t *src, const lv_area_t *src_area)
{
    if (!dest || !src || !dest_area || !src_area) {
        s_fallbacks++;
        s_default_copy(dest, dest_area, src, src_area);
        return;
    }
    const int32_t w = lv_area_get_width(dest_area);
    const int32_t h = lv_area_get_height(dest_area);
    const uint32_t bpp = fmt_bpp((lv_color_format_t)dest->header.cf);
    const bool shape_ok =
        s_begun &&
        dest->data && src->data &&
        bpp != 0u &&
        src->header.cf == dest->header.cf &&
        w == lv_area_get_width(src_area) &&
        h == lv_area_get_height(src_area) &&
        h >= 2 &&
        (uint32_t)w * (uint32_t)h >= s_threshold_px &&
        dest->header.stride == src->header.stride &&
        area_fits(dest, dest_area, bpp) && area_fits(src, src_area, bpp);
    if (!shape_ok) {
        s_fallbacks++;
        s_default_copy(dest, dest_area, src, src_area);
        return;
    }
    const uint8_t *sp = src->data
                      + (uint32_t)src_area->y1 * src->header.stride
                      + (uint32_t)src_area->x1 * bpp;
    uint8_t *dp = dest->data
                + (uint32_t)dest_area->y1 * dest->header.stride
                + (uint32_t)dest_area->x1 * bpp;
    if (!lvgl_edma_copy_rect(dp, dest->header.stride, sp, src->header.stride,
                             (uint32_t)w * bpp, (uint32_t)h)) {
        /* Counted APART from shape fallbacks; the CPU copy then redoes the
         * whole rectangle, so a partial DMA write is never left standing. */
        s_errors++;
        s_fallbacks++;
        s_default_copy(dest, dest_area, src, src_area);
        return;
    }
    s_copies++;
}

void lvgl_edma_copy_install(uint32_t threshold_px)
{
    lv_draw_buf_handlers_t *h = lv_draw_buf_get_handlers();
    if (h->buf_copy_cb != edma_copy_cb) s_default_copy = h->buf_copy_cb;
    s_threshold_px = threshold_px;
    /* begin() failing (no channel, D-cache on) leaves s_begun false: every
     * copy then chains to the CPU and counts a fallback -- visible, never
     * wrong. */
    (void)lvgl_edma_copy_begin();
    h->buf_copy_cb = edma_copy_cb;
}

uint32_t lvgl_edma_copies()         { return s_copies; }
uint32_t lvgl_edma_copy_fallbacks() { return s_fallbacks; }
uint32_t lvgl_edma_copy_errors()    { return s_errors; }
```

- [ ] **Step 9: Compile it against a target build (no consumer yet)**

Temporarily add `${_lvgl_dir}/port/lvgl_edma_copy.cpp` to `examples/display/lvgl_pxp_copy_bench/CMakeLists.txt` (Task 4 makes this permanent; do it now so the compile error surfaces here): add `evkb_library_dir(LVGL _lvgl_dir)` after `import_evkb_library(PXP)` and list the source in `teensy_add_executable`. Run: `cd examples/display/lvgl_pxp_copy_bench && cmake --build build 2>&1 | grep -E "error|Error" ; echo done`.
Expected: no errors. If `DMABaseClass::TCD_t` or a macro name differs from `~/Development/teensy-cores/imxrt1176/DMAChannel.h:66-90`, fix the handler, not the core.

- [ ] **Step 10: Commit in LVGL**

```bash
cd ~/Development/LVGL && git add port/lvgl_edma_bands.h port/lvgl_edma_copy.h port/lvgl_edma_copy.cpp port/tests/lvgl_edma_bands_test.c port/tests/run.sh && git commit -m "port: eDMA 2-D copy handler -- one channel, <=960-B vertical bands with minor-loop offsets, byte-preserving; pure band planner host-tested with three mutants red (NEW-55)"
```

---

### Task 4: The probe — bench arms, scanout, per-arm oracles, gate, vacuity

**Files:**
- Rewrite: `examples/display/lvgl_pxp_copy_bench/lvgl_pxp_copy_bench.cpp`
- Modify: `examples/display/lvgl_pxp_copy_bench/CMakeLists.txt`, `run_qemu.sh`
- Modify: `tools/gate-vacuity.test.sh` (fake-qemu `-D`; a new section)
- Re-capture: `examples/display/lvgl_pxp_copy_bench/transcript_qemu.txt`

- [ ] **Step 1: CMake — panel, the eDMA handler source, depth definitions**

Replace `examples/display/lvgl_pxp_copy_bench/CMakeLists.txt` with:

```cmake
cmake_minimum_required(VERSION 3.24)
project(lvgl_pxp_copy_bench)

# The panel scans out during the measurement (NEW-55): the LCDIFv2's SDRAM
# reads are what turned the v6 bench's 174 ms full-frame CPU copy into the
# pipeline's 260 ms.  MipiDisplay needs the depth macros every db example sets.
add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()      # lv_draw_buf_copy IS the thing under test (CPU side)
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(lvgl_pxp_copy_bench
    lvgl_pxp_copy_bench.cpp
    ${_lvgl_dir}/port/lvgl_edma_copy.cpp)
teensy_target_link_libraries(lvgl_pxp_copy_bench cores MipiDisplay PXP)

target_link_libraries(lvgl_pxp_copy_bench.elf LVGL stdc++)
```

- [ ] **Step 2: Rewrite the bench**

Replace `examples/display/lvgl_pxp_copy_bench/lvgl_pxp_copy_bench.cpp` with:

```cpp
/* lvgl_pxp_copy_bench - CPU vs PXP vs eDMA for LVGL's cross-buffer sync copy.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * v7 (2026-07-31) measured LVGL's lv_draw_buf_copy against a PXP blit on 14
 * geometries x {RGB565, XRGB8888} and found the PXP's 32-bit copy writes
 * X:=0.  NEW-55 (2026-09-18) turned that into the probe that picks the db
 * pipeline's sync copy: FOUR ARMS, each with ITS OWN BYTE CONTRACT, timed
 * with the panel SCANNING OUT (the v6 numbers had no scanout and read 174 ms
 * where the pipeline reads 260 for the same full-frame copy).  FIVE arms.
 *
 *   arm=cpu_clib  newlib memcpy per row      byte identity   (the CPU floor)
 *   arm=pxp_x0    PXP.blit                   RGB identity, X:=0 (v7's contract)
 *   arm=pxp_aff   PXP with alphaOut(0xFF)    src=xff:   byte identity with the
 *                                            plain CPU copy (every LVGL buffer
 *                                            has X=0xFF);  src=mixed: RGB
 *                                            identity, X==0xFF -- proves the
 *                                            override is what writes it
 *   arm=pxp_affa  as pxp_aff but the surfaces declared PXP_ARGB8888 (OUT
 *                 encoding 0x00, the one the RM describes as carrying an
 *                 alpha component; pxp_aff's XRGB8888 is 0x04 RGB888).
 *                 Same bytes in memory, so src=xff byte identity holds if
 *                 the bit works at 0x00 -- the arm that separates "silicon
 *                 ignores the bit" from "ignores it for RGB888".
 *   arm=edma      lvgl_edma_copy_rect        byte identity, any source
 *
 * CORRECTNESS (QEMU-gated): every arm MATCHes its contract on every case, by
 * name; the count is pinned (140).  TIMING (hardware-only): DWT cycles per
 * arm per case, plus ref_us for the lv_draw_buf_copy reference of the same
 * case.  QEMU's numbers are vacuous and the transcript says so.  Decision
 * rule (spec 2): the fastest byte-preserving arm under one refresh period
 * (33 ms) on the full-frame case wins.
 *
 * Fills and checksums cover the WHOLE max-size allocation in both formats,
 * so an out-of-rect write is as red as a wrong pixel (v7's rule).
 * Uses Serial1 (LPUART; QEMU captures it).  Every line stays under 120
 * characters: the core's printf truncates at 128 (NEW-53 finding A). */
#include <Arduino.h>
#include <string.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_edma_copy.h"
#include "PXP.h"

static constexpr uint32_t BUF_W = 720, BUF_H = 1280;
static constexpr uint32_t BUF_BYTES_MAX = BUF_W * 4u * BUF_H;   /* 8888 extent */

struct Case { uint16_t w, h, x, y; };
/* The v7 matrix, unchanged: button-scale through full-screen, odd offsets
 * and widths, edge-hugging rects.  The gate pins the count. */
static constexpr Case CASES[] = {
    {  16,   16,   0,    0}, {  16,   16,  13,    7},
    {  64,   64,   0,    0}, {  64,   64,  13,    7},
    { 120,  140,   0,    0}, { 120,  140, 599, 1139},
    { 200,  160,   8,   48}, { 360,  320,   0,    0},
    { 360,  320, 180,  480}, { 719,    1,   1,    0},
    {   1, 1280, 719,    0}, { 720,  640,   0,  320},
    { 720,  640,   0,  640}, { 720, 1280,   0,    0},
};
static constexpr uint8_t NUM_CASES = sizeof(CASES) / sizeof(CASES[0]);

struct Fmt {
    const char       *tag;
    lv_color_format_t cf;
    uint8_t           bpp;
    uint32_t          seed_a, seed_b;   /* format-dependent fills: a copy routed */
};                                      /* at the wrong bpp cannot checksum clean */
static constexpr Fmt FMTS[] = {
    { "565",  LV_COLOR_FORMAT_RGB565,   2u, 0xA53Cu,     0x0F1Eu     },
    { "8888", LV_COLOR_FORMAT_XRGB8888, 4u, 0xC3A5F00Du, 0x1E2D3C4Bu },
};

enum Arm : uint8_t { ARM_CPU_CLIB, ARM_PXP_X0, ARM_PXP_AFF, ARM_PXP_AFFA, ARM_EDMA };
static const char *const ARM_TAG[] = { "cpu_clib", "pxp_x0", "pxp_aff", "pxp_affa", "edma" };
enum Src : uint8_t { SRC_MIXED, SRC_XFF };          /* X byte: whatever the seed gives / forced 0xFF */
static const char *const SRC_TAG[] = { "mixed", "xff" };
enum Contract : uint8_t { CON_BYTES, CON_X0, CON_XFF };

/* One line of the matrix per (arm, source, contract); pxp_aff is 32-bit only
 * (alphaOut() is PXP_ERR_CONFIG on a 16-bit output, by design). */
struct Run { Arm arm; Src src; Contract con; bool only8888; };
static constexpr Run RUNS[] = {
    { ARM_CPU_CLIB, SRC_MIXED, CON_BYTES, false },
    { ARM_PXP_X0,   SRC_MIXED, CON_X0,    false },   /* 565: CON_X0 is byte identity */
    { ARM_PXP_AFF,  SRC_XFF,   CON_BYTES, true  },
    { ARM_PXP_AFF,  SRC_MIXED, CON_XFF,   true  },
    { ARM_PXP_AFFA, SRC_XFF,   CON_BYTES, true  },   /* OUT encoding 0x00 (ARGB8888) */
    { ARM_PXP_AFFA, SRC_MIXED, CON_XFF,   true  },   /* both rows: honoured at 0x00 vs forwards the PS alpha */
    { ARM_EDMA,     SRC_MIXED, CON_BYTES, false },
};

static uint8_t *s_src, *s_dst;
static lv_draw_buf_t s_src_db, s_dst_db;

static uint8_t *alloc_buf() {
    uint8_t *raw = (uint8_t *)extmem_malloc(BUF_BYTES_MAX + 64);
    if (!raw) return nullptr;
    return (uint8_t *)(((uintptr_t)raw + 63) & ~(uintptr_t)63);
}

/* Position- and format-dependent fill over the WHOLE allocation; x_ff forces
 * byte 3 of every word to 0xFF -- what an LVGL XRGB8888 render leaves there. */
static void fill_buf(uint8_t *b, uint32_t seed, bool x_ff) {
    uint32_t *w = (uint32_t *)b;
    for (uint32_t i = 0; i < BUF_BYTES_MAX / 4; i++) {
        uint32_t v = seed ^ (i * 2654435761u);
        if (x_ff) v |= 0xFF000000u;
        w[i] = v;
    }
}

static uint32_t dst_sum() {                 /* FNV-1a over the WHOLE dest */
    lvgl_sum_reset();
    lvgl_sum_feed(s_dst, BUF_BYTES_MAX);
    return lvgl_sum_value();
}

static uint32_t cycles_us(uint32_t cyc) { return cyc / 996u; }

/* The contract's X-byte expectation, applied to the in-rect bytes of the CPU
 * reference OUTSIDE the timed window.  No-op for 16-bit and for CON_BYTES. */
static void apply_contract(const Fmt &F, const Case &c, Contract con) {
    if (F.bpp != 4u || con == CON_BYTES) return;
    const uint32_t stride = BUF_W * 4u;
    for (uint32_t ry = 0; ry < c.h; ry++) {
        uint32_t *row = (uint32_t *)(void *)(s_dst + ((uint32_t)c.y + ry) * stride + (uint32_t)c.x * 4u);
        for (uint32_t rx = 0; rx < c.w; rx++)
            row[rx] = (con == CON_X0) ? (row[rx] & 0x00FFFFFFu) : (row[rx] | 0xFF000000u);
    }
}

/* Reference: fresh dest, LVGL's own copy (timed), the contract, the sum. */
static uint32_t reference(const Fmt &F, const Case &c, const lv_area_t &area, Contract con, uint32_t *ref_us) {
    fill_buf(s_dst, F.seed_b, false);
    const uint32_t t0 = ARM_DWT_CYCCNT;
    lv_draw_buf_copy(&s_dst_db, &area, &s_src_db, &area);
    *ref_us = cycles_us(ARM_DWT_CYCCNT - t0);
    apply_contract(F, c, con);
    return dst_sum();
}

static const char *pxp_err_name(PXPError e) {
    switch (e) {
    case PXP_OK:                return "PXP_OK";
    case PXP_ERR_BUSY:          return "PXP_ERR_BUSY";
    case PXP_ERR_TIMEOUT:       return "PXP_ERR_TIMEOUT";
    case PXP_ERR_CONFIG:        return "PXP_ERR_CONFIG";
    case PXP_ERR_UNREACHABLE:   return "PXP_ERR_UNREACHABLE";
    case PXP_ERR_ALIGN:         return "PXP_ERR_ALIGN";
    case PXP_ERR_AXI_READ:      return "PXP_ERR_AXI_READ";
    case PXP_ERR_AXI_WRITE:     return "PXP_ERR_AXI_WRITE";
    case PXP_ERR_NOT_BEGUN:     return "PXP_ERR_NOT_BEGUN";
    case PXP_ERR_FORMAT:        return "PXP_ERR_FORMAT";
    case PXP_ERR_UNIMPLEMENTED: return "PXP_ERR_UNIMPLEMENTED";
    }
    return "PXP_ERR_UNKNOWN";
}

/* One arm into a fresh dest.  Returns an error token, or nullptr. */
static const char *run_arm(Arm arm, const Fmt &F, const Case &c, uint32_t *us) {
    const uint32_t stride = BUF_W * F.bpp;
    const uint8_t *sp = s_src + (uint32_t)c.y * stride + (uint32_t)c.x * F.bpp;
    uint8_t *dp = s_dst + (uint32_t)c.y * stride + (uint32_t)c.x * F.bpp;
    fill_buf(s_dst, F.seed_b, false);
    const char *err = nullptr;
    const uint32_t t0 = ARM_DWT_CYCCNT;
    switch (arm) {
    case ARM_CPU_CLIB:
        for (uint32_t r = 0; r < c.h; r++)
            memcpy(dp + r * stride, sp + r * stride, (size_t)c.w * F.bpp);
        break;
    case ARM_PXP_X0:
    case ARM_PXP_AFF:
    case ARM_PXP_AFFA: {
        /* pxp_affa declares the SAME bytes as ARGB8888 -- OUT encoding 0x00
         * instead of XRGB8888's 0x04 RGB888 -- because RM 52.6.3 introduces
         * OUT_CTRL[ALPHA] with "when generating an output buffer with an
         * alpha component": a MISMATCH on pxp_aff alone could mean the bit
         * is honoured only at 0x00, and this arm says which. */
        const auto pxp_fmt = (F.bpp == 2u) ? PXP_RGB565
                           : (arm == ARM_PXP_AFFA) ? PXP_ARGB8888 : PXP_XRGB8888;
        /* Offset-base sub-rect surfaces, the v7 idiom; 2880 fits uint16_t. */
        PXPSurface ssrc(const_cast<uint8_t *>(sp), c.w, c.h, pxp_fmt, (uint16_t)stride);
        PXPSurface sdst(dp, c.w, c.h, pxp_fmt, (uint16_t)stride);
        PXPOp op = PXP.op();
        op.source(ssrc).output(sdst);
        if (arm == ARM_PXP_AFF || arm == ARM_PXP_AFFA) op.alphaOut(0xFF);
        const PXPError pe = op.run();      /* synchronous: program + enable + wait */
        if (pe != PXP_OK) err = pxp_err_name(pe);
        break; }
    case ARM_EDMA:
        if (!lvgl_edma_copy_rect(dp, stride, sp, stride, (uint32_t)c.w * F.bpp, c.h)) err = "EDMA_ERR";
        break;
    }
    *us = cycles_us(ARM_DWT_CYCCNT - t0);
    return err;
}

void setup()
{
    Serial1.begin(115200);
    while (!Serial1 && millis() < 2000) {}
    Serial1.println("=== BOOT ===");
    Serial1.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n", "lvgl_pxp_copy_bench", 2, __DATE__, __TIME__);
    Serial1.println("PXP_COPY_BENCH_BEGIN");

    /* The panel scans out for the whole run: SDRAM contention as in the
     * pipeline.  Its framebuffer is MipiDisplay's own; the bench's buffers
     * are separate.  Recorded as a token so the transcript states the
     * condition the timings were taken under. */
    const bool scan = Display.begin();
    if (scan) Display.fillScreen(0x07E0);
    Serial1.printf("SCANOUT=%d\n", scan ? 1 : 0);

    s_src = alloc_buf(); s_dst = alloc_buf();
    if (!s_src || !s_dst) { Serial1.println("ALLOC_FAIL"); Serial1.println("PXP_COPY_BENCH_DONE"); return; }
    Serial1.println("ALLOC_OK");

    lvgl_rt1176_begin();   /* lv_init: the default draw-buf handlers exist */

    bool pxp_up = PXP.begin();          /* idempotent: fillScreen may have begun it */
    uint32_t ctrl = PXP_CTRL;
    if (!pxp_up || (ctrl & (PXP_CTRL_SFTRST | PXP_CTRL_CLKGATE))) {
        Serial1.println("PXP_FAIL");
        Serial1.println("PXP_COPY_BENCH_DONE");
        return;
    }
    Serial1.println("PXP_OK");

    if (!lvgl_edma_copy_begin()) {
        Serial1.println("EDMA_FAIL");
        Serial1.println("PXP_COPY_BENCH_DONE");
        return;
    }
    Serial1.println("EDMA_OK");

    bool all_ok = true;
    uint32_t printed = 0;

    for (const Fmt &F : FMTS) {
        const uint32_t stride = BUF_W * F.bpp;
        if (lv_draw_buf_init(&s_src_db, BUF_W, BUF_H, F.cf, stride, s_src, stride * BUF_H) != LV_RESULT_OK ||
            lv_draw_buf_init(&s_dst_db, BUF_W, BUF_H, F.cf, stride, s_dst, stride * BUF_H) != LV_RESULT_OK) {
            Serial1.println("DRAWBUF_FAIL");
            Serial1.println("PXP_COPY_BENCH_DONE");
            return;
        }

        for (uint8_t i = 0; i < NUM_CASES; i++) {
            const Case &c = CASES[i];
            lv_area_t area;
            area.x1 = c.x; area.y1 = c.y;
            area.x2 = (int32_t)c.x + c.w - 1; area.y2 = (int32_t)c.y + c.h - 1;

            for (const Run &R : RUNS) {
                if (R.only8888 && F.bpp != 4u) continue;
                fill_buf(s_src, F.seed_a, R.src == SRC_XFF);
                uint32_t ref_us = 0, us = 0;
                const uint32_t ref = reference(F, c, area, R.con, &ref_us);
                const char *err = run_arm(R.arm, F, c, &us);
                const uint32_t got = dst_sum();
                if (err) {
                    all_ok = false;
                    Serial1.printf("CASE i=%u f=%s arm=%s src=%s r=%ux%u+%u+%u ERR=%s\n",
                                   (unsigned)(i + 1), F.tag, ARM_TAG[R.arm], SRC_TAG[R.src],
                                   c.w, c.h, c.x, c.y, err);
                    continue;
                }
                const bool ok = (ref == got);
                all_ok = all_ok && ok;
                printed++;
                Serial1.printf("CASE i=%u f=%s arm=%s src=%s r=%ux%u+%u+%u",
                               (unsigned)(i + 1), F.tag, ARM_TAG[R.arm], SRC_TAG[R.src],
                               c.w, c.h, c.x, c.y);
                Serial1.printf(" REF=0x%08lX GOT=0x%08lX %s ref_us=%lu us=%lu\n",
                               (unsigned long)ref, (unsigned long)got, ok ? "MATCH" : "MISMATCH",
                               (unsigned long)ref_us, (unsigned long)us);
            }
        }
    }

    Serial1.println("NOTE timings are hardware-only; QEMU numbers are vacuous");
    Serial1.printf("CASES=%lu\n", (unsigned long)printed);
    if (all_ok) Serial1.println("COPY_BENCH_OK");
    Serial1.println("PXP_COPY_BENCH_DONE");
}

void loop() {}
```

- [ ] **Step 3: Build and run in QEMU, read the structure**

Run:
```bash
cd examples/display/lvgl_pxp_copy_bench && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake > /dev/null && cmake --build build 2>&1 | grep -E "error|text" ; ./run_qemu.sh > /tmp/bench_gate.txt 2>&1; grep -a -c "^CASE .* MATCH " pxp_copy_bench.uart; grep -a "MISMATCH\|ERR=\|SCANOUT\|EDMA_OK\|CASES=" pxp_copy_bench.uart | head
```
Expected: `140` MATCH lines, no MISMATCH/ERR, `SCANOUT=1`, `EDMA_OK`, `CASES=140`. (The OLD gate script fails on the count — that is Step 4's job.) If `pxp_aff src=mixed` MISMATCHes in QEMU, Task 1's model change is wrong; if `edma` MISMATCHes, compare `GOT` against the `cpu_clib` line of the same case: an equal sum means the copy is right and the contract application is wrong, an unequal one means a band/offset bug — the host suite's cases are the first place to add the failing shape.

- [ ] **Step 4: Rewrite the gate**

Replace `examples/display/lvgl_pxp_copy_bench/run_qemu.sh` with:

```sh
#!/bin/sh
# lvgl_pxp_copy_bench -- five sync-copy arms against LVGL's lv_draw_buf_copy,
# each on its own byte contract (NEW-55; v7's PXP contract kept as arm=pxp_x0).
# Demonstrated RED (NEW-55): the edma arm's lines stripped from the fixture
# and a MISMATCH injected on a pxp_aff line both fail by name (vacuity suite).
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
# 140 cases, each a 3.7 MB fill + copy + checksum twice over: ~30 s in QEMU.
QRUN_TIMEOUT="${QRUN_TIMEOUT:-90}"; export QRUN_TIMEOUT
ELF="$DIR/$(gate_build_dir)/lvgl_pxp_copy_bench.elf"; OUT="$DIR/pxp_copy_bench.uart"
rm -f "$OUT" "$DIR/pxp_copy_bench.dbg"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/pxp_copy_bench.dbg" &
P=$!; gate_pid $P
# Poll for the DONE token; ceiling 75 s (300 x 0.25).
i=0
while [ $i -lt 300 ]; do
    [ -f "$OUT" ] && grep -q "PXP_COPY_BENCH_DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
    i=$((i+1))
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

grep -q "^SCANOUT=1" "$OUT" || { echo "FAIL: panel not scanning out -- the timings would not be the pipeline's"; exit 1; }
grep -q "ALLOC_OK" "$OUT" || { echo "FAIL: extmem alloc"; exit 1; }
grep -q "PXP_OK"   "$OUT" || { echo "FAIL: PXP bring-up"; exit 1; }
grep -q "^EDMA_OK" "$OUT" || { echo "FAIL: eDMA channel not claimed"; exit 1; }
# Every arm must MATCH its contract on every case, by name.  Space-anchored so
# f=565 cannot satisfy f=8888, i=1 cannot alias i=10..14, and src=xff cannot
# satisfy src=mixed.
check() { # <i> <fmt> <arm> <src>
    grep -q "^CASE i=$1 f=$2 arm=$3 src=$4 " "$OUT" && \
        grep "^CASE i=$1 f=$2 arm=$3 src=$4 " "$OUT" | grep -q " MATCH " || \
        { echo "FAIL: case f=$2 arm=$3 src=$4 i=$1 did not match"; exit 1; }
}
for i in $(seq 1 14); do
    for f in 565 8888; do
        check $i $f cpu_clib mixed
        check $i $f pxp_x0   mixed
        check $i $f edma     mixed
    done
    check $i 8888 pxp_aff  xff
    check $i 8888 pxp_aff  mixed
    check $i 8888 pxp_affa xff
    check $i 8888 pxp_affa mixed
done
grep -q "MISMATCH" "$OUT" && { echo "FAIL: at least one case mismatched"; exit 1; }
grep -q " ERR=" "$OUT" && { echo "FAIL: at least one arm errored"; exit 1; }
# The count pin catches a matrix edit that forgot the loops above.
grep -q "^CASES=140$" "$OUT" || { echo "FAIL: case count"; exit 1; }
grep -q "COPY_BENCH_OK" "$OUT" || { echo "FAIL: bench verdict withheld"; exit 1; }
# Timings are NOT asserted anywhere: hardware-only, vacuous in QEMU.
[ -f "$DIR/pxp_copy_bench.dbg" ] || { echo "FAIL: no guest-error log"; exit 1; }
grep -q "guest" "$DIR/pxp_copy_bench.dbg" && { echo "FAIL: guest errors"; exit 1; }
echo "PASS: five sync-copy arms match their contracts across the matrix"
```

Run: `./run_qemu.sh | tail -1; echo "gate=$?"` → `PASS…`, `gate=0`. Note the wall time; it must stay well under the runner's 120 s budget.

- [ ] **Step 5: (retired) the fake QEMU and the `-D` log** — `tools/qrun` intercepts `-D` and creates the log itself before the (real or fake) QEMU runs, so the bench gate's `.dbg` assertion already holds under vacuity replay; nothing to change in the fake (a branch was written, found inert in review, and dropped).

- [ ] **Step 6: Re-capture the fixture and add the vacuity section**

Run: `cp examples/display/lvgl_pxp_copy_bench/pxp_copy_bench.uart examples/display/lvgl_pxp_copy_bench/transcript_qemu.txt` (the fixture is a raw capture, 35 lines before this; check with `head -3` that it starts with `=== BOOT ===`).

Append to `tools/gate-vacuity.test.sh` before `exit $FAILED` (line 1067):

```sh
# --- 17. lvgl_pxp_copy_bench: green fixture passes; an arm's lines stripped
# and a MISMATCH injected fail by name (NEW-55).  The arms are the probe that
# chose the db pipeline's sync copy: a gate that could pass without one of
# them would let a dead arm read as measured.
PCB="examples/display/lvgl_pxp_copy_bench"
if [ -d "$EVKB/$PCB" ] && [ -f "$EVKB/$PCB/transcript_qemu.txt" ]; then
    run_gate "$PCB" "run_qemu.sh" "$EVKB/$PCB/transcript_qemu.txt"; rc=$?
    [ "$rc" -eq 0 ] && result=0 || result=1
    report "green_still_passes_lvgl_pxp_copy_bench" $result

    grep -v "arm=edma " "$EVKB/$PCB/transcript_qemu.txt" > "$WORK/pcb_noedma.txt"
    run_gate "$PCB" "run_qemu.sh" "$WORK/pcb_noedma.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "^FAIL: case f=565 arm=edma src=mixed i=1 did not match" || result=1
    report "pxp_copy_bench_missing_arm_fails_by_name" $result

    # Exactly the i=1 pxp_aff mixed-source line (r=16x16+0+0 is unique to
    # case 1) flipped to MISMATCH; the cmp proves the mutant applied, since an
    # unmatched sed would replay the green fixture and pass vacuously.
    sed 's|\(arm=pxp_aff src=mixed r=16x16+0+0 REF=0x........ GOT=0x........\) MATCH |\1 MISMATCH |' \
        "$EVKB/$PCB/transcript_qemu.txt" > "$WORK/pcb_mismatch.txt"
    result=0
    cmp -s "$EVKB/$PCB/transcript_qemu.txt" "$WORK/pcb_mismatch.txt" && { echo "FAIL: mismatch mutant did not apply"; result=1; }
    run_gate "$PCB" "run_qemu.sh" "$WORK/pcb_mismatch.txt"; rc=$?
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "^FAIL: case f=8888 arm=pxp_aff src=mixed i=1 did not match" || result=1
    report "pxp_copy_bench_mismatch_fails_by_name" $result
else
    echo "SKIP: lvgl_pxp_copy_bench vacuity (example or fixture missing)"
fi
```

(The `sed` edits exactly the `i=1` `pxp_aff src=mixed` line: `r=16x16+0+0` is unique to case 1.)

- [ ] **Step 7: Run the vacuity suite**

Run: `tools/gate-vacuity.test.sh > /tmp/vac.txt 2>&1; grep -c "^PASS:" /tmp/vac.txt; grep -E "^FAIL:|pxp_copy_bench" /tmp/vac.txt`
Expected: **73** PASS (70 + the three new), no FAIL. ~18 min; nothing else running.

- [ ] **Step 8: Commit**

```bash
git add examples/display/lvgl_pxp_copy_bench tools/gate-vacuity.test.sh && git commit -m "lvgl_pxp_copy_bench: five sync-copy arms (newlib memcpy, PXP X:=0, PXP alphaOut 0xFF on RGB888 and on ARGB8888, eDMA bands) each on its own byte contract, panel scanning out, 140 cases pinned; two vacuity negatives (NEW-55)"
```

---

### Task 5: Bench session 1 — the probe (operator at the board)

**Files:**
- Modify: `examples/display/lvgl_pxp_copy_bench/transcript_hw_evkb.txt` (append a NEW-55 section)

- [ ] **Step 1: Flash and boot twice**

```bash
tools/rt1170-flash.sh --unlock; tools/rt1170-flash.sh examples/display/lvgl_pxp_copy_bench/build/lvgl_pxp_copy_bench.hex
```
Operator presses SW4; wait for `PXP_COPY_BENCH_DONE` in `/tmp/rt1170/serial.log` (the full run is ~2 min on silicon: 182 checksums of 3.7 MB); SW4 again for boot 2.

- [ ] **Step 2: Extract the decision table**

```bash
tr -d '\r\000' < /tmp/rt1170/serial.log | grep -a "^CASE i=14 f=8888" ; tr -d '\r\000' < /tmp/rt1170/serial.log | grep -a -c "MISMATCH\| ERR="
```
Expected shape per boot (values are the measurement):
```
CASE i=14 f=8888 arm=cpu_clib src=mixed r=720x1280+0+0 REF=… GOT=… MATCH ref_us=~260000 us=?
CASE i=14 f=8888 arm=pxp_x0   src=mixed … MATCH … us=~13000
CASE i=14 f=8888 arm=pxp_aff  src=xff   … MATCH … us=~13000
CASE i=14 f=8888 arm=pxp_aff  src=mixed … MATCH …
CASE i=14 f=8888 arm=pxp_affa src=xff   … MATCH … us=~13000
CASE i=14 f=8888 arm=pxp_affa src=mixed … MATCH …
CASE i=14 f=8888 arm=edma     src=mixed … MATCH … us=?
```
Every REF/GOT pair byte-identical between the two boots; zero MISMATCH/ERR. `pxp_affa` has two rows: both MATCH means the bit is honoured at OUT encoding 0x00 (if `pxp_aff` MISMATCHed, the handler then declares ARGB8888 surfaces -- same bytes); `xff` MATCH with `mixed` MISMATCH means 0x00 merely forwards the PS alpha byte (still byte-identical for LVGL's 0xFF buffers, but a different mechanism -- record it as such); every alpha row MISMATCH refutes the override (record it; eDMA carries §6); an `edma` MISMATCH or `EDMA_ERR` is a bug to fix before deciding (compare against `cpu_clib`'s GOT for the same case).

- [ ] **Step 3: Apply the decision rule and write it down**

Winner = the fastest byte-preserving arm (`pxp_aff` on the xff source, `pxp_affa` if only the 0x00 encoding honours the bit -- then the handler declares ARGB8888 surfaces -- or `edma`) with `us < 33000` on `i=14 f=8888`. Append to `transcript_hw_evkb.txt` a section `NEW-55 PROBE, <date>` with: the build stamp, both boots' `i=14` lines for every arm, the `i=1`/`i=2` (16×16) and `i=10` (719×1) lines for every arm (the crossover inputs), the count of MATCH lines (`224` over two boots), the decision and the rule it followed, and the no-scanout v6 column beside the new scanout numbers. Commit: `git add examples/display/lvgl_pxp_copy_bench/transcript_hw_evkb.txt && git commit -m "lvgl_pxp_copy_bench: NEW-55 probe benched -- <winner> wins at <us> us per full frame (cpu_lv <ref_us>, cpu_clib <us>, pxp_x0 <us>, edma <us>), every arm MATCH on two boots"`.

Then release the board: `tools/rt1170-flash.sh --unlock`.

---

### Task 6: Install the winner from `create_db()`

Two variants; execute the one the probe chose. Both share Steps 6.3–6.7.

**Files (both variants):**
- Modify: `~/Development/LVGL/port/lvgl_mipi_panel.cpp:266-295`, `lvgl_mipi_panel.h`
- Modify: the 15 `CMakeLists.txt` under `examples/display/` that compile `port/lvgl_mipi_panel.cpp` and do not yet compile the handler (all but `lvgl_rk055_flip_test`, `lvgl_rk055_touch_test`): `lvgl_rk055_panel_test acid_box lvgl_rpi_panel_test synthui_knob_test rotary_knob_bench synthui_slide_toggle_test synthui_lamp_test synthui_led_button_test synthui_fader_test synthui_piano_key_test synthui_level_meter_test synthui_panel_button_test synthui_seven_segment_test vglite_lvgl_test synthui_step_test`
- Modify: `examples/display/lvgl_rk055_flip_test/lvgl_rk055_flip_test.cpp:31,113-122,203-207`, `run_qemu.sh`; same for `lvgl_rk055_touch_test` (`:50,298-306`, its `PXP_COPIES` prints and `run_qemu.sh:44-45,92-93`)
- Modify: `examples/display/synthui_led_button_test/synthui_led_button_test.cpp`, `run_qemu.sh`

#### 6a — the PXP alpha override won

- [ ] **Step 6a.1: The handler applies the override on 32-bit copies**

In `~/Development/LVGL/port/lvgl_pxp_copy.cpp:113`, replace:

```cpp
    if (PXP.blit(ssrc, sdst) != PXP_OK) {
```

with:

```cpp
    /* NEW-55: a 32-bit copy carries alphaOut(0xFF).  Without it the PXP
     * writes its computed alpha -- 0 -- into byte 3 (v7's measured X:=0);
     * with it the copy is byte-identical to the CPU copy of any buffer whose
     * X byte is 0xFF, which is every LVGL-rendered XRGB8888 buffer (opaque
     * fills write 0xFF, blends touch bytes 0-2 only).  Silicon-verified:
     * lvgl_pxp_copy_bench arm=pxp_aff, two boots.  16-bit copies are
     * unchanged (no alpha byte; alphaOut would be PXP_ERR_CONFIG). */
    PXPOp op = PXP.op();
    op.source(ssrc).output(sdst);
    if (bpp == 4u) op.alphaOut(0xFF);
    if (op.run() != PXP_OK) {
```

In `lvgl_pxp_copy.h`, replace the paragraph beginning `MEASURED 32-BIT CONTRACT` with:

```
 * 32-BIT CONTRACT (NEW-55, superseding v7's X:=0): the copy carries
 * alphaOut(0xFF), so byte 3 of every copied pixel is 0xFF -- byte-identical
 * to a CPU copy of an LVGL-rendered XRGB8888 buffer (whose X byte is always
 * 0xFF) and therefore invisible to every db checksum and delta-equality
 * guard.  It is NOT byte-preserving for a source whose X byte is not 0xFF;
 * ARGB8888 (where that byte means something) stays excluded above.
```

- [ ] **Step 6a.2: `create_db()` installs it**

In `~/Development/LVGL/port/lvgl_mipi_panel.cpp`, add `#include "lvgl_pxp_copy.h"` after `#include "lvgl_mipi_panel.h"`, and in `lvgl_mipi_panel_create_db()` after `lv_display_set_buffers(...)` (`:289-290`) and before `lcdifv2AttachVsyncInterrupt` insert:

```cpp
    /* NEW-55: the double-buffer SYNC COPY (refr_sync_areas -- the previous
     * frame's rendered areas copied into the buffer about to be drawn) goes
     * through the PXP.  LVGL's default is lv_memcpy's word loop over uncached
     * SDRAM: 260 ms for a full frame with the panel scanning out (NEW-53
     * finding A), i.e. ~515 ms of dead UI over the two refreshes that follow
     * any full-screen change.  Threshold from the NEW-55 probe
     * (lvgl_pxp_copy_bench transcript_hw_evkb.txt): <fill in from the 16x16
     * lines -- the area below which the CPU copy is faster>. */
    lvgl_pxp_copy_install(LVGL_MIPI_SYNC_COPY_THRESHOLD_PX);
```

and near the top of the file:

```cpp
/* Sync-copy crossover, px.  MEASURED (NEW-55 probe, i=1/i=2 16x16 cases):
 * replace 0 with the largest area at which the CPU copy still beat the
 * accelerator, or leave 0 if the accelerator won every multi-row case. */
static constexpr uint32_t LVGL_MIPI_SYNC_COPY_THRESHOLD_PX = 0;
```

#### 6b — the eDMA won

- [ ] **Step 6b.1: `create_db()` installs the eDMA handler**

As 6a.2 but `#include "lvgl_edma_copy.h"` and `lvgl_edma_copy_install(LVGL_MIPI_SYNC_COPY_THRESHOLD_PX);`, with the comment naming the eDMA and its measured full-frame time. The PXP handler stays as it was (v7's X:=0 contract, used by nothing in the tree after Step 6.4).

#### Shared

- [ ] **Step 6.3: Sync counters through the port**

In `lvgl_mipi_panel.h` add:

```cpp
/* NEW-55: the db pipeline's sync-copy handler, as installed by create_db().
 * copies = rectangles the accelerator moved; fallbacks = chained to the CPU
 * (shape or error); errors = the accelerator itself failed (0 on any healthy
 * run -- adopting gates pin it, and assert copies > 0 so a handler that is
 * installed but never engages cannot read as adopted). */
uint32_t lvgl_mipi_panel_sync_copies();
uint32_t lvgl_mipi_panel_sync_copy_fallbacks();
uint32_t lvgl_mipi_panel_sync_copy_errors();
```

and in `lvgl_mipi_panel.cpp` (after `lvgl_mipi_panel_wait_us`):

```cpp
uint32_t lvgl_mipi_panel_sync_copies()         { return lvgl_pxp_copies(); }
uint32_t lvgl_mipi_panel_sync_copy_fallbacks() { return lvgl_pxp_copy_fallbacks(); }
uint32_t lvgl_mipi_panel_sync_copy_errors()    { return lvgl_pxp_copy_errors(); }
```
(6b: the `lvgl_edma_*` getters instead.)

- [ ] **Step 6.4: The two rk055 tests drop their own install**

In `lvgl_rk055_flip_test.cpp`: delete `#include "lvgl_pxp_copy.h"` (`:31`) and the block `:113-122` (the v6 comment and `lvgl_pxp_copy_install(1024);`), replacing it with:

```cpp
    /* The sync-copy handler is installed by lvgl_mipi_panel_create_db()
     * since NEW-55 (it was this example's own lvgl_pxp_copy_install(1024)
     * from v6 to then); the PXP_* tokens below read the port's counters. */
```
and change `:205-207` to print through the port:
```cpp
    Serial1.printf("PXP_COPIES=%lu\n", (unsigned long)lvgl_mipi_panel_sync_copies());
    Serial1.printf("PXP_FALLBACKS=%lu\n", (unsigned long)lvgl_mipi_panel_sync_copy_fallbacks());
    Serial1.printf("PXP_ERRORS=%lu\n", (unsigned long)lvgl_mipi_panel_sync_copy_errors());
```
Same edits in `lvgl_rk055_touch_test.cpp` (`:50`, `:298-306`, its three prints). Their `CMakeLists.txt` keep `port/lvgl_pxp_copy.cpp` (6a) — or swap it for `port/lvgl_edma_copy.cpp` (6b). The `FLIP_DEMO_SINGLE` variant of the flip test calls `lvgl_mipi_panel_create()` (no db, no sync) and is unaffected.

- [ ] **Step 6.5: The 15 CMakeLists compile the handler**

For each listed example, in `teensy_add_executable(...)` add the line `${_lvgl_dir}/port/lvgl_pxp_copy.cpp` (6a) or `${_lvgl_dir}/port/lvgl_edma_copy.cpp` (6b) after `${_lvgl_dir}/port/lvgl_mipi_panel.cpp`. Every one of them already has `evkb_library_dir(LVGL _lvgl_dir)` and links `PXP` (verified 2026-09-18: `grep -L PXP` over the 17 is empty). An example that is missed fails to LINK (`undefined reference to lvgl_pxp_copy_install`) — loud, which is the point of this layout.

Rebuild all 17 (each: `cmake --build build`; acid_box also `build-bt`, `build-bench`, `build-loopstat` — read `--print-memory-usage` for its ITCM headroom: the handler is referenced only by `create_db`, which acid_box never calls, so `--gc-sections` drops both and the headroom must read 2868 ±16 B; a change of 32 B or more is a real cost to explain).

- [ ] **Step 6.6: `synthui_led_button_test` witnesses the handler**

In `synthui_led_button_test.cpp`, after the `led_button_mem` print, add:

```cpp
    Serial1.printf("led_button_sync copies=%lu fallbacks=%lu errors=%lu\n",
                   (unsigned long)lvgl_mipi_panel_sync_copies(),
                   (unsigned long)lvgl_mipi_panel_sync_copy_fallbacks(),
                   (unsigned long)lvgl_mipi_panel_sync_copy_errors());
```

In its `run_qemu.sh`, after the vsync check (`:192`), add:

```sh
# NEW-55: the db pipeline's accelerated sync copy must have ENGAGED (the
# rk055 IDLE_POLLS idiom -- an installed handler that never runs would leave
# every golden green and the 260 ms copy in place) and never errored.
grep -qE "led_button_sync copies=[1-9][0-9]* fallbacks=[0-9]+ errors=0\r?$" "$OUT" || { echo "FAIL: sync-copy handler did not engage cleanly"; exit 1; }
```

- [ ] **Step 6.7: Re-record the two rk055 goldens; every other golden must hold**

Run the eleven db gates plus acid_box:
```bash
for e in synthui_knob_test synthui_slide_toggle_test synthui_lamp_test synthui_led_button_test synthui_fader_test synthui_piano_key_test synthui_level_meter_test synthui_panel_button_test synthui_seven_segment_test acid_box rotary_knob_bench vglite_lvgl_test synthui_step_test lvgl_rk055_panel_test lvgl_rpi_panel_test; do (cd examples/display/$e && ./run_qemu.sh > /tmp/g_$e.txt 2>&1; echo "$e $(tail -1 /tmp/g_$e.txt)"); done
```
(The last five do not use `create_db` — they are the compile-only consumers of the CMake change, run here so a link or placement fault shows before the sweep.)
Expected: fifteen PASS lines with NO golden moved (a `FAIL: … checksum` here means the copy is not byte-preserving — STOP, that is the spec's primary check; under 6a it means an LVGL write path left X≠0xFF, find it with the delta-equality mismatch before touching a golden).

Then `lvgl_rk055_flip_test` and `lvgl_rk055_touch_test`: run each twice; the moved tokens (`LVGL_SUM=`, the flip sums / MATCH pair) must be identical across the two runs; update `run_qemu.sh` with the new values and the reason (`re-recorded 2026-09-xx, NEW-55: the sync copy now preserves X (0xFF) where v6's PXP copy wrote 0; two bit-identical runs`), keeping the old value in the comment. Re-run: PASS. Re-capture both fixtures (`cp` the uart to `transcript_qemu.txt` — confirm each is a raw capture first with `head -3`).

- [ ] **Step 6.8: Commit**

LVGL: `git -C ~/Development/LVGL add port && git -C ~/Development/LVGL commit -m "port: create_db installs the <winner> sync-copy handler; sync counters exposed (NEW-55)"`.
evkb: `git add examples/display evkb.cmake && git commit -m "db pipeline: <winner> sync copy installed from create_db for all 11 consumers; rk055 goldens re-recorded (X now preserved); led_button_sync witness (NEW-55)"`.

---

### Task 7: Bench session 2 — ratification (operator at the board)

- [ ] **Step 1: `synthui_led_button_test` — the number**

Flash its `.hex`, two boots. Read:
```bash
tr -d '\r\000' < /tmp/rt1170/serial.log | grep -a "led_button_transition\|led_button_fps\|led_button_sync\|led_button_crc\|fresh_crc\|delta_eq\|timeouts"
```
Expected: `led_button_transition i=1 … sync_us=<N>` with **N < 33000** (was 272,283); `led_button_sync copies>0 errors=0`; goldens `0xD474F06D` / `0x463C3371`, `delta_eq=PASS`, `timeouts=0`; `us_max` ~85 ms and `us_med` within noise of 27.7 ms. If `sync_us` is not under 33 ms the acceptance is NOT met: record the number and stop to discuss (the spec's decision rule said the eDMA proceeds regardless, but a miss here is a design conversation, not a re-golden).

- [ ] **Step 2: The GPU veto — fader and knob equality on silicon**

Flash `synthui_fader_test` (its `build-vglite`/GPU configuration as its transcript describes) and `synthui_knob_test`; two boots each. Expected: `fd_delta_eq=PASS`, `fd_crc=0x814F4047`, `delta==fresh=0xE9A9A2B5`; knob `KNOB_DELTA_SEQ=FULL=0x7C9EC8DB` and its six `KNOB_SUM_*` unmoved, `gpu_err=0`. **Under 6a a `delta_eq=FAIL` here is the veto**: the GC355 wrote X≠0xFF somewhere and the alpha override changed it — switch to 6b (eDMA) and repeat Task 6 from Step 6b.1; the 6a work stays in the PXP library and handler as a documented, un-adopted capability.

- [ ] **Step 3: The two rk055 goldens on silicon, and acid_box as the control**

Flash `lvgl_rk055_flip_test`, `lvgl_rk055_touch_test`: the re-recorded tokens must equal QEMU's (the model's alpha override was written to make that true). Flash acid_box (`build`): `ACIDBOX_UI_SUM=0xA67828E9`, `ACIDBOX_ROT_EQ … mismatch=0 starved=0`, `full<=2` over a few bars, `PLAY_LIT=0` — nothing here touches its pipeline, so nothing may move.

- [ ] **Step 4: Record**

Append a `NEW-55 RATIFICATION` section to `synthui_led_button_test/transcript_hw_evkb.txt` (the transition lines from both boots, before/after `sync_us`, the sync counters) and a line each to the fader, knob, rk055 and acid_box transcripts. Commit: `git commit -am "NEW-55 ratified on silicon: led_button_transition sync_us 272283 -> <N>; nine synthui goldens + every equality guard unmoved, GPU equality (fader/knob) unmoved, rk055 goldens equal QEMU's, acid_box control unmoved"`. Release the board.

---

### Task 8: Close-out

- [ ] **Step 1: Pins and fresh-user**

Push LVGL (`git -C ~/Development/LVGL push`), bump its SHA in `evkb.cmake:130` with the reason (`sync-copy handler installed from create_db, eDMA handler + band planner, NEW-55`). Fresh-user: in `examples/display/synthui_led_button_test`, `cmake -B build-fetch -DEVKB_FORCE_FETCH=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake > /tmp/fetch.log 2>&1 && cmake --build build-fetch 2>&1 | tail -2`; confirm `/tmp/fetch.log` shows the LVGL and PXP clones at the new pins; run the gate against the fetched ELF (`mv build build-local && ln -s build-fetch build && ./run_qemu.sh | tail -1; rm build && mv build-local build`). Expected: PASS, `led_button_sync copies>0`.

- [ ] **Step 2: Rebuild every gate image whose inputs moved, then the sweep**

The LVGL pin moved: rebuild the 17 panel examples' build dirs (done in 6.5) and the self-building gates' dirs (`bt_sink_test/build`, the `bt_tone_test` variants, `m2_hci_probe`'s) so a reconfigure does not eat their 120 s budget. Then, nothing else running:
```bash
./tools/run-all-qemu-gates.sh > /tmp/sweep-new55.txt 2>&1; tail -3 /tmp/sweep-new55.txt
```
Expected: `gates: 141 passed` (no new gate). Any red: re-run idle before dispositioning; the load-sensitivity class is documented in `docs/KNOWN-BROKEN-GATES.md`.

- [ ] **Step 3: Vacuity and audit, sequentially**

`tools/gate-vacuity.test.sh` → 73/73. `LICENSE_AUDIT_EVKB=$(pwd) tools/license-audit.sh | tail -1` → `LICENSE-AUDIT: PASS` (the new port sources are MIT; the `GATES` list needs no change — no new gate).

- [ ] **Step 4: Documentation**

- `CLAUDE.md`: a ✅ Measured block for this close-out (sweep, vacuity 73, audit, pins), and the substance: the probe's table (all four arms, full frame, scanout), the winner and why, `led_button_transition sync_us` before/after, the X≡0xFF fact about LVGL's renderer, **the qemu2 floor** (Task 1's SHA: "an older model leaves X:=0 in synced regions and every db delta-equality guard fails by name: `delta render differs from full render`"), the rk055 goldens' move and reason, the 15-CMakeLists layout rule (a db consumer that forgets the handler source fails to link), acid_box's ITCM reading, and the fake-qemu `-D` fix. Update the NEW-53 finding A entry's "NOT fixed, deliberately" paragraph with a pointer to this block.
- `examples/README.md`: the bench row (four arms, 112 cases) and the led_button row (`led_button_sync`).
- `docs/superpowers/specs/2026-09-18-db-sync-copy-acceleration-design.md`: a "Measured" postscript with the decision.
- Linear NEW-55: the result, marked Done; NEW-53: one line pointing at it.

- [ ] **Step 5: Final commit and hand-off**

`git add -A && git commit -m "docs: NEW-55 close-out -- <winner> sync copy measured (sweep 141/141/0, vacuity 73/73, audit PASS), qemu2 floor <sha>, pins bumped"`. Then use `superpowers:finishing-a-development-branch` (merge to master and push on the user's word).

---

## Self-review notes (written before hand-off)

- Spec §3 (scanout, per-arm oracle, 28 rectangles, decision rule) → Task 4/5. §4.1 → Task 2. §4.2 → Task 1. §5 (bands, preconditions, counters) → Task 3. §6.1–6.4 → Task 6/8. §6.3's veto → Task 7 Step 2. §7 (host suite, vacuity, gates) → Tasks 3/4/8.
- Type consistency: `lvgl_edma_copy_rect(dst, dst_stride, src, src_stride, row_bytes, rows)` is used identically in the handler, the bench and the header; `lvgl_edma_plan_bands` returns `int`, band fields `src/dst/bytes/rows/mloff/xfer` match between the header, the test and `run_band`.
- The one unknown the plan cannot pre-fill is `LVGL_MIPI_SYNC_COPY_THRESHOLD_PX` and the winner's name — both are outputs of Task 5 and are marked as such rather than guessed.
