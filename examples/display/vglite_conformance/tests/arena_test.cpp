/* Host-compiled unit test for the conformance probe's PATH ARENA.
 * Build: tests/run.sh
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * The arena is the piece of this harness whose bugs do not look like bugs. A
 * truncated path has no VLC_OP_END, and unterminated path data hangs the
 * Vivante front end while every vg_lite_* call keeps returning
 * VG_LITE_SUCCESS -- so an arena defect presents as a board that died in the
 * middle of the matrix, with the case_begin line of an innocent case as the
 * last thing on the wire. There is no way to bisect that from the transcript.
 * It gets settled here instead, against a stubbed driver, before any GPU is
 * asked anything.
 *
 * ★ C++ RATHER THAN C, unlike predicates_test.c, and not by preference:
 * vgc_harness.h wraps its vg_lite.h include in `extern "C"` and is compiled
 * into C++ TUs on the target. Building this test as C would either need that
 * guard weakened or would link a C caller against C++-mangled arena symbols.
 * The predicates test stays C because vgc_predicates.h genuinely is pure C.
 *
 * ★ CHECK(), NOT assert(), AND IT KEEPS GOING -- same convention as
 * predicates_test.c and the rest of the tree: PASS:/FAIL: per check, a count
 * at the end, and a red run still tells you which parts of the arena are
 * trustworthy. */
#include "../vgc_harness.h"
#include <stdio.h>
#include <string.h>

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

/* ---- the stubbed driver entry point --------------------------------------
 * Records what the arena handed it, and otherwise behaves as the real
 * vg_lite_init_path does in the ways this test depends on: it MEMSETS the
 * path first (vg_lite_path.c:188) and then fills in the fields. The memset is
 * load-bearing for the overflow case below -- because init_path is NOT called
 * on that path, a zeroed *p there can only have come from vgc_finish_path
 * itself, which is the regression being pinned. */
static struct {
    int               calls;
    uint32_t          length;
    void             *data;
    vg_lite_format_t  format;
    vg_lite_quality_t quality;
    float             bb[4];
} g_last;

vg_lite_error_t vg_lite_init_path(vg_lite_path_t *path,
                                  vg_lite_format_t format,
                                  vg_lite_quality_t quality,
                                  uint32_t length,
                                  void *data,
                                  float min_x, float min_y,
                                  float max_x, float max_y)
{
    g_last.calls++;
    g_last.length  = length;
    g_last.data    = data;
    g_last.format  = format;
    g_last.quality = quality;
    g_last.bb[0] = min_x; g_last.bb[1] = min_y;
    g_last.bb[2] = max_x; g_last.bb[3] = max_y;

    memset(path, 0, sizeof(*path));
    path->format = format;
    path->quality = quality;
    path->path_length = length;
    path->path = data;
    path->bounding_box[0] = min_x; path->bounding_box[1] = min_y;
    path->bounding_box[2] = max_x; path->bounding_box[3] = max_y;
    return VG_LITE_SUCCESS;
}

/* ---- helpers -------------------------------------------------------------- */

/* A rect contour is 4 vertices (op + 2 coords) plus CLOSE; vgc_finish_path
 * then appends END. Spelled out rather than hardcoded as 14 so a reader can
 * see where the number comes from. */
#define RECT_WORDS   (4 * 3 + 1)
#define PATH_WORDS   (RECT_WORDS + 1)
#define PATH_BYTES   ((uint32_t)(PATH_WORDS * sizeof(int32_t)))

/* Fill *p with a recognisable non-zero pattern, so "left zeroed" is a real
 * assertion rather than one satisfied by a fresh stack frame that happened to
 * be zero already. */
static void poison(vg_lite_path_t *p) { memset(p, 0xA5, sizeof(*p)); }

static int all_zero(const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) if (b[i]) return 0;
    return 1;
}

static const int32_t *words(const vg_lite_path_t *p)
{
    return (const int32_t *)p->path;
}

