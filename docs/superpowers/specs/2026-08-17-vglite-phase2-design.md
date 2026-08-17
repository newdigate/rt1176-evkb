# VGLite Phase 2 — LVGL's VG_LITE draw unit on the GC355

Date: 2026-08-17. Status: approved in session, pending implementation.
Phase 1 (foundation) is complete and hardware-verified — see
`2026-08-16-vglite-gc355-design.md` and
`examples/display/vglite_probe/transcript_hw_evkb.txt`.

## 1. Goal

Enable `LV_USE_DRAW_VG_LITE` against the MIT VGLite driver in the `VGLite`
sibling repo, so LVGL renders vector UI on the GC355 instead of in software.

**Success criterion, inherited unchanged from the Phase 1 spec §3:** the
**16-knob grid animating at ≥30 fps** (≤33 ms for all sixteen). Software is
~208 ms ⇒ ~5 fps, so this needs ~6×. That number is the whole point of the
project and Phase 2 is the first phase that can measure it.

## 2. The blocker, measured rather than recalled

Phase 1 recorded "LVGL 9.4 queries four feature bits this driver does not
define". **That was wrong** — it came from reading a couple of call sites
during debugging, not from a compile. Measured on 2026-08-17 by extracting
every name LVGL's backend references and diffing against the driver's headers:

| | LVGL 9.4 references | driver defines | missing |
|---|---|---|---|
| `gcFEATURE_BIT_VG_*` | 47 | 9 | **38** |
| error / format enums | 61 | — | **9** |

It is a **driver version gap**, not a feature disagreement: LVGL 9.4's backend
targets a newer NXP VGLite than `VGLITE_HEADER_VERSION 6` / `VGLITE_VERSION_2_0`.

`lv_vg_lite_feature_string()` and `lv_vg_lite_error_string()` are compiled
unconditionally, so **every** name must exist at compile time even though most
are only ever used for logging.

## 3. Two findings that make the shim viable

**3.1 `vg_lite_query_feature()` is bounds-checked by the vendor.**

```c
if (feature < gcFEATURE_COUNT) result = ctx->s_ftable.ftable[feature];
else                           result = 0;
```

Any name defined above the `gcFEATURE_COUNT` sentinel returns 0 — no
out-of-bounds table read. This is a vendor-provided safety valve, so the shim
uses a documented behaviour rather than exploiting an accident.

**3.2 `LVGL_SUPPORT` is not a kill switch.** It is consulted in exactly two
places, both selecting *software* premultiplication
(`lv_vg_lite_utils.c:960`, `lv_draw_vg_lite_img.c:66`). Reporting 0 puts LVGL
on the conservative correct path; it does not disable the backend. Phase 1's
note implied this bit gated the whole thing. It does not.

## 4. Approach (approved): a compatibility shim, driver untouched

A header defining the missing names as macros valued above `gcFEATURE_COUNT`,
force-included for the LVGL translation units. C enums cannot be reopened, so
macros — not enumerators — are the mechanism; `vg_lite_query_feature((vg_lite_feature_t)N)`
compiles and returns 0, and `case gcFEATURE_BIT_VG_X:` stays a valid constant
case label.

Chosen over re-vendoring a newer NXP driver because it keeps Phase 1's
**silicon-verified** driver bit-for-bit, and edits no vendored file, so
`VENDORING.md`'s verbatim claim survives. The cost is honest and stated: we
assert these capabilities are absent **by construction** rather than by asking
the hardware. If the GC355 does implement, say, SCISSOR, we leave that on the
table. Re-vendoring stays available and is the right move if §1's number
disappoints.

## 5. ★ The real hazard: two of the missing names are LIVE format selections

The feature bits are inert — worst case LVGL takes a slower path. **Nine
missing names are not feature bits**, and three of those appear in real code
rather than only in a stringifier:

```c
case LV_COLOR_FORMAT_ARGB8565: return VG_LITE_BGRA5658;   // lv_vg_lite_utils.c:582
case LV_COLOR_FORMAT_RGB888:   return VG_LITE_BGR888;     // lv_vg_lite_utils.c:585
if (tiled || format == VG_LITE_RGBA8888_ETC2_EAC) ...     // lv_vg_lite_utils.c:728
```

