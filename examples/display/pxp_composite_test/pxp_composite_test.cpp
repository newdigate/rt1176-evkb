/* pxp_composite_test - RT1176 PXP Phase 3: alpha-surface compositing + colorkey
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * v8's centerpiece: every AS behavior the PXP library can program - the four
 * alpha modes with invert, the 12-op ROP table, AS and PS colorkeys, four AS
 * formats, placement geometries and a full 0..255 alpha gradient - composited
 * INTO the live RK055 framebuffer (PS = framebuffer content, OUT = same), then
 * checked against an INDEPENDENT per-pixel software oracle transcribed from
 * the i.MX RT1170 RM rev.5 chapter 52.
 *
 * THE ORACLE'S EPISTEMIC STATUS (read before touching it): the RM contradicts
 * itself at least twice (blend direction, AS-key result - see the v8 plan's
 * "Known RM contradictions").  Every formerly-CONTESTED rule lives in exactly
 * one small named function tagged "MEASURED: <date> ... SOLVED" below - the
 * tags record silicon's verdicts (Task 3, six instrumented runs; the
 * transcript's RESOLVED AMBIGUITIES section is the authority).  The oracle is
 * FROZEN: it IS the silicon truth, and any QEMU-vs-oracle disagreement is a
 * model bug by definition.  Everything outside the tagged functions is an
 * RM-documented fact and is cited where encoded.
 *
 * Per case: (re)fill the PS region with a position-dependent pattern, run the
 * PXPOp, oracle the same composite into an expected-frame scratch, compare the
 * composited sub-rect byte-for-byte AND the whole-framebuffer FNV-1a both
 * ways (the whole-frame sum is the out-of-bounds-write detector):
 *     CASE n=<name> EXPECT=0x........ GOT=0x........ MATCH|MISMATCH
 * CASES=<emitted> counts compare-cases actually emitted (the v7 discipline: a
 * dropped case cannot hide behind a passing sweep).  The ALPHA_OUT raw dump
 * does NOT count - it has no oracle compare by design (contested knob 4).
 *
 * After the case table, four ritual frames are held on the glass for the eye
 * (FRAME=<name>, 6 s each), then UNDERRUNS (v7 convention; vacuous in QEMU -
 * no timing model), CASES, COMPOSITE_OK, PXP_COMPOSITE_DONE.  Every early-out
 * path prints PXP_COMPOSITE_DONE - the run never hangs silently.
 *
 * QEMU (Task 4): the model's AS datapath was written FROM the silicon truth
 * table and all 31 cases MATCH under QEMU with checksums byte-identical to
 * silicon's; run_qemu_pxp_composite.sh is the gate.  (Historical: Task 2's
 * pre-model baseline had 30/31 MISMATCH -- with argb_override_00's honest
 * coincidental MATCH proving the compare machinery.)
 *
 * Uses Serial1 (LPUART; QEMU captures it), like every sibling gate.
 */
#include <Arduino.h>
#include <string.h>
#include "Display.h"
#include "PXP.h"

/* ======================= fixed colours & geometry ========================= */

/* PS_BACKGROUND for every op: shows only where BOTH colorkeys hit
 * (RM 52.3.1.13: both-keys hit -> background passes). */
static constexpr uint32_t BG_RGB = 0x00204060u;

/* PS colorkey: magenta, chosen as an exact RGB565 expansion so the painted
 * 565 block re-expands to exactly the key value (0xF81F -> 0xFF00FF). */
static constexpr uint16_t PSKEY_565 = 0xF81Fu;

/* AS colorkey for the 8-bit-channel formats (an arbitrary 24-bit value no
 * RGB565 expansion can produce, so a 565 PS pixel can never alias it). */
static constexpr uint32_t ASKEY_RGB = 0x00123456u;

/* AS colorkey for the RGB565 green-screen case: pure green, exact range. */
static constexpr uint32_t GREEN_RGB = 0x0000FF00u;
static constexpr uint16_t GREEN_565 = 0x07E0u;

/* AS overlay maximum extent (the allocation bound: 160x120 at 4 B/px). */
static constexpr uint32_t AS_MAX_W = 160u, AS_MAX_H = 120u;
static constexpr uint32_t AS_MAX_BYTES = AS_MAX_W * AS_MAX_H * 4u;

/* ================= RM-documented (UNCONTESTED) conversions ================ */

/* RGB565 -> 888: replicate the upper bits into the lower (RM 52.3.1.22
 * pixel-handling rule 3); full-scale maps to full-scale. */
static inline uint32_t expand565(uint16_t c)
{
    uint32_t r = (c >> 11) & 0x1Fu, g = (c >> 5) & 0x3Fu, b = c & 0x1Fu;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

/* 888 -> RGB565 output narrowing: take the top bits (truncation - the exact
 * left inverse of the replication above for every 565-expressible value). */
static inline uint16_t narrow565(uint32_t rgb)
{
    return (uint16_t)((((rgb >> 16) & 0xF8u) << 8) |
                      (((rgb >>  8) & 0xFCu) << 3) |
                      (((rgb      ) & 0xF8u) >> 3));
}

/* Colorkey hit: ALL THREE channels within low..high INCLUSIVE on the 24-bit
 * expansion (RM 52.3.1.13; the disable encoding low=0xFFFFFF/high=0 makes the
 * range test never true).  Uncontested. */
static inline bool key_hit(uint32_t px, uint32_t low, uint32_t high)
{
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t c = (px >> sh) & 0xFFu;
        if (c < ((low >> sh) & 0xFFu) || c > ((high >> sh) & 0xFFu))
            return false;
    }
    return true;
}

