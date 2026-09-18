# db pipeline sync copy acceleration — design (NEW-55)

**Date:** 2026-09-18
**Issue:** NEW-55 — the db pipeline's double-buffer sync copy is a 260 ms CPU word loop per full frame
**Follows:** NEW-53 finding A (`examples/display/synthui_led_button_test/transcript_hw_evkb.txt`, "NEW-53 FINDING A")
**Decisions taken in brainstorming:** probe all three candidates on the bench before designing around one; install the winner in `lvgl_mipi_panel_create_db()` for every db consumer; re-record the two rk055 goldens whose X:=0 was the v6 copy's artefact.

## 1. The problem, measured

`lvgl_mipi_panel_create_db()` runs LVGL in `LV_DISPLAY_RENDER_MODE_DIRECT` with two full 720×1280×4 framebuffers. LVGL records the areas each refresh rendered, and the **next** refresh begins (`refr_sync_areas`, `lv_refr.c`) by copying those areas from the on-screen buffer into the buffer it is about to draw, so a partial render lands on an up-to-date backdrop. That copy is `lv_draw_buf_copy`'s default handler: `lv_memcpy`'s 32-bit word loop, row by row, over uncached SDRAM.

After a full-screen render — a screen load, or any tick that dirties more than `LV_INV_BUF_SIZE` (32) areas, on which LVGL substitutes the whole screen — the sync is the whole frame:

```
led_button_transition i=0 us=227619 render_us=227519 px=921600 ev=44 inv=1 full=1
led_button_transition i=1 us=287979 sync_us=272283 render_us=15576 wait_us=11649 px=6552 sync_px=921600
```

**260 ms for 3.7 MB, 14 MB/s**, with the panel scanning out (five boots to 0.05 %). The v6 bench (`lvgl_pxp_copy_bench`, case 14, XRGB8888) measured the same copy at 174 ms with no scanout; the LCDIFv2's ~221 MB/s of SDRAM reads are the difference. So a full-screen change in the db pipeline costs ~515 ms over two refreshes. Steady-state syncs are a few thousand px and cheap.