`VG_LITE_BGR888` and `VG_LITE_BGRA5658` are **returned as the format a buffer
will actually be drawn with**. If an LVGL image in `LV_COLOR_FORMAT_RGB888` or
`ARGB8565` reaches the backend, a shimmed value is handed to the driver as a
real format.

**Phase 1 established that this driver hangs rather than erroring when handed
something it does not understand** — a misaligned command buffer hung the front
end while every API call returned `VG_LITE_SUCCESS`. An unknown format is the
same class of input. So the failure mode here is not "wrong colours", it is
plausibly "the GPU stops and every status says fine".

`VG_LITE_RGBA8888_ETC2_EAC` is safe by comparison: it appears only in
equality tests, so a unique never-produced value makes them permanently false.

**Mitigation, and it is a requirement not a nicety:** the two live formats get
values that are *provably* never accepted, and the shim must make their
selection **loud**. Options to settle in the plan, cheapest first:
1. A build-time assertion that the port's `lv_conf.h` cannot produce those
   colour formats (they are decode targets for image assets this tree does not
   currently use).
2. A runtime `LV_ASSERT`/trap on the mapping function returning a shimmed
   format, so it fails at the point of selection rather than inside the GPU.

Do **not** map them onto a real format that "looks close". Silently drawing
ARGB8565 as BGRA8888 is exactly the kind of plausible-but-wrong behaviour this
tree's gates exist to prevent.

## 6. Verification (approved bar)

Phase 1 needed three silicon-only defects fixed before it rendered, and every
one of them passed QEMU. Phase 2 therefore verifies all three ways:

1. **QEMU gate** — the software-fallback path still works with the backend
   compiled in. QEMU has no GC355, so this gates the same ABSENT path
   `vglite_probe` does, now with `LV_USE_DRAW_VG_LITE 1`. It proves the binary
   is still one-image-two-paths.
