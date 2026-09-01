/* Host-compiled test for the conformance probe's PATH CASE GEOMETRY.
 * Build: tests/run.sh
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * ★★ WHAT THIS TEST IS, AND WHAT IT IS NOT. Read this before quoting a green
 * run at anybody.
 *
 * It IS an exercise of vgc_cases_path.cpp's own geometry, sample points,
 * tolerances and predicates -- the REAL run()/check()/sum() functions, linked
 * and called -- against THREE MODELS of a GPU:
 *   - a correct one (a scanline reference rasteriser honouring every contour
 *     and both fill rules), under which all fifteen cases must report ok;
 *   - this GC355's KNOWN defect (the same rasteriser dropping every contour
 *     after the first), under which the six cases aimed at that defect must
 *     report broken BY NAME and every control must stay ok; and
 *   - one that draws NOTHING, under which fourteen of fifteen must go broken
 *     (degenerate-zero-area legitimately stays ok -- "nothing drawn" is an
 *     accepted outcome there).
 * ★ THE LAST ARM IS NOT REDUNDANT. Measured: a case hard-wired to VGC_OK
 * leaves arm 1 GREEN and arm 2 green for every control, and is caught ONLY by
 * arm 3. A positive-only suite is equally consistent with a matrix that cannot
 * detect anything.
 *
 * It is NOT a statement about what the real silicon does. Not one line here
 * touches a GPU. The silicon's answers live in the example's
 * transcript_hw_evkb.txt and expected_silicon.txt, and nothing in this file
 * can confirm, contradict or substitute for them. A future reader who reports
 * "the conformance cases pass" on the strength of this suite has said
 * something true and useless. Silicon wins; this is the instrument's
 * calibration, taken before the instrument is pointed at anything.
 *
 * ★ WHY IT EXISTS AT ALL: the QEMU gate cannot reach this code. QEMU has no
 * GC355, so the chip-ID probe reads 0 and every case reports pixel=skip --
 * meaning a green gate says NOTHING about a sample point, a tolerance or a
 * predicate. Between the gate and the one silicon boot there was no check on
 * any of it. This is that check, and it earned its place before it was
 * written: it caught an FP32 path array whose opcodes were encoded as
 * (float)VLC_OP_* rather than as the driver's one-byte-at-the-slot-base, which
 * renders NOTHING and would have reported two false `broken`s from the bench.
 *
 * ★ THE NEGATIVE ARM IS THE HALF THAT MATTERS. A suite that only ran the
 * correct-GPU model would pass against a case table that cannot detect
 * anything at all -- fifteen predicates hard-wired to VGC_OK included. The
 * first-contour-only arm is what says a `broken` on the bench is a GC355
 * finding rather than a harness artefact, and it is asserted here rather than
 * printed for the same reason every other guard in this tree is: an
 * observation nobody checks stops being true silently.
 *
 * ★ C++ RATHER THAN C, for the same reason arena_test is: vgc_harness.h wraps
 * its vg_lite.h include in `extern "C"` and the code under test is compiled as
 * C++ on the target.
 *
 * ★ CHECK(), NOT assert(), AND IT KEEPS GOING -- the tree's convention:
 * PASS:/FAIL: per check, a count at the end, and a red run still tells you
 * which parts of the matrix are trustworthy. */