Consumers: the eleven `create_db` users — nine `display/synthui_*_test` plus `lvgl_rk055_flip_test` and `lvgl_rk055_touch_test` (which already install v6's PXP copy explicitly). `acid_box` is not affected: its rotated pipeline is a single canvas with no sync.

### 1.1 Why v6's PXP copy is not a drop-in

`lvgl_pxp_copy_install()` does the copy in 13 ms per full frame, but the PXP **writes X:=0 on every 32-bit output** (v6/v7, architectural, `OUT_CTRL[ALPHA_OUTPUT]=0` "retain computed alpha"). LVGL's software renderer writes X=0xFF on every opaque fill (`lv_color_to_u32`) and leaves byte 3 untouched on every blended write (`lv_color_24_24_mix` touches bytes 0–2 only). Every db example paints an opaque screen background first, so **X ≡ 0xFF throughout an LVGL-rendered db buffer**. A synced region with X=0 therefore differs from a rendered one in a byte every db checksum hashes, and each example's delta-equality guard (`led_button_delta_crc`, a mix of synced and rendered regions, must equal `led_button_fresh_crc`, all rendered) would fail. Those guards are the tree's only witness for stale-pixel defects and are not weakened.

## 2. Candidates

Three, all probed on silicon before one is chosen (§3):

| arm | mechanism | byte contract |
|---|---|---|
| `cpu_clib` | newlib `memcpy` per row (LDM/STM) | byte identity — the CPU floor, not a fix |
| `pxp_aff` | `PXP.blit` with `OUT_CTRL[ALPHA_OUTPUT]=1, ALPHA=0xFF` (RM 52.6.3) | RGB identity, X==0xFF: byte identity for any source whose X is 0xFF, i.e. every LVGL buffer |
| `edma` | one eDMA channel, 2-D copy | byte identity, any source |

`cpu_lv` (the existing `lv_draw_buf_copy`) and `pxp_x0` (the existing blit) stay as the reference and the old contract.

**Decision rule, pre-registered:** the winner is the fastest byte-preserving arm whose full-frame copy is under one refresh period (33 ms) with the panel scanning out. If none qualifies, the eDMA design proceeds anyway and the number is recorded. §5.3's GPU equality check can veto the probe's winner.

## 3. The probe — `display/lvgl_pxp_copy_bench` extended

Two harness changes:

- **Scan out while measuring.** `Display.begin()`, the panel's own framebuffer with static content, so every timing carries the pipeline's SDRAM contention. The v6 numbers stay in the transcript as the no-scanout column.
- **Per-arm oracle.** Each arm's line states its own contract and the bench asserts exactly that: `cpu_clib` and `edma` byte-identity with the CPU reference; `pxp_x0` RGB identity with X:=0 in-rect (v6's oracle); `pxp_aff` RGB identity with X==0xFF in-rect, run on BOTH the 0xFF-X source and a source with X≠0xFF, so the override is proven to be what writes the byte. Every byte outside the rectangle untouched (v6's whole-extent checksums).

Same 28 rectangles (14 × {RGB565, XRGB8888}); the `pxp_aff` and `edma` arms run both formats the handler serves (`pxp_aff` only on 32-bit — §4.1). Output per case:

```
CASE i=<n> f=<fmt> r=<w>x<h>+<x>+<y> arm=<arm> REF=0x… GOT=0x… MATCH|MISMATCH us=<n>
```

plus the existing `CASES=`, `COPY_BENCH_OK`, `PXP_COPY_BENCH_DONE`. A missing arm fails the gate by name. Silicon gives the timings (two boots, byte-identical checksums); QEMU gives every arm's correctness once the model knows the alpha override (§4.2).

## 4. The two `pxp_aff` prerequisites

### 4.1 PXP library (`~/Development/PXP`)

One opt-in setter on the op builder, `.alphaOut(uint8_t alpha)`. `_program()` then writes `OUT_CTRL = format | (1u<<23) | (alpha<<24)` instead of the bare format. Default off — acid_box's rotated present, the v8 composites and every existing golden keep their measured X:=0. Set with a 16-bit OUT format it returns `PXP_ERR_CONFIG` ("never a silent plain blit"), so the copy handler applies it only to 32-bit formats. The format switch tables are untouched. README gains a line on what "retain computed alpha" measures to on this silicon and what the override does.

### 4.2 QEMU model (`qemu2/hw/dma/imxrt_pxp.c:331`)

Implement the bit it logs as unmodelled: with `ALPHA_OUTPUT=1`, byte 3 of every 32-bit output pixel is `OUT_CTRL[31:24]`; with it clear, the existing X:=0 behaviour stands (the measured silicon contract). Pushed to gitlab like the IW416 model. Its revision becomes the floor for the eleven db gates: an older qemu2 leaves X:=0 in synced regions, so each example's delta-equality guard fails **by name** ("delta render differs from full render") — the right failure mode, and CLAUDE.md's floor line says so.

If silicon does not honour the bit, `pxp_aff` reads MISMATCH by name on the probe, the arm is recorded as refuted, and §5 proceeds on eDMA.

## 5. The eDMA arm — `LVGL/port/lvgl_edma_copy.cpp`

Same skeleton as `lvgl_pxp_copy.cpp`: `lvgl_edma_copy_install(threshold_px)` saves the default handler, a shape check chains anything unserved to the default, every hardware error and bounded-wait timeout falls back to the CPU copy and increments a counter (`lvgl_edma_copies / _fallbacks / _errors`). One channel claimed through the core's `DMAChannel` at install. Synchronous, like the PXP handler, so LVGL's ordering holds.

### 5.1 The transfer

A rectangle is `h` rows of `w×bpp` bytes with a `stride − w×bpp` skip on both sides — the eDMA minor-loop offset (`NBYTES_MLOFFYES`, `SMLOE|DMLOE`, `MLOFF` = the skip, `CITER=BITER=h`). With offsets enabled `NBYTES` is 10 bits (≤ 1023 bytes), so a full 2880-byte row does not fit one minor loop. The handler copies **vertical bands** of ≤ 960 bytes (240 px at 4 bpp, 480 at 2 bpp): a full 720-px row is three bands, each one TCD run to major completion. Transfer size per band from alignment: 32-byte bursts when the band origin and its byte width are 32-aligned (every full-width sync is), 4-byte otherwise; the v6 bench's odd-offset rectangles (`+13+7`) measure the slow regime. Trigger: the DMAMUX always-on request (`triggerContinuously()`), because a software START serves one minor loop on classic eDMA; `disableOnCompletion()` stops the channel at the major loop; DONE is polled with a bounded wait (100 ms, PXP's cap), after which the channel is disabled so a wedged transfer cannot write late over the CPU fallback.

The band arithmetic — rectangle → bands, transfer size from alignment, `MLOFF`/`NBYTES` packing — is pure and lives in a header (`lvgl_edma_bands.h`) with a host suite (§7).