/* The 12-op ROP table (RM 52.6.22 AS_CTRL[ROP]): bitwise ops on the 24-bit
 * RGB.  Uncontested - transcribed 1:1 from the field's value table. */
static uint32_t rop_apply(uint8_t rop, uint32_t as, uint32_t ps)
{
    uint32_t v;
    switch (rop) {
    case PXP_ROP_MASKAS:     v = as & ps;    break;
    case PXP_ROP_MASKNOTAS:  v = ~as & ps;   break;
    case PXP_ROP_MASKASNOT:  v = as & ~ps;   break;
    case PXP_ROP_MERGEAS:    v = as | ps;    break;
    case PXP_ROP_MERGENOTAS: v = ~as | ps;   break;
    case PXP_ROP_MERGEASNOT: v = as | ~ps;   break;
    case PXP_ROP_NOTCOPYAS:  v = ~as;        break;
    case PXP_ROP_NOT:        v = ~ps;        break;
    case PXP_ROP_NOTMASKAS:  v = ~(as & ps); break;
    case PXP_ROP_NOTMERGEAS: v = ~(as | ps); break;
    case PXP_ROP_XORAS:      v = as ^ ps;    break;
    case PXP_ROP_NOTXORAS:   v = ~(as ^ ps); break;
    default:                 v = ps;         break;
    }
    return v & 0x00FFFFFFu;
}

/* ============ CONTESTED RULES -- the four MEASURED: knobs =================
 * The RM contradicts itself (v8 plan preamble).  Each knob encodes the
 * CURRENT best reading; Task 3 (silicon) flips ONLY these, updating the tag.
 * (Knob 4, ALPHA_OUT, is a comment at its case below - raw dump, no oracle.) */

/* MEASURED: 2026-07-31 gradient dump (P2b) -- SOLVED EXACTLY, 256/256 points.
 * Knob 1: blend direction AND divisor.  Direction: the PROSE reading of RM
 * 52.3.1.12 is correct (a weights AS; a=0 -> PS passes; the RM's own
 * equation, which weights PS, is the typo).  Arithmetic: the full 0..255
 * alpha ramp (GROW dump, one pixel per alpha) is fit by exactly one
 * candidate:
 *     out = (a*AS + (256-a)*PS) >> 8      -- plain a, /256, TRUNCATING.
 * Not /255-rounded (196/256), not a+(a>>7)-mapped (238/256).  At a=0xFF the
 * 1/256 PS leak never crosses a 565 quantization step, which is why the
 * opaque case could not discriminate. */
static inline uint8_t blend_channel(uint8_t ps, uint8_t as, uint32_t a)
{
    if (a == 255u) return as;   /* measured: exact pass-through at full alpha */
    return (uint8_t)((a * as + (256u - a) * ps) >> 8);
}

/* MEASURED: 2026-07-31 MROW sweeps (P2b) -- SOLVED EXACTLY, 512/512 points
 * across Ga=0x80 and Ga=0xC3.  Knob 2: effective alpha per mode:
 * EMBEDDED->Ea (gradient solve), OVERRIDE->Ga (override cases exact),
 * MULTIPLY->(Ga*Ea + 128) >> 8 (round-nearest /256 -- NOT the RM's
 * (Ga*Ea+0x80)/128 cap-128 text, which fit 14/512), INVERT applied LAST
 * as 255-a (invert case exact under the solved blend). */
static inline uint32_t effective_alpha(uint8_t mode, uint8_t emb, uint8_t glob,
                                       bool invert)
{
    uint32_t a;
    switch (mode) {
    case PXP_ALPHA_OVERRIDE: a = glob; break;
    case PXP_ALPHA_MULTIPLY: a = ((uint32_t)glob * emb + 128u) >> 8; break;
    case PXP_ALPHA_EMBEDDED:
    default:                 a = emb; break;
    }
    if (invert) a = 255u - a;
    return a;
}

/* MEASURED: 2026-07-31 run 1 (P2b) -- CONFIRMED first try, all three AS-key
 * cases MATCH.  Knob 3: AS-key hit result.  The AS_CTRL[ENABLE_COLORKEY]
 * field description is correct: the PS pixel shows (a transparency key).
 * RM 52.3.1.13's "AS is passed" is the typo -- transcript RESOLVED
 * AMBIGUITIES item 4. */
static inline uint32_t askey_hit_result(uint32_t ps24, uint32_t as24)
{
    (void)as24;
    return ps24;
}

/* ============================ FNV-1a-32 =================================== */