#include "../vgc_harness.h"
#include "../vgc_predicates.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int failed = 0;
static int checks = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        checks++;                                                        \
        if (cond) {                                                      \
            printf("PASS: %s\n", #cond);                                 \
        } else {                                                         \
            printf("FAIL: %s  (line %d)\n", #cond, __LINE__);            \
            failed++;                                                    \
        }                                                                \
    } while (0)

/* Per-case variant: the failure message has to name the CASE, since the same
 * source line runs for all fifteen and `#cond` alone would not say which one
 * went red. */
#define CHECK_CASE(cond, id, what)                                       \
    do {                                                                 \
        checks++;                                                        \
        if (cond) {                                                      \
            printf("PASS: %s %s\n", (id), (what));                       \
        } else {                                                         \
            printf("FAIL: %s %s  (line %d)\n", (id), (what), __LINE__);  \
            failed++;                                                    \
        }                                                                \
    } while (0)

/* ---- the scratch buffer and the harness services the TARGET provides -------
 * On the board these live in vglite_conformance.cpp. Re-implemented here
 * exactly as documented in vgc_harness.h -- one access path (vgc_fb), vgc_px
 * counting out-of-range rather than answering, vgc_scratch_sum hashing the
 * flat packed byte count -- because the cases under test are specified against
 * those contracts and a convenient deviation here would test something else. */
static uint32_t s_fb[VGC_W * VGC_H];

/* Declared extern by the harness. The cases never read it (that is the whole
 * point of vgc_fb being the one access path), but the symbol must exist. */
vg_lite_buffer_t vgc_scratch;

const uint32_t *vgc_fb(void) { return s_fb; }

static uint32_t s_oob;
uint32_t vgc_px_oob(void)       { return s_oob; }
void     vgc_px_oob_reset(void) { s_oob = 0; }

uint32_t vgc_px(int x, int y)
{
    if (x < 0 || x >= VGC_W || y < 0 || y >= VGC_H) { s_oob++; return 0u; }
    return s_fb[(size_t)y * VGC_W + (size_t)x];
}

uint32_t vgc_scratch_sum(void) { return vgc_fnv(s_fb, sizeof(s_fb)); }

vg_lite_error_t vgc_clear(void)
{
    for (size_t i = 0; i < (size_t)VGC_W * VGC_H; i++) s_fb[i] = VGC_BG_COLOR;
    return VG_LITE_SUCCESS;
}

static vg_lite_matrix_t s_ident;
vg_lite_matrix_t *vgc_ident(void) { return &s_ident; }

/* The reference rasteriser is synchronous, so there is nothing to wait for and
 * nothing that can fail. */
void vgc_finish_into(vg_lite_error_t *acc) { (void)acc; }

/* ---- the stubbed driver entry point ---------------------------------------
 * ★ WHAT THIS MODELS AND WHAT IT DOES NOT. It models the three things the
 * cases depend on: it memsets the path, it records format/length/data, and it
 * stores the bounding box. It does NOT model the real function's
 * CLOSE->END fixup (vg_lite_path.c:200-231) -- it only DETECTS whether that
 * fixup would have fired, and counts it. Performing it is not an option: the
 * real S8 branch reads byte num-1 and writes at 4*(num-1), 29 bytes past the
 * end of an 11-byte array, and reproducing an out-of-bounds write in a host
 * test would corrupt the test rather than measure anything. The COUNT is the
 * assertion (see the close_fixup check in main): every path in the file under
 * test is supposed to end on an explicit VLC_OP_END so the branch never fires,
 * and this is the only place in the tree that can prove it does not. */
static int data_size_of(vg_lite_format_t f)
{
    return f == VG_LITE_S8 ? 1 : f == VG_LITE_S16 ? 2 : 4;
}

static int g_close_fixup_fired;

vg_lite_error_t vg_lite_init_path(vg_lite_path_t *path, vg_lite_format_t format,
                                  vg_lite_quality_t quality, uint32_t length,
                                  void *data, float min_x, float min_y,
                                  float max_x, float max_y)
{
    memset(path, 0, sizeof(*path));
    path->format         = format;
    path->quality        = quality;
    path->path_length    = length;
    path->path           = data;
    path->bounding_box[0] = min_x;
    path->bounding_box[1] = min_y;
    path->bounding_box[2] = max_x;
    path->bounding_box[3] = max_y;

    if (data && length) {
        const size_t ds  = (size_t)data_size_of(format);
        const size_t num = (size_t)length / ds;
        if (num && ((const unsigned char *)data)[(num - 1) * ds] == VLC_OP_CLOSE)
            g_close_fixup_fired++;
    }
    return VG_LITE_SUCCESS;
}

/* ---- the reference rasteriser ---------------------------------------------
 * A model of a CORRECT GPU: it parses the path exactly as the driver lays one
 * out, collects every contour, and fills by winding number (NON_ZERO) or
 * crossing parity (EVEN_ODD) sampled at pixel centres. No antialiasing --
 * deliberately, because the predicates under test threshold at ~50% coverage
 * and a hard-edged reference is the cleanest thing to hold them to. The one
 * cost is that pixel-centre sampling under-counts a diagonal edge, which is
 * why the triangle reads 1770 against its analytic 1800; the +/-8% tolerance
 * in the case under test has to hold that, and this is where that is checked.
 *
 * ★ PATH LAYOUT, taken from the driver rather than guessed (vg_lite_path.c
 * ~line 573: `*(pathc + offset) = cmd[i]; offset++;` then
 * `offset = CDALIGN(offset, data_size);`): an opcode is ONE BYTE at the base
 * of a slot, the cursor then re-aligns to the format's element width, and
 * coordinates follow at that width. That is why (float)VLC_OP_MOVE is not a
 * MOVE -- its first byte is 0x00, VLC_OP_END. */
#define GEOM_MAXPT  256
#define GEOM_MAXCON 32

static float g_ptx[GEOM_MAXPT], g_pty[GEOM_MAXPT];
static int   g_cstart[GEOM_MAXCON], g_clen[GEOM_MAXCON], g_ncon;
static int   g_parse_error;

/* Set by the negative arm: drop every contour after the first, which is
 * exactly what this GC355 does to a multi-contour path. */
static int g_one_contour_only;

static float read_coord(const unsigned char *b, size_t off, vg_lite_format_t f)
{
    if (f == VG_LITE_S8)  { int8_t  v; memcpy(&v, b + off, sizeof(v)); return (float)v; }
    if (f == VG_LITE_S16) { int16_t v; memcpy(&v, b + off, sizeof(v)); return (float)v; }
    if (f == VG_LITE_S32) { int32_t v; memcpy(&v, b + off, sizeof(v)); return (float)v; }
    { float v; memcpy(&v, b + off, sizeof(v)); return v; }
}

static void parse_path(const vg_lite_path_t *p)
{
    const unsigned char *b = (const unsigned char *)p->path;
    const size_t ds = (size_t)data_size_of(p->format);
    size_t off = 0;
    int npt = 0;

    g_ncon = 0;
    if (!b) { g_parse_error++; return; }

    while (off < (size_t)p->path_length) {
        const unsigned char op = b[off];
        off += 1;
        off = (off + ds - 1) / ds * ds;                 /* CDALIGN(offset, ds) */
        if (op == VLC_OP_END)   break;
        if (op == VLC_OP_CLOSE) continue;               /* contours are implicitly closed */
        if (op == VLC_OP_MOVE) {
            if (g_ncon >= GEOM_MAXCON) { g_parse_error++; return; }
            g_cstart[g_ncon] = npt;
            g_clen[g_ncon]   = 0;
            g_ncon++;
        } else if (op != VLC_OP_LINE) {
            g_parse_error++;                            /* the cases emit only these */
            return;
        }
        if (g_ncon == 0 || npt >= GEOM_MAXPT || off + 2 * ds > (size_t)p->path_length) {
            g_parse_error++;
            return;
        }
        g_ptx[npt] = read_coord(b, off, p->format); off += ds;
        g_pty[npt] = read_coord(b, off, p->format); off += ds;
        g_clen[g_ncon - 1]++;
        npt++;
    }
}

/* Set by arm 3: model a GPU that accepts everything and draws NOTHING. */
static int g_draw_nothing;

void vgc_draw_path(vg_lite_path_t *p, vg_lite_fill_t rule, uint32_t color,
                   vg_lite_error_t *acc)
{
    (void)acc;
    parse_path(p);
    if (g_one_contour_only && g_ncon > 1) g_ncon = 1;
    if (g_draw_nothing) g_ncon = 0;

    /* ★ THE BOUNDING BOX IS ENFORCED, because on hardware it IS enforced and
     * getting it wrong is silent. The driver derives its tessellation window
     * from path->bounding_box; a box that under-covers the geometry clips the
     * render while every vg_lite_* call returns SUCCESS -- the exact failure
     * class this whole example exists to catch. Every box in the file under
     * test is correct today, but nothing CHECKED that, and Phase 2 and 3
     * authors will write new ones. Honouring it here turns a bbox typo into a
     * host-visible failure instead of a bench cycle. Four lines. */
    const float bx0 = p->bounding_box[0], by0 = p->bounding_box[1];
    const float bx1 = p->bounding_box[2], by1 = p->bounding_box[3];

    for (int y = 0; y < VGC_H; y++) {
        const float sy = (float)y + 0.5f;
        if (sy < by0 || sy > by1) continue;
        for (int x = 0; x < VGC_W; x++) {
            const float sx = (float)x + 0.5f;
            if (sx < bx0 || sx > bx1) continue;
            int wind = 0, cross = 0;
            for (int c = 0; c < g_ncon; c++) {
                const int s = g_cstart[c], len = g_clen[c];
                for (int i = 0; i < len; i++) {
                    const float ax = g_ptx[s + i], ay = g_pty[s + i];
                    const float bx = g_ptx[s + (i + 1) % len];
                    const float by = g_pty[s + (i + 1) % len];
                    if ((ay <= sy) == (by <= sy)) continue;   /* no crossing */
                    const float t = (sy - ay) / (by - ay);
                    if (ax + t * (bx - ax) <= sx) continue;   /* ray runs to +x */
                    cross++;
                    wind += (by > ay) ? 1 : -1;
                }
            }
            const int in = (rule == VG_LITE_FILL_EVEN_ODD) ? (cross & 1) : (wind != 0);
            if (in) s_fb[(size_t)y * VGC_W + (size_t)x] = color;
        }
    }
}

/* ---- running one case, exactly as the harness does ------------------------
 * The sequence mirrors run_case() in vglite_conformance.cpp: reset the arena
 * and the oob counter, clear, run, sum, check -- then clear and run AGAIN and
 * sum, which is what makes the repeat= comparison meaningful. Deviating here
 * would test the cases under a lifecycle they never see. */
typedef struct {
    vgc_verdict_t   verdict;
    vg_lite_error_t api;
    vg_lite_error_t api2;      /* the SECOND run's status -- the harness prints it */
    uint32_t        oob;
    int             repeat_same;
    int             hook_distinct;  /* a sum() hook must not be the live buffer */
    char            detail[VGC_DETAIL_MAX];
} case_result_t;

static void run_one(const vgc_case_t *c, case_result_t *r)
{
    memset(r, 0, sizeof(*r));
    vgc_arena_reset();
    vgc_px_oob_reset();
    vgc_clear();
    r->api = c->run();
    const uint32_t live1 = vgc_scratch_sum();
    const uint32_t sum1  = c->sum ? c->sum() : live1;
    /* ★ A HOOK THAT RETURNS THE LIVE BUFFER IS A HOOK THAT DOES NOTHING, and
     * the failure is invisible: repeat= keeps comparing something, just not
     * every sub-render. vgc_harness.h names a multi-render case without a
     * working hook as its top risk, and this is the only place that can see
     * it. Cases WITHOUT a hook are exempt by construction -- for them sum1 IS
     * live1. */
    r->hook_distinct = c->sum ? (sum1 != live1) : 1;
    r->verdict = c->check(r->detail, sizeof(r->detail));
    r->oob = vgc_px_oob();

    vgc_arena_reset();
    vgc_clear();
    r->api2 = c->run();
    const uint32_t sum2 = c->sum ? c->sum() : vgc_scratch_sum();
    r->repeat_same = (sum1 == sum2);
}

/* The SIX cases aimed at the contour-encoding question. Everything else in
 * the table is a control and must survive that model unchanged.
 *
 * ★ THE PHASE 1b PAIR IS IN HERE FOR THE SAME REASON THE OTHERS ARE, AND FOR
 * NO STRONGER ONE. path/two-disjoint-bars and path/four-nested-rings are both
 * multi-contour paths, so a rasteriser that keeps only the first contour must
 * fail their predicates -- two bars collapse to one run, four alternating
 * rings collapse to the outer rect's one solid block. That is a statement
 * about THIS MODEL. The pair exists to separate disjointness from contour
 * COUNT on silicon, and this model cannot separate them: it drops contour two
 * onward whatever they are. Nothing here predicts the bench. */
static int is_multi_contour_probe(const char *id)
{
    /* ★ path/multi-contour-close-padded IS IN THIS SET, and its membership is
     * a statement about the MODEL, not about silicon. Both model rasterisers
     * read the opcode byte and ignore the slot's padding, so the padded case
     * is byte-for-byte the same geometry to them and must behave exactly like
     * multi-contour-disjoint in both arms. That agreement is what proves the
     * two cases really are one variable apart. It is NOT evidence about the
     * padding hypothesis -- only the bench can separate those, which is the
     * whole reason that case exists. */
    return strcmp(id, "path/multi-contour-disjoint")    == 0 ||
           strcmp(id, "path/multi-contour-close-padded") == 0 ||
           strcmp(id, "path/two-disjoint-bars")          == 0 ||
           strcmp(id, "path/four-nested-rings")          == 0 ||
           strcmp(id, "path/two-contour-ring-nonzero")  == 0 ||
           strcmp(id, "path/evenodd-vs-nonzero")        == 0;
}

int main(void)
{
    memset(&vgc_scratch, 0, sizeof(vgc_scratch));

    /* The table's size is part of what the gate and expected_silicon.txt key
     * on, and both arms below iterate it -- so an accidental table edit must
     * not quietly shrink what this suite covers. */
    CHECK(vgc_path_case_count == 15);

    /* ---- ARM 1: a CORRECT GPU. Everything must be ok. ---------------------- */
    printf("-- arm 1: correct rasteriser (all contours, both fill rules)\n");
    g_one_contour_only = 0;
    g_parse_error = 0;
    g_close_fixup_fired = 0;

    for (size_t i = 0; i < vgc_path_case_count; i++) {
        const vgc_case_t *c = &vgc_path_cases[i];
        case_result_t r;
        run_one(c, &r);
        printf("   %-34s %-6s %s\n", c->id,
               r.verdict == VGC_OK ? "ok" : r.verdict == VGC_BROKEN ? "BROKEN" : "skip",
               r.detail);
        CHECK_CASE(r.verdict == VGC_OK,   c->id, "verdict ok");
        /* ★ RELABELLED, because "api success" overstated it. This suite's
         * vgc_finish_into is a no-op and its vg_lite_draw always succeeds, so
         * the ONLY reachable non-success here is vgc_finish_path refusing an
         * arena overflow. That is worth pinning -- an overflow returns early
         * and leaves the case measuring a path it never drew -- but it is not
         * a statement about any driver. */
        CHECK_CASE(r.api == VG_LITE_SUCCESS,  c->id, "arena did not overflow");
        CHECK_CASE(r.oob == 0u,               c->id, "no out-of-range px read");
        CHECK_CASE(r.repeat_same,             c->id, "repeat identical");
        /* api2 is a field the harness PRINTS and the gate can assert on; with
         * the second run's status discarded it was never exercised at all. */
        CHECK_CASE(r.api2 == r.api,           c->id, "second run's status matches");
        CHECK_CASE(r.hook_distinct,           c->id, "sum hook is not the live buffer");

        /* One numeric pin, on the baseline only. 6400 is the exact analytic
         * area of an 80x80 integer-aligned rect under centre sampling, so it
         * catches a reference off by a row or a column -- which would silently
         * shift every tolerance judgement this suite makes. The triangle's
         * 1770 is NOT pinned: it is an artefact of centre sampling, and
         * pinning it would fight a legitimately better rasteriser. */
        if (strcmp(c->id, "path/single-contour-rect") == 0)
            CHECK_CASE(strstr(r.detail, "fill=6400,expect=6400") != NULL,
                       c->id, "fill is the exact analytic area");

        /* ★ THE NESTED RINGS' FILL IS PINNED FOR THE SAME REASON, and it is
         * the case in the table where a right VERDICT is cheapest to get for
         * a wrong reason. Its predicate counts RUNS down one column, and four
         * runs is a number several wrong pictures can produce -- a mis-nested
         * set of rects, or an alternation that pairs the wrong rects. 5760 is
         * the exact analytic area of the alternating nest,
         * (96^2-72^2) + (48^2-24^2) = 4032 + 1728, with no antialiasing in
         * this model and every edge integer-aligned, so it is arithmetic
         * rather than a tolerance. Dropping the alternation makes it the
         * solid 9216; a shifted rect moves it. Either shows up as a NUMBER
         * instead of only a verdict.
         *
         * On the TARGET the same field is a reading, not a bound: hardware
         * antialiasing moves it by a perimeter's worth, which is why
         * check_four_nested judges on runs alone and prints expfill= beside
         * it. Pinning it here costs nothing there. */
        if (strcmp(c->id, "path/four-nested-rings") == 0)
            CHECK_CASE(strstr(r.detail, "runs=4,expect=4,fill=5760") != NULL,
                       c->id, "four bands and the exact analytic area");

        /* Its partner is the baseline rect's arithmetic twice over
         * (2 x 80x16 = 2560), and runs= is what the whole
         * disjoint-vs-nested comparison is read from -- so pin both rather
         * than inferring either from the verdict. */
        if (strcmp(c->id, "path/two-disjoint-bars") == 0)
            CHECK_CASE(strstr(r.detail, "runs=2,expect=2,fill=2560") != NULL,
                       c->id, "two bands and the exact analytic area");
    }
    CHECK(g_parse_error == 0);

    /* ★ Every path in the file under test ends on an explicit VLC_OP_END, so
     * vg_lite_init_path's CLOSE->END fixup -- whose S8 branch writes 4x out of
     * bounds -- must never fire. Nothing on the target can check this; a hit
     * there presents as memory corruption, not as a failed call. */
    CHECK(g_close_fixup_fired == 0);

    /* ---- ARM 2: THIS GC355's DEFECT. The probes must go red. --------- */
    printf("-- arm 2: first-contour-only rasteriser (this GC355's defect)\n");
    g_one_contour_only = 1;
    g_parse_error = 0;
    g_close_fixup_fired = 0;

    for (size_t i = 0; i < vgc_path_case_count; i++) {
        const vgc_case_t *c = &vgc_path_cases[i];
        case_result_t r;
        run_one(c, &r);
        const int probe = is_multi_contour_probe(c->id);
        printf("   %-34s %-6s %s\n", c->id,
               r.verdict == VGC_OK ? "ok" : r.verdict == VGC_BROKEN ? "BROKEN" : "skip",
               r.detail);
        CHECK_CASE(r.verdict == (probe ? VGC_BROKEN : VGC_OK), c->id,
                   probe ? "goes BROKEN on the defect" : "control unaffected");
        CHECK_CASE(r.oob == 0u, c->id, "no out-of-range px read");
    }

    /* ★ BROKEN FOR THE RIGHT REASON. A verdict alone cannot distinguish "this
     * predicate saw the dropped contours" from "this predicate went red for
     * some unrelated reason", and the second would leave the matrix looking
     * healthy while measuring nothing. These pin the SYMPTOM each probe is
     * supposed to report. */
    {
        case_result_t r;
        for (size_t i = 0; i < vgc_path_case_count; i++) {
            const vgc_case_t *c = &vgc_path_cases[i];
            if (!is_multi_contour_probe(c->id)) continue;
            run_one(c, &r);
            if (strncmp(c->id, "path/multi-contour", 18) == 0)
                CHECK_CASE(strstr(r.detail, "runs=1,") != NULL, c->id,
                           "reports one run, not four");
            else if (strcmp(c->id, "path/two-disjoint-bars") == 0)
                CHECK_CASE(strstr(r.detail, "runs=1,") != NULL, c->id,
                           "reports one run, not two");
            else if (strcmp(c->id, "path/four-nested-rings") == 0)
                /* The outer rect alone under NON_ZERO is one solid block, so
                 * the counter reads 1 -- the k=1 cell of its own table. */
                CHECK_CASE(strstr(r.detail, "runs=1,") != NULL, c->id,
                           "reports one run, not four");
            else if (strcmp(c->id, "path/two-contour-ring-nonzero") == 0)
                CHECK_CASE(strstr(r.detail, "rim=1,centre=1") != NULL, c->id,
                           "reports the hole filled in");
            else
                CHECK_CASE(strstr(r.detail, "eo_centre=1") != NULL, c->id,
                           "reports EVEN_ODD failing to cut the hole");
        }
    }
    CHECK(g_parse_error == 0);
    CHECK(g_close_fixup_fired == 0);

    /* ---- ARM 3: a GPU that accepts everything and DRAWS NOTHING. ---------
     * ★ ARM 2 LEAVES A HOLE THIS CLOSES. Under the first-contour model nine of
     * the fifteen cases are controls that must stay ok -- so nine predicates
     * hard-wired to `return VGC_OK` would survive both arms so far. A null GPU
     * is the cheapest model that forces almost all of them to speak: fourteen
     * of fifteen must go broken. The exception is real rather than a
     * concession -- path/degenerate-zero-area accepts "nothing drawn" BY
     * DESIGN (a zero-area path legitimately rasterises to nothing), so it is
     * asserted ok here, which also pins that its `fill == 0` branch is live. */
    printf("-- arm 3: null rasteriser (every call succeeds, nothing is drawn)\n");
    g_one_contour_only = 0;
    g_draw_nothing = 1;
    g_parse_error = 0;

    for (size_t i = 0; i < vgc_path_case_count; i++) {
        const vgc_case_t *c = &vgc_path_cases[i];
        case_result_t r;
        run_one(c, &r);
        const int degenerate = strcmp(c->id, "path/degenerate-zero-area") == 0;
        printf("   %-34s %-6s %s\n", c->id,
               r.verdict == VGC_OK ? "ok" : r.verdict == VGC_BROKEN ? "BROKEN" : "skip",
               r.detail);
        CHECK_CASE(r.verdict == (degenerate ? VGC_OK : VGC_BROKEN), c->id,
                   degenerate ? "accepts an empty render by design"
                              : "goes BROKEN when nothing is drawn");
    }
    g_draw_nothing = 0;
    CHECK(g_parse_error == 0);

    printf("--\n");
    if (failed) {
        printf("cases_path_geom_test: FAILED (%d of %d checks)\n", failed, checks);
        return 1;
    }
    printf("cases_path_geom_test: OK (%d checks)\n", checks);
    return 0;
}