### 5.2 Preconditions, asserted

1. **D-cache off.** The `imxrt1176` core enables only the I-cache (NEW-36); a DMA copy of a cached buffer would move stale bytes. `install()` reads `SCB_CCR` and refuses (counts a fallback, keeps the CPU copy) if `DC` is set.
2. **Model coverage.** qemu2's eDMA implements minor-loop offsets and START (`imxrt_edma.c`); whether it serves an always-on DMAMUX request is checked in the plan's first task and modelled if not.

### 5.3 Why it stays even if the PXP wins

It preserves every byte with no invariant about what any writer puts in X. The GC355 compositors (`synthui_fader_test`, `synthui_knob_test`) are a second writer of the framebuffer, and what they put in byte 3 is not visible to the probe — under `pxp_aff` a forced 0xFF could differ from the GPU's byte and break those examples' silicon equality guards (`fd_delta_eq`, `KNOB_DELTA_SEQ`); under eDMA it cannot. That check ratifies or vetoes the probe's winner (§6.3).

## 6. Integration

### 6.1 Install point

`lvgl_mipi_panel_create_db()` installs the winning handler after `lv_display_set_buffers`. The two rk055 tests drop their own `lvgl_pxp_copy_install()` calls: one install path. The handler is LVGL-global, as v6's is. `create_rotated()` installs nothing.

### 6.2 Crossover

v6's rule stays (≥ 2 rows, area ≥ threshold); the threshold is a **measured** number from the probe's 16×16 and single-row cases, pinned in the port with its derivation.

### 6.3 What may move — the acceptance

| | expectation | why |
|---|---|---|
| nine `synthui_*_test` sw goldens + every delta-equality guard | **unmoved** | the CPU copy was byte-preserving; the new one must be. One moved byte means the copy is not what it claims |
| `lvgl_rk055_{flip,touch}_test` goldens | **move once**, re-recorded with the reason, QEMU and silicon | their X:=0 was the v6 copy's artefact |
| fader / knob **GPU** goldens and silicon equality guards | **unmoved** | the veto (§5.3) |
| acid_box sw `0x18B7B637`, gpu `0xA67828E9`, `ROT_EQ mismatch=0` | unmoved | no sync path; the control |
| `led_button_transition i=1 sync_us` | 272 ms → **< 33 ms** | the number NEW-55 exists for; `us_max` 85 ms / `us_med` are steady-state controls |

### 6.4 Pins and floors

PXP (if `alphaOut` lands) and LVGL (the port) pushed and pinned; fresh-user verified by running a db gate on a `-DEVKB_FORCE_FETCH=ON` ELF. qemu2's revision becomes the floor for the eleven db gates, named in CLAUDE.md with its failure signature.

## 7. Testing

- **Gates.** No new gate. The bench gate asserts every arm's MATCH by name on every rectangle and pins the case count; its 20 s budget is re-checked. Vacuity gains two negatives: an arm's lines stripped from the fixture fails by name; a MISMATCH injected fails by name. The eleven db gates plus acid_box are the regression, in the full sweep at close-out.
- **Host.** `LVGL/port/tests/` gains the band suite, with mutants that must go red: a band past 1023 bytes, a 32-byte burst chosen on a 4-aligned origin, a skip computed from the width instead of the stride.
- **Silicon.** Session 1: the probe (two boots). Session 2: ratification — nine synthui, two rk055, fader/knob GPU equality, acid_box control, the transition number.

## 8. Order of work

1. qemu2: `ALPHA_OUTPUT` in the PXP model; verify/model the always-on DMAMUX request. Push.
2. PXP `.alphaOut()`, pushed and pinned.
3. eDMA handler, band header, host suite.
4. Bench arms, scanout, per-arm oracles, gate, vacuity, fixture.
5. **Bench session 1** — decision by §2's rule, written down before step 6.
6. `create_db()` install, rk055 cleanup, measured threshold, rk055 goldens re-recorded in QEMU.
7. **Bench session 2** — §6.3.
8. Close-out: sweep, audit, LVGL pin, fresh-user run, CLAUDE.md floor line, NEW-55.

## 9. Out of scope

`LV_INV_BUF_SIZE` (moves the cliff, costs render passes — NEW-50's lesson); the 227 ms full-screen **render** (the sw renderer's cost, a separate finding); ARGB8888 buffers whose alpha means something (the v6 header's reserved alpha-engine design); triple buffering.