static uint32_t fnv1a(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

/* ============================ patterns ==================================== */

/* Position-dependent PS pattern (the decimate-test idiom: any stride error
 * changes the checksum).  Never emits the PS key colour: the PS-key cases
 * paint their key block explicitly, so an accidental key hit - whose
 * semantics OUTSIDE the AS rect the RM leaves undefined - cannot occur. */
static inline uint16_t ps_pattern(uint32_t x, uint32_t y)
{
    uint16_t p = (uint16_t)((((x * 7u + y * 31u) & 0x1Fu) << 11) |
                            (((x * 3u + y * 5u)  & 0x3Fu) << 5)  |
                            (((x + y * 11u)      & 0x1Fu)));
    if (p == PSKEY_565) p ^= 0x0842u;
    return p;
}

/* Position-dependent AS pattern channels (alpha varies across the full byte
 * range so the embedded cases exercise real transparency). */
static inline void as_pattern(uint32_t x, uint32_t y,
                              uint8_t &r, uint8_t &g, uint8_t &b, uint8_t &a)
{
    r = (uint8_t)(x * 7u  + y * 3u  + 0x21u);
    g = (uint8_t)(x * 5u  + y * 11u + 0x43u);
    b = (uint8_t)(x * 13u + y * 1u  + 0x65u);
    a = (uint8_t)(x * 2u  + y * 29u + 0x87u);
}

/* ============================ the case table ============================== */

struct Case {
    const char *name;
    PXPFormat   as_fmt;
    uint16_t    rx, ry, rw, rh;   /* PS/OUT region within the framebuffer    */
    uint16_t    aw, ah, ax, ay;   /* AS dims + placement within the region   */
    uint8_t     mode;             /* PXPAlphaMode                            */
    uint8_t     alpha;            /* AS_CTRL[ALPHA] (override/multiply)      */
    bool        invert;           /* ALPHA_INVERT                            */
    bool        as_key;           /* exact-range AS colorkey                 */
    uint32_t    as_key_rgb;       /* 24-bit key (low == high)                */
    bool        ps_key;           /* exact-range PS colorkey                 */
    uint16_t    ps_key_565;       /* PS key colour as the painted 565 pixel  */
    bool        rop_set;
    uint8_t     rop;              /* PXPRop                                  */
    bool        gradient;         /* ARGB strip: alpha = x (0..255)          */
};

/* Default geometry: a 200x160 region at (64,64), AS 160x120 at (20,20). */
#define GEO_DEF   64,   64, 200, 160, 160, 120,  20,  20
#define NO_KEYS   false, 0u, false, 0u
#define NO_ROP    false, 0u
#define ROP(r)    true, (uint8_t)(r)

static const Case CASES[] = {
    /* --- formats x alpha behaviors --------------------------------------- */
    { "argb_embedded",        PXP_ARGB8888, GEO_DEF, PXP_ALPHA_EMBEDDED, 0xFF, false, NO_KEYS, NO_ROP, false },
    { "argb_override_80",     PXP_ARGB8888, GEO_DEF, PXP_ALPHA_OVERRIDE, 0x80, false, NO_KEYS, NO_ROP, false },
    { "argb_override_00",     PXP_ARGB8888, GEO_DEF, PXP_ALPHA_OVERRIDE, 0x00, false, NO_KEYS, NO_ROP, false },
    { "argb_multiply_80",     PXP_ARGB8888, GEO_DEF, PXP_ALPHA_MULTIPLY, 0x80, false, NO_KEYS, NO_ROP, false },
    { "argb_invert_embedded", PXP_ARGB8888, GEO_DEF, PXP_ALPHA_EMBEDDED, 0xFF, true,  NO_KEYS, NO_ROP, false },
    { "argb_askey",           PXP_ARGB8888, GEO_DEF, PXP_ALPHA_OVERRIDE, 0xFF, false, true, ASKEY_RGB, false, 0u, NO_ROP, false },
    { "argb_pskey",           PXP_ARGB8888, GEO_DEF, PXP_ALPHA_OVERRIDE, 0x40, false, false, 0u, true, PSKEY_565, NO_ROP, false },
    { "argb_bothkeys",        PXP_ARGB8888, GEO_DEF, PXP_ALPHA_OVERRIDE, 0x40, false, true, ASKEY_RGB, true, PSKEY_565, NO_ROP, false },
    { "rgba_embedded",        PXP_RGBA8888, GEO_DEF, PXP_ALPHA_EMBEDDED, 0xFF, false, NO_KEYS, NO_ROP, false },
    { "rgba_askey",           PXP_RGBA8888, GEO_DEF, PXP_ALPHA_OVERRIDE, 0xFF, false, true, ASKEY_RGB, false, 0u, NO_ROP, false },
    { "rgb565_override_80",   PXP_RGB565,   GEO_DEF, PXP_ALPHA_OVERRIDE, 0x80, false, NO_KEYS, NO_ROP, false },
    { "rgb565_askey",         PXP_RGB565,   GEO_DEF, PXP_ALPHA_OVERRIDE, 0xFF, false, true, GREEN_RGB, false, 0u, NO_ROP, false },
    { "rgb565_pskey",         PXP_RGB565,   GEO_DEF, PXP_ALPHA_OVERRIDE, 0x40, false, false, 0u, true, PSKEY_565, NO_ROP, false },
    { "rgb888x_override_80",  PXP_XRGB8888, GEO_DEF, PXP_ALPHA_OVERRIDE, 0x80, false, NO_KEYS, NO_ROP, false },
    /* Assertion case (RM 52.3.1.22 rule 3): an RGB565 AS has embedded alpha
     * := 0xFF, so EMBEDDED mode must be a fully opaque replace. */
    { "rgb565_embedded_opaque", PXP_RGB565, GEO_DEF, PXP_ALPHA_EMBEDDED, 0xFF, false, NO_KEYS, NO_ROP, false },
    /* --- the 12 ROPs (RGB565 AS over the fixed default PS block) --------- */
    { "rop_maskas",     PXP_RGB565, GEO_DEF, PXP_ALPHA_ROPS, 0x00, false, NO_KEYS, ROP(PXP_ROP_MASKAS),     false },
    { "rop_masknotas",  PXP_RGB565, GEO_DEF, PXP_ALPHA_ROPS, 0x00, false, NO_KEYS, ROP(PXP_ROP_MASKNOTAS),  false },
    { "rop_maskasnot",  PXP_RGB565, GEO_DEF, PXP_ALPHA_ROPS, 0x00, false, NO_KEYS, ROP(PXP_ROP_MASKASNOT),  false },
    { "rop_mergeas",    PXP_RGB565, GEO_DEF, PXP_ALPHA_ROPS, 0x00, false, NO_KEYS, ROP(PXP_ROP_MERGEAS),    false },
    { "rop_mergenotas", PXP_RGB565, GEO_DEF, PXP_ALPHA_ROPS, 0x00, false, NO_KEYS, ROP(PXP_ROP_MERGENOTAS), false },
    { "rop_mergeasnot", PXP_RGB565, GEO_DEF, PXP_ALPHA_ROPS, 0x00, false, NO_KEYS, ROP(PXP_ROP_MERGEASNOT), false },
    { "rop_notcopyas",  PXP_RGB565, GEO_DEF, PXP_ALPHA_ROPS, 0x00, false, NO_KEYS, ROP(PXP_ROP_NOTCOPYAS),  false },
    { "rop_not",        PXP_RGB565, GEO_DEF, PXP_ALPHA_ROPS, 0x00, false, NO_KEYS, ROP(PXP_ROP_NOT),        false },
    { "rop_notmaskas",  PXP_RGB565, GEO_DEF, PXP_ALPHA_ROPS, 0x00, false, NO_KEYS, ROP(PXP_ROP_NOTMASKAS),  false },
    { "rop_notmergeas", PXP_RGB565, GEO_DEF, PXP_ALPHA_ROPS, 0x00, false, NO_KEYS, ROP(PXP_ROP_NOTMERGEAS), false },
    { "rop_xoras",      PXP_RGB565, GEO_DEF, PXP_ALPHA_ROPS, 0x00, false, NO_KEYS, ROP(PXP_ROP_XORAS),      false },
    { "rop_notxoras",   PXP_RGB565, GEO_DEF, PXP_ALPHA_ROPS, 0x00, false, NO_KEYS, ROP(PXP_ROP_NOTXORAS),   false },
    /* --- geometries (the v6 lesson: odd offsets + edge-hugging) ---------- */
    { "argb_embedded_offs13_7", PXP_ARGB8888,  13,    7, 200, 160, 160, 120, 13, 7,
      PXP_ALPHA_EMBEDDED, 0xFF, false, NO_KEYS, NO_ROP, false },
    { "argb_embedded_edge_br",  PXP_ARGB8888, 520, 1120, 200, 160, 160, 120, 40, 40,
      PXP_ALPHA_EMBEDDED, 0xFF, false, NO_KEYS, NO_ROP, false },   /* AS LRC = fb (719,1279) */
    /* Full-frame OUTPUT region (720x1280), AS 160x120 centered - the AS stays
     * within the allocation bound; it is the output window that is
     * full-frame, hence the name. */
    { "override_center", PXP_ARGB8888, 0, 0, 720, 1280, 160, 120, 280, 580,
      PXP_ALPHA_OVERRIDE, 0x80, false, NO_KEYS, NO_ROP, false },
    /* --- the rounding probe: alpha 0..255 by x, embedded ----------------- */
    { "gradient_embedded", PXP_ARGB8888, 64, 300, 280, 40, 256, 4, 12, 18,
      PXP_ALPHA_EMBEDDED, 0xFF, false, NO_KEYS, NO_ROP, true },
};
static constexpr uint32_t NUM_CASES = sizeof(CASES) / sizeof(CASES[0]);
static_assert(NUM_CASES == 31u, "the pinned case count -- update the gate too");

/* ====================== buffers (extmem, 64B aligned) ===================== */

static uint16_t *s_fb;         /* the live panel framebuffer (RGB565)        */
static uint16_t *s_expected;   /* whole-frame oracle scratch (PANEL_FB_BYTES)*/
static uint8_t  *s_as_argb;    /* AS pattern buffers, one per format         */
static uint8_t  *s_as_rgba;
static uint8_t  *s_as_xrgb;
static uint8_t  *s_as_565;
static uint32_t *s_aout;       /* XRGB8888 scratch OUT for ALPHA_OUT (160x120)*/

static uint8_t *alloc_ext(size_t bytes)
{
    uint8_t *raw = (uint8_t *)extmem_malloc(bytes + 64);
    if (!raw) return nullptr;
    return (uint8_t *)(((uintptr_t)raw + 63) & ~(uintptr_t)63);
}

/* PXPError -> token for the per-case failure line (the bench's idiom). */
static const char *pxp_err_name(PXPError e)
{
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

/* =========================== AS buffer filling ============================ */

static uint8_t *as_buf_for(PXPFormat f)
{
    switch (f) {
    case PXP_ARGB8888: return s_as_argb;
    case PXP_RGBA8888: return s_as_rgba;
    case PXP_XRGB8888: return s_as_xrgb;
    case PXP_RGB565:   return s_as_565;
    default:           return nullptr;
    }
}

/* AS-key block: a known rect of the exact key colour inside the overlay.
 * [aw/8, aw/8+aw/3) x [ah/8, ah/8+ah/3). */
static inline bool in_askey_block(const Case &c, uint32_t x, uint32_t y)
{
    return x >= c.aw / 8u && x < c.aw / 8u + c.aw / 3u &&
           y >= c.ah / 8u && y < c.ah / 8u + c.ah / 3u;
}

/* PS-key block, in AS-relative coordinates (painted into the framebuffer
 * where the overlay will sit, so PS-key semantics are only ever exercised
 * INSIDE the AS rect - outside it the RM's PS-key-with-no-AS behavior is
 * undefined and this example deliberately never triggers it):
 * [aw/4, 3aw/4) x [ah/4, 3ah/4).  Overlaps the AS-key block partially, so
 * the both-keys case has all three sub-regions: AS-only, PS-only, both. */
static inline bool in_pskey_block(const Case &c, uint32_t x, uint32_t y)
{
    return x >= c.aw / 4u && x < 3u * c.aw / 4u &&
           y >= c.ah / 4u && y < 3u * c.ah / 4u;
}

static void fill_as(const Case &c)
{
    uint8_t *buf = as_buf_for(c.as_fmt);
    for (uint32_t y = 0; y < c.ah; y++) {
        for (uint32_t x = 0; x < c.aw; x++) {
            uint8_t r, g, b, a;
            if (c.gradient) {
                /* the rounding probe: alpha == x (aw is 256) */
                r = (uint8_t)x; g = (uint8_t)(255u - x); b = 0x80u;
                a = (uint8_t)x;
            } else {
                as_pattern(x, y, r, g, b, a);
            }
            if (c.as_key) {
                if (in_askey_block(c, x, y)) {
                    r = (uint8_t)(c.as_key_rgb >> 16);
                    g = (uint8_t)(c.as_key_rgb >> 8);
                    b = (uint8_t)(c.as_key_rgb);
                    a = 0xFF;
                } else if ((((uint32_t)r << 16) | ((uint32_t)g << 8) | b)
                           == c.as_key_rgb) {
                    b ^= 1u;   /* never the key colour by accident */
                }
            }
            switch (c.as_fmt) {
            case PXP_ARGB8888:
                ((uint32_t *)buf)[y * c.aw + x] =
                    ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                    ((uint32_t)g << 8) | b;
                break;
            case PXP_RGBA8888:   /* alpha in the LOW byte (RM 52.6.22) */
                ((uint32_t *)buf)[y * c.aw + x] =
                    ((uint32_t)r << 24) | ((uint32_t)g << 16) |
                    ((uint32_t)b << 8) | a;
                break;
            case PXP_XRGB8888: { /* X byte = varied garbage: proves ignored */
                uint8_t xb = (uint8_t)(0x5Au ^ x);
                ((uint32_t *)buf)[y * c.aw + x] =
                    ((uint32_t)xb << 24) | ((uint32_t)r << 16) |
                    ((uint32_t)g << 8) | b;
                break;
            }
            case PXP_RGB565: {
                uint16_t p = (uint16_t)((((uint32_t)r >> 3) << 11) |
                                        (((uint32_t)g >> 2) << 5)  |
                                        ((uint32_t)b >> 3));
                if (c.as_key && !in_askey_block(c, x, y) &&
                    expand565(p) == c.as_key_rgb)
                    p ^= 0x0020u;
                if (c.as_key && in_askey_block(c, x, y))
                    p = narrow565(c.as_key_rgb);   /* exact-expansion key */
                ((uint16_t *)buf)[y * c.aw + x] = p;
                break;
            }
            default: break;
            }
        }
    }
}

/* AS pixel readback for the oracle: unpack per RM 52.6.22 / 52.3.1.22.
 * RGB565 and RGB888X carry NO alpha: embedded alpha := 0xFF (rule 3). */
static void as_read(const Case &c, uint32_t x, uint32_t y,
                    uint32_t &rgb24, uint8_t &emb_a)
{
    const uint8_t *buf = as_buf_for(c.as_fmt);
    switch (c.as_fmt) {
    case PXP_ARGB8888: {
        uint32_t w = ((const uint32_t *)buf)[y * c.aw + x];
        rgb24 = w & 0x00FFFFFFu; emb_a = (uint8_t)(w >> 24);
        break;
    }
    case PXP_RGBA8888: {  /* alpha in the LOW byte */
        uint32_t w = ((const uint32_t *)buf)[y * c.aw + x];
        rgb24 = (w >> 8) & 0x00FFFFFFu; emb_a = (uint8_t)w;
        break;
    }
    case PXP_XRGB8888: {  /* X ignored; alpha := 0xFF */
        uint32_t w = ((const uint32_t *)buf)[y * c.aw + x];
        rgb24 = w & 0x00FFFFFFu; emb_a = 0xFF;
        break;
    }
    case PXP_RGB565: {    /* expand by bit replication; alpha := 0xFF */
        uint16_t p = ((const uint16_t *)buf)[y * c.aw + x];
        rgb24 = expand565(p); emb_a = 0xFF;
        break;
    }
    default: rgb24 = 0; emb_a = 0; break;
    }
}

/* ============================== the oracle ================================ */

/* Composite the AS rect into the expected frame.  Pixels outside the AS rect
 * are PS pass-through: RGB565 -> 888 -> RGB565 is the identity (truncation is
 * the left inverse of replication), so the pre-op copy already holds them. */
static void oracle_case(const Case &c)
{
    const uint32_t ps_key24 = expand565(c.ps_key_565);
    for (uint32_t y = 0; y < c.ah; y++) {
        for (uint32_t x = 0; x < c.aw; x++) {
            uint32_t fx = (uint32_t)c.rx + c.ax + x;
            uint32_t fy = (uint32_t)c.ry + c.ay + y;
            uint16_t *dst = &s_expected[fy * PANEL_WIDTH + fx];
            uint32_t ps24 = expand565(*dst);
            uint32_t as24; uint8_t emb;
            as_read(c, x, y, as24, emb);

            bool as_hit = c.as_key && key_hit(as24, c.as_key_rgb, c.as_key_rgb);
            bool ps_hit = c.ps_key && key_hit(ps24, ps_key24, ps_key24);

            uint32_t out24;
            if (as_hit && ps_hit) {
                out24 = BG_RGB;                    /* both keys -> background */
            } else if (ps_hit) {
                out24 = as24;                      /* PS key -> AS, UNblended */
            } else if (as_hit) {
                out24 = askey_hit_result(ps24, as24);   /* CONTESTED knob 3  */
            } else if (c.rop_set) {
                out24 = rop_apply(c.rop, as24, ps24);
            } else {
                uint32_t a = effective_alpha(c.mode, emb, c.alpha, c.invert);
                out24 = ((uint32_t)blend_channel((uint8_t)(ps24 >> 16),
                                                 (uint8_t)(as24 >> 16), a) << 16)
                      | ((uint32_t)blend_channel((uint8_t)(ps24 >> 8),
                                                 (uint8_t)(as24 >> 8), a) << 8)
                      |  (uint32_t)blend_channel((uint8_t)ps24,
                                                 (uint8_t)as24, a);
            }
            *dst = narrow565(out24);
        }
    }
}

/* ============================ the case runner ============================= */

static void fill_region(const Case &c)
{
    for (uint32_t y = 0; y < c.rh; y++)
        for (uint32_t x = 0; x < c.rw; x++)
            s_fb[((uint32_t)c.ry + y) * PANEL_WIDTH + c.rx + x] =
                ps_pattern(c.rx + x, c.ry + y);
}

static void paint_pskey_block(const Case &c)
{
    for (uint32_t y = 0; y < c.ah; y++)
        for (uint32_t x = 0; x < c.aw; x++)
            if (in_pskey_block(c, x, y))
                s_fb[((uint32_t)c.ry + c.ay + y) * PANEL_WIDTH
                     + c.rx + c.ax + x] = c.ps_key_565;
}

/* -1 = PXP op error (line printed, not counted); 0 = MISMATCH; 1 = MATCH. */
static int run_case(const Case &c)
{
    /* 1. PS: (re)fill this case's region with the position pattern, plus the
     *    PS-key block when armed.  Regions touched by earlier cases stay as
     *    they were - they are identical in fb and expected, so the whole-
     *    frame sums still agree unless THIS op writes out of bounds. */
    fill_region(c);
    if (c.ps_key) paint_pskey_block(c);

    /* 2. AS pattern + the pre-op snapshot the oracle composites into. */
    fill_as(c);
    memcpy(s_expected, s_fb, PANEL_FB_BYTES);

    /* 3. The hardware pass: PS = OUT = the framebuffer region (the glass
     *    shows every case), AS placed inside it. */
    uint8_t *base = (uint8_t *)s_fb + (uint32_t)c.ry * PANEL_PITCH_BYTES
                                    + (uint32_t)c.rx * 2u;
    PXPSurface reg(base, c.rw, c.rh, PXP_RGB565, (uint16_t)PANEL_PITCH_BYTES);
    PXPSurface as(as_buf_for(c.as_fmt), c.aw, c.ah, c.as_fmt);
    PXPOp op = PXP.op();
    op.source(reg).output(reg).background(BG_RGB)
      .overlay(as).overlayAt(c.ax, c.ay);
    if (c.rop_set)
        op.overlayAlpha(PXP_ALPHA_ROPS).rop((PXPRop)c.rop);
    else
        op.overlayAlpha((PXPAlphaMode)c.mode, c.alpha, c.invert);
    if (c.as_key) op.overlayColorKey(c.as_key_rgb, c.as_key_rgb);
    if (c.ps_key) {
        uint32_t k = expand565(c.ps_key_565);
        op.sourceColorKey(k, k);
    }
    PXPError e = op.run();
    if (e != PXP_OK) {
        Serial1.printf("CASE n=%s PXP_ERR=%s\n", c.name, pxp_err_name(e));
        return -1;
    }

    /* 4. Oracle + compare: sub-rect byte-for-byte AND whole-frame FNV. */
    oracle_case(c);
    bool sub_ok = true;
    for (uint32_t y = 0; y < c.rh && sub_ok; y++) {
        const void *g = &s_fb[((uint32_t)c.ry + y) * PANEL_WIDTH + c.rx];
        const void *x = &s_expected[((uint32_t)c.ry + y) * PANEL_WIDTH + c.rx];
        if (memcmp(g, x, (size_t)c.rw * 2u) != 0) sub_ok = false;
    }
    uint32_t got = fnv1a(s_fb, PANEL_FB_BYTES);
    uint32_t exp = fnv1a(s_expected, PANEL_FB_BYTES);
    bool ok = sub_ok && (got == exp);
    Serial1.printf("CASE n=%s EXPECT=0x%08lX GOT=0x%08lX %s\n", c.name,
                   (unsigned long)exp, (unsigned long)got,
                   ok ? "MATCH" : "MISMATCH");

    return ok ? 1 : 0;
}

/* ========================= the ALPHA_OUT raw dump ========================= */

/* MEASURED: 2026-07-31 (P2b), raw verdict.  Knob 4: with the LEGACY engine
 * ARMED, the output X byte carries a NONZERO, per-pixel, fully deterministic
 * computed alpha (dump stable across every run:
 * DE316D74 E0387180 E23E758D E4457A99 E64B7EA5 E8528399 EA5887A8 EC5F8CB6)
 * -- not v7's engine-unconfigured 0, not OUT_CTRL[ALPHA] (0 here), not the
 * source X.  The ANALYTIC formula was deliberately not derived: no consumer
 * reads the byte (XRGB everywhere), so the QEMU model does NOT claim it and
 * the gate checks this line's PRESENCE only -- a stated asymmetry, recorded
 * in the transcript, honest the way the UNDERRUN vacuity is. */
static void run_alpha_out(void)
{
    /* PS pattern pre-filled into the scratch itself (in-place, like every
     * other case), X byte varied so an X-preserving path is visible too. */
    for (uint32_t y = 0; y < AS_MAX_H; y++) {
        for (uint32_t x = 0; x < AS_MAX_W; x++) {
            uint8_t r, g, b, a;
            as_pattern(x + 7u, y + 3u, r, g, b, a);   /* offset: != the AS fill */
            s_aout[y * AS_MAX_W + x] =
                ((uint32_t)(0xC3u ^ x) << 24) | ((uint32_t)r << 16) |
                ((uint32_t)g << 8) | b;
        }
    }
    /* AS = the standard ARGB8888 pattern (varied alpha). */
    Case ac = CASES[0];        /* argb_embedded's format/dims for fill_as */
    ac.aw = AS_MAX_W; ac.ah = AS_MAX_H;
    fill_as(ac);

    PXPSurface ps_s(s_aout, AS_MAX_W, AS_MAX_H, PXP_XRGB8888);
    PXPSurface as(s_as_argb, AS_MAX_W, AS_MAX_H, PXP_ARGB8888);
    PXPError e = PXP.op().source(ps_s).output(ps_s).background(BG_RGB)
                    .overlay(as).overlayAt(0, 0)
                    .overlayAlpha(PXP_ALPHA_EMBEDDED).run();
    if (e != PXP_OK) {
        Serial1.printf("ALPHA_OUT PXP_ERR=%s\n", pxp_err_name(e));
        return;
    }
    /* 8 destination words from row 3 (varied source alphas cross it). */
    const uint32_t *d = &s_aout[3u * AS_MAX_W];
    Serial1.printf("ALPHA_OUT dst %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX\n",
                   (unsigned long)d[0], (unsigned long)d[1],
                   (unsigned long)d[2], (unsigned long)d[3],
                   (unsigned long)d[4], (unsigned long)d[5],
                   (unsigned long)d[6], (unsigned long)d[7]);
}

/* ============================ ritual frames =============================== */
/* Eye evidence on the same engine the oracle checked - no checksums here.
 * Each frame: compose into the live framebuffer, print FRAME=<name>, hold. */

static void ritual_composite(PXPSurface &as, uint16_t x, uint16_t y,
                             PXPAlphaMode m, uint8_t val,
                             bool green_key, bool rop_xor)
{
    PXPSurface fbs(s_fb, (uint16_t)PANEL_WIDTH, (uint16_t)PANEL_HEIGHT,
                   PXP_RGB565, (uint16_t)PANEL_PITCH_BYTES);
    PXPOp op = PXP.op();
    op.source(fbs).output(fbs).background(BG_RGB).overlay(as).overlayAt(x, y);
    if (rop_xor) op.overlayAlpha(PXP_ALPHA_ROPS).rop(PXP_ROP_XORAS);
    else         op.overlayAlpha(m, val);
    if (green_key) op.overlayColorKey(GREEN_RGB, GREEN_RGB);
    PXPError e = op.run();
    if (e != PXP_OK)
        Serial1.printf("FRAME_ERR=%s\n", pxp_err_name(e));
}

static void frame_alpha_sprite(void)
{
    Display.drawTestPattern();   /* colour bars + diagonal backdrop */
    /* Soft-edged amber blob: radial alpha falloff (quadratic), no hard edge. */
    constexpr int32_t CX = 80, CY = 60, R2 = 58 * 58;
    for (uint32_t y = 0; y < AS_MAX_H; y++) {
        for (uint32_t x = 0; x < AS_MAX_W; x++) {
            int32_t dx = (int32_t)x - CX, dy = (int32_t)y - CY;
            int32_t d2 = dx * dx + dy * dy;
            uint32_t a = (d2 >= R2) ? 0u : (255u - (255u * (uint32_t)d2) / R2);
            ((uint32_t *)s_as_argb)[y * AS_MAX_W + x] =
                (a << 24) | (0xFFu << 16) | (0xB4u << 8) | 0x28u;
        }
    }
    PXPSurface as(s_as_argb, AS_MAX_W, AS_MAX_H, PXP_ARGB8888);
    ritual_composite(as, 280, 580, PXP_ALPHA_EMBEDDED, 0xFF, false, false);
    Serial1.println("FRAME=alpha_sprite");
    delay(6000);
}

static void frame_greenkey_sprite(void)
{
    Display.drawTestPattern();
    /* RGB565 sprite on solid key green: a red diamond with a white rim.  The
     * key must cut a hard silhouette with NO green fringe. */
    for (uint32_t y = 0; y < AS_MAX_H; y++) {
        for (uint32_t x = 0; x < AS_MAX_W; x++) {
            int32_t m = ((int32_t)x - 80 < 0 ? 80 - (int32_t)x : (int32_t)x - 80)
                      + ((int32_t)y - 60 < 0 ? 60 - (int32_t)y : (int32_t)y - 60);
            uint16_t p = GREEN_565;               /* keyed-out background */
            if (m <= 48) p = (m > 44) ? 0xFFFFu : 0xF800u;
            ((uint16_t *)s_as_565)[y * AS_MAX_W + x] = p;
        }
    }
    PXPSurface as(s_as_565, AS_MAX_W, AS_MAX_H, PXP_RGB565);
    ritual_composite(as, 280, 900, PXP_ALPHA_OVERRIDE, 0xFF, true, false);
    Serial1.println("FRAME=greenkey_sprite");
    delay(6000);
}

static void frame_fade(void)
{
    Serial1.println("FRAME=fade");
    /* Opaque colourful AS; override alpha swept 0 -> 255 in 16 steps over
     * ~3 s, recomposing from a clean backdrop each step, then held ~3 s. */
    for (uint32_t y = 0; y < AS_MAX_H; y++)
        for (uint32_t x = 0; x < AS_MAX_W; x++) {
            uint8_t r, g, b, a;
            as_pattern(x, y, r, g, b, a);
            ((uint32_t *)s_as_argb)[y * AS_MAX_W + x] =
                0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    PXPSurface as(s_as_argb, AS_MAX_W, AS_MAX_H, PXP_ARGB8888);
    for (uint32_t k = 0; k < 16; k++) {
        Display.drawTestPattern();      /* clean PS + one scanout */
        ritual_composite(as, 280, 580, PXP_ALPHA_OVERRIDE, (uint8_t)(k * 17u),
                         false, false);
        delay(160);
    }
    delay(3000);
}

static void frame_xor_rop(void)
{
    Display.drawTestPattern();
    /* All-ones AS + XORAS == NOT PS: an obviously colour-inverted block. */
    for (uint32_t i = 0; i < AS_MAX_W * AS_MAX_H; i++)
        ((uint16_t *)s_as_565)[i] = 0xFFFFu;
    PXPSurface as(s_as_565, AS_MAX_W, AS_MAX_H, PXP_RGB565);
    ritual_composite(as, 280, 580, PXP_ALPHA_EMBEDDED, 0xFF, false, true);
    Serial1.println("FRAME=xor_rop");
    delay(6000);
}

/* ================================ setup =================================== */

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("PXP_COMPOSITE_BEGIN");

    if (!Display.begin()) {
        Serial1.println("PANEL_FAIL");
        Serial1.println("PXP_COMPOSITE_DONE");
        return;
    }
    Serial1.println("PANEL_OK");
    s_fb = Display.framebuffer();

    /* PXP bring-up exactly as the bench does it: begin() performs the RM 52.5
     * reset ritual; afterwards CTRL's SFTRST and CLKGATE must both read clear
     * or the block is not actually out of reset. */
    bool pxp_up = PXP.begin();
    uint32_t ctrl = PXP_CTRL;
    if (!pxp_up || (ctrl & (PXP_CTRL_SFTRST | PXP_CTRL_CLKGATE))) {
        Serial1.println("PXP_FAIL");
        Serial1.println("PXP_COMPOSITE_DONE");
        return;
    }
    Serial1.println("PXP_OK");

    s_expected = (uint16_t *)alloc_ext(PANEL_FB_BYTES);
    s_as_argb  = alloc_ext(AS_MAX_BYTES);   /* 256x4 gradient strip fits too */
    s_as_rgba  = alloc_ext(AS_MAX_BYTES);
    s_as_xrgb  = alloc_ext(AS_MAX_BYTES);
    s_as_565   = alloc_ext(AS_MAX_W * AS_MAX_H * 2u);
    s_aout     = (uint32_t *)alloc_ext(AS_MAX_BYTES);
    if (!s_expected || !s_as_argb || !s_as_rgba || !s_as_xrgb ||
        !s_as_565 || !s_aout) {
        Serial1.println("ALLOC_FAIL");
        Serial1.println("PXP_COMPOSITE_DONE");
        return;
    }
    Serial1.println("ALLOC_OK");

    /* One full pattern fill; each case then re-fills only its own region
     * (earlier cases' residue is identical in fb and expected, so the whole-
     * frame sums still catch any out-of-bounds write). */
    for (uint32_t y = 0; y < PANEL_HEIGHT; y++)
        for (uint32_t x = 0; x < PANEL_WIDTH; x++)
            s_fb[y * PANEL_WIDTH + x] = ps_pattern(x, y);

    bool all_ok = true;
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < NUM_CASES; i++) {
        int r = run_case(CASES[i]);
        if (r < 0) { all_ok = false; continue; }   /* PXP_ERR line printed */
        emitted++;
        if (r == 0) all_ok = false;
    }

    run_alpha_out();   /* raw dump; contested knob 4; not counted */

    
    /* The eye on the same engine: four held frames. */
    frame_alpha_sprite();
    frame_greenkey_sprite();
    frame_fade();
    frame_xor_rop();

    /* Vacuous in QEMU (no timing model); the hardware number is the claim. */
    Display.sampleUnderrun(10);
    Serial1.printf("UNDERRUNS=%lu/%lu\n",
                   (unsigned long)Display.underrunCount(),
                   (unsigned long)Display.underrunFrames());
    Serial1.printf("CASES=%lu\n", (unsigned long)emitted);
    if (all_ok && emitted == NUM_CASES) Serial1.println("COMPOSITE_OK");
    Serial1.println("PXP_COMPOSITE_DONE");
}

void loop() {}
