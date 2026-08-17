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