2. **Silicon** — the LVGL scene renders on the RK055 under the GPU, confirmed
   by eye. ★ GPU and software output will **not** be pixel-identical
   (hardware AA differs from LVGL's mask arithmetic), so this needs its own
   golden set. Two golden sets, never one; do not copy one over the other.
3. **The number** — the 16-knob grid measured with the same `#ifdef FPSBENCH`
   method Phase 1 used for the 75 ms software figure, built into a separate
   build dir so the golden-producing ELF is untouched.

A Phase 2 that compiles and renders but is not faster has failed its own spec.
Report the fps whatever it says.

## 7. Non-goals

- Re-vendoring a newer VGLite driver (§4 — available as a follow-up).
- Touching LVGL source. The `#include <vg_lite.h>` hook exists precisely so
  this is unnecessary; using it is what keeps the licence firewall intact.
- Enabling ThorVG, NemaGFX, or `LV_USE_VG_LITE_DRIVER` (the pruned
  dual-licensed copy — see `LVGL/NOTICE`).
- Touch/interaction work on the knob.

---

## 8. Re-vendor scope (measured 2026-08-17, after §4's shim hit its limit)

§4 chose the shim and §5 warned it might not reach. It did not: the shim closed
the *name* gap, and the first real compile then failed on a *functional API*
gap — `vg_lite_stroke_t` and the whole stroke API absent, `vg_lite_draw_pattern`
taking 11 arguments upstream against our 10, `VG_LITE_PATTERN_REPEAT` absent,
and a hard `#error` because our gradient table is smaller than
`LV_GRADIENT_MAX_STOPS`. **Stroking settles it: the knob's arc is a stroke, so a
backend without it is not the feature.** Macros cannot supply missing code.

**Source, already on this machine:**
`~/Development/mcuxsdk-ws/mcuxsdk/middleware/vglite/driver` — MCUXpresso SDK
**v26.06.00-LTS**, the same release the LVGL tree was vendored from, so the
provenance story stays consistent.

**It clears every blocker, measured:**

| | ours | SDK |
|---|---|---|
| `VGLITE_HEADER_VERSION` | 6 | 7 |
| `gcFEATURE_BIT_VG_*` | 9 | 66 |
| `vg_lite_stroke_t` | absent | present |
| `VG_LITE_PATTERN_REPEAT` | absent | present |
| `VLC_MAX_GRADIENT_STOPS` | undefined | 16 |
| `vg_lite_draw_pattern` params | 10 | 11 |

**The compat shim goes to ZERO names.** Re-run of the Task 1 extractor against
the SDK headers: 0 missing features, 0 missing enums. Keep
`tools/vglite-lvgl-names.py` anyway — it is what proves that, and what catches
the next LVGL bump.

### Port work is small, and the reason is worth recording

An intermediate reading of the evidence looked alarming: the SDK driver has no
`vg_lite_os.h`, declares no `vg_lite_os_submit`/`wait`, and has **zero**
occurrences of `VG_DRIVER_SINGLE_THREAD` — which reads as "the submit/wait
abstraction was deleted, port rewrite required". **That was wrong, and reading
the actual submit path is what corrected it:**

```c
vg_lite_hal_poke(VG_LITE_HW_CMDBUF_ADDRESS, physical);
vg_lite_hal_poke(VG_LITE_HW_CMDBUF_SIZE, (size + 7) / 8);
```

— byte-for-byte the same two registers our single-thread path already pokes, and
completion still runs through `vg_lite_hal_wait_interrupt`, which our port
already implements. The abstraction did not move the mechanism; it removed a
layer we were already bypassing.

| item | cost |
|---|---|
| 4 new HAL functions (`map_memory`, `unmap_memory`, `operation_cache`, `memory_export`) | NXP's own port: **4–7 lines each** |
| `vg_lite_hal_map` gains `flags`, `bytes`, `dma_buf_fd` | NXP's port `(void)`s all three — add and ignore |
| `vg_lite_hal_allocate_contiguous` gains `pool`, `klogical` | clamp `pool` (as NXP does), `klogical = logical` in a single address space |
| `vg_lite_os_fopen`/`fclose` | **zero** — only `vg_lite_dump.c`/`dumpAPI.c` use them, and we do not vendor those |
| our single-thread `vg_lite_os_*` scaffolding | loses its callers; keep `malloc`/`free` (the driver uses them 48/46×), demote the rest to static helpers |

A few dozen lines. The ISR-counter and bounded-wait logic Phase 1 paid for
survives — it moves under `vg_lite_hal_wait_interrupt` rather than being rewritten.

### Licence: the position improves

No copyleft. **The Apache-2.0 pair disappears** — the new driver has no
`vg_lite_flat.{c,h}` — so VGLite becomes MIT-only and `NOTICE` simplifies.

★ **One hazard, and it is the kind this tree exists to catch.**
`VGLite/vg_lite_stroke.c` is **ISO-8859-1, not UTF-8**. Plain `grep` treats it
as binary and skips it, and `tools/license-audit.sh` Part 1 greps with `-I`,
*ignore binary files*. Proven directly: `grep -I` finds nothing in that file,
`grep -a` finds Vivante's MIT text. Vendoring it as-is puts a source file into
the tree that **the copyleft sweep silently never reads** — the same hole the
`nema_gfx` unlicensed-binary rule was written to close, arriving through a
different door. It is MIT, so nothing is wrong today; the audit simply could not
tell you so. Fix on vendoring (transcode to UTF-8) or teach Part 1 about
non-UTF-8 text. One file.

### Risks

1. **The feature table becomes truthful for 66 bits**, so LVGL will take paths
   previously forced off. That is the point, but it is new behaviour, not just
   new names — and it is where a GPU-vs-software pixel difference will show up.
2. **Phase 1's silicon result must be re-proven first** — `vglite_probe`'s
   golden `0x45465405`, `TIMEOUTS=0`, and eyes on glass. Nothing downstream is
   trustworthy until the new driver renders the blue square.
3. Signature drift beyond the four above is possible in code paths the compile
   has not reached yet.

### What survives from the shim attempt

`tools/vglite-lvgl-names.py` and its gate, the `lv_conf.h` `#ifndef` opt-in
guard, `import_evkb_lvgl(VGLITE)` and its configure-time format guard, and the
`IN_LIST ARGN` fix. All are needed either way.