int main(void)
{
    vg_lite_path_t p1, p2, p3;

    /* --- one path: word count, contents, bounds, format ------------------- */
    printf("-- single path --\n");
    vgc_arena_reset();
    vgc_emit_rect_cw(10, 20, 30, 40);
    poison(&p1);
    CHECK(vgc_finish_path(&p1, 10.0f, 20.0f, 40.0f, 60.0f) == VG_LITE_SUCCESS);
    CHECK(g_last.calls == 1);
    CHECK(p1.path_length == PATH_BYTES);
    CHECK(p1.path == g_last.data);
    CHECK(p1.format == VG_LITE_S32);
    CHECK(p1.quality == VG_LITE_HIGH);

    /* The CW winding, vertex by vertex. Pinning the opcodes AND the
     * coordinates is what makes vgc_emit_rect_cw's (x+w, y+h) arithmetic
     * testable -- a transposed corner still emits 13 words. */
    CHECK(words(&p1)[0]  == VLC_OP_MOVE  && words(&p1)[1]  == 10 && words(&p1)[2]  == 20);
    CHECK(words(&p1)[3]  == VLC_OP_LINE  && words(&p1)[4]  == 40 && words(&p1)[5]  == 20);
    CHECK(words(&p1)[6]  == VLC_OP_LINE  && words(&p1)[7]  == 40 && words(&p1)[8]  == 60);
    CHECK(words(&p1)[9]  == VLC_OP_LINE  && words(&p1)[10] == 10 && words(&p1)[11] == 60);
    CHECK(words(&p1)[12] == VLC_OP_CLOSE);

    /* ★ EVERY FINISHED PATH ENDS IN VLC_OP_END. This is the assertion the
     * whole file exists for: it is the difference between a path the front end
     * retires and one it hangs on. */
    CHECK(words(&p1)[PATH_WORDS - 1] == VLC_OP_END);

    /* Bounds are padded one unit on every side. */
    CHECK(p1.bounding_box[0] == 9.0f  && p1.bounding_box[1] == 19.0f);
    CHECK(p1.bounding_box[2] == 41.0f && p1.bounding_box[3] == 61.0f);

    /* --- CCW winding emits the mirrored vertex order ---------------------- */
    printf("-- ccw winding --\n");
    vgc_arena_reset();
    vgc_emit_rect_ccw(10, 20, 30, 40);
    CHECK(vgc_finish_path(&p1, 10.0f, 20.0f, 40.0f, 60.0f) == VG_LITE_SUCCESS);
    /* Same corners, opposite traversal: down the left edge first. A CCW that
     * accidentally duplicated the CW order would make every non-zero
     * hole-cutting case in Task 3 silently measure the wrong thing. */
    CHECK(words(&p1)[0] == VLC_OP_MOVE && words(&p1)[1] == 10 && words(&p1)[2] == 20);
    CHECK(words(&p1)[3] == VLC_OP_LINE && words(&p1)[4] == 10 && words(&p1)[5] == 60);
    CHECK(words(&p1)[6] == VLC_OP_LINE && words(&p1)[7] == 40 && words(&p1)[8] == 60);
    CHECK(words(&p1)[9] == VLC_OP_LINE && words(&p1)[10] == 40 && words(&p1)[11] == 20);
    CHECK(words(&p1)[PATH_WORDS - 1] == VLC_OP_END);

    /* --- two sequential paths must not overlap ---------------------------- */
    printf("-- two sequential paths --\n");
    vgc_arena_reset();
    vgc_emit_rect_cw(0, 0, 10, 10);
    CHECK(vgc_finish_path(&p1, 0.0f, 0.0f, 10.0f, 10.0f) == VG_LITE_SUCCESS);
    vgc_emit_rect_ccw(20, 20, 10, 10);
    CHECK(vgc_finish_path(&p2, 20.0f, 20.0f, 30.0f, 30.0f) == VG_LITE_SUCCESS);

    /* ★ THE SECOND PATH STARTS EXACTLY WHERE THE FIRST ENDED. Without
     * `s_start = s_used` the second path would be initialised over the FIRST
     * path's words as well as its own -- same pointer, double the length --
     * and would render both shapes on every draw. On silicon that reads as a
     * GC355 quirk, not as an arena bug. */
    CHECK(p2.path == (const void *)(words(&p1) + PATH_WORDS));
    CHECK(p1.path_length == PATH_BYTES);
    CHECK(p2.path_length == PATH_BYTES);
    CHECK((const char *)p2.path >= (const char *)p1.path + p1.path_length);
    /* and the first path's data survived the second's emission */
    CHECK(words(&p1)[0] == VLC_OP_MOVE);
    CHECK(words(&p1)[PATH_WORDS - 1] == VLC_OP_END);
    CHECK(words(&p2)[PATH_WORDS - 1] == VLC_OP_END);

    /* --- reset mid-sequence rewinds to the start -------------------------- */
    printf("-- reset mid-sequence --\n");
    vgc_arena_reset();
    vgc_emit_rect_cw(5, 5, 10, 10);
    CHECK(vgc_finish_path(&p3, 5.0f, 5.0f, 15.0f, 15.0f) == VG_LITE_SUCCESS);
    CHECK(p3.path == p1.path);   /* p1 was the first path after the last reset */

    /* --- overflow REFUSES, and leaves *p safe ----------------------------- */
    printf("-- overflow --\n");
    vgc_arena_reset();
    for (int i = 0; i < VGC_ARENA_WORDS; i++) vgc_emit(VLC_OP_LINE);
    const int calls_before = g_last.calls;
    poison(&p1);
    /* The arena is exactly full, so vgc_finish_path's own END emit overflows. */
    CHECK(vgc_finish_path(&p1, 0.0f, 0.0f, 1.0f, 1.0f) == VG_LITE_OUT_OF_RESOURCES);
    /* It must REFUSE rather than hand the driver a path with no END. */
    CHECK(g_last.calls == calls_before);
    /* ★ AND IT MUST LEAVE *p ZEROED, NOT AS THE STACK FOUND IT. Callers
     * declare vg_lite_path_t on the stack; a caller that dropped the status
     * and drew anyway would otherwise submit a path pointer and a length made
     * of garbage -- the exact unterminated-path-data condition that hangs the
     * front end. Zeroed, the worst it can do is submit nothing. */
    CHECK(all_zero(&p1, sizeof(p1)));
    CHECK(p1.path == NULL);
    CHECK(p1.path_length == 0u);

    /* --- a path emitted AFTER an overflow is still refused ----------------- */
    printf("-- overflow is not self-clearing --\n");
    /* No reset: the arena is still full. Emitting more must not silently
     * succeed by wrapping, reusing, or truncating. */
    vgc_emit_rect_cw(0, 0, 5, 5);
    poison(&p2);
    CHECK(vgc_finish_path(&p2, 0.0f, 0.0f, 5.0f, 5.0f) == VG_LITE_OUT_OF_RESOURCES);
    CHECK(g_last.calls == calls_before);
    CHECK(all_zero(&p2, sizeof(p2)));

    /* ...and a reset makes the arena usable again, so the refusal is a state
     * the probe recovers from rather than a latch that kills the matrix. */
    vgc_arena_reset();
    vgc_emit_rect_cw(1, 2, 3, 4);
    CHECK(vgc_finish_path(&p3, 1.0f, 2.0f, 4.0f, 6.0f) == VG_LITE_SUCCESS);
    CHECK(g_last.calls == calls_before + 1);
    CHECK(words(&p3)[PATH_WORDS - 1] == VLC_OP_END);

    printf("--\n");
    if (failed) {
        printf("arena_test: FAILED (%d of %d checks)\n", failed, checks);
        return 1;
    }
    printf("arena_test: OK (%d checks)\n", checks);
    return 0;
}
