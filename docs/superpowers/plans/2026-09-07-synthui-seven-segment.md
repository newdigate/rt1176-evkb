# SynthUI SevenSegment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the DC SevenSegment primitive as `synthui_seven_segment` in SynthUI (LVGL 9, sw geometric primitives delta rendering), verified by host unit tests and a new double-buffered QEMU-gated example `examples/display/synthui_seven_segment_test` in rt1176-evkb.

**Architecture:** Spec: [`docs/superpowers/specs/2026-09-07-synthui-seven-segment-design.md`](file:///Users/moolet/Development/rt1170/rt1176-evkb/docs/superpowers/specs/2026-09-07-synthui-seven-segment-design.md). Single widget TU in SynthUI with clean-room vector reconstruction of 7 hexagonal segments (decomposed to convex triangles) plus centered dot and colon punctuation cells. Early-return guards and in-place changed-cell bounding box delta invalidation. The consumer example runs the hardware double-buffer pipeline (`lvgl_mipi_panel_create_db()`) and executes Phase A correctness (pinned golden CRC, 64-step deterministic delta sequence, delta-equality guard, damage bounds) and Phase B (continuous animation benchmark).

**Tech Stack:** LVGL 9.4.0 (vendored in rt1176-evkb tree), SynthUI (`/Users/moolet/Development/SynthUI`, local-first), rt1176-evkb CMake + QEMU (`/Users/moolet/Development/qemu-rt1170/build/qemu-system-arm`).

## Global Constraints
- Clean-room provenance rules: `reference/` is never compiled; clean-room vector rebuilds from written descriptions only.
- `./run_qemu.sh`, never `sh run_qemu.sh`.
- Use the QEMU binary at `/Users/moolet/Development/qemu-rt1170/build/qemu-system-arm`.
- Goldens are RECORDED from two bit-identical consecutive QEMU runs, never derived.
- A demonstrated-RED is required for gate assertions.

---

### Task 1: `synthui_seven_segment_types.h`, `synthui_seven_segment_math.h` + Host Unit Test (SynthUI repo)

**Files:**
- Create: `/Users/moolet/Development/SynthUI/src/synthui_seven_segment_types.h`
- Create: `/Users/moolet/Development/SynthUI/src/synthui_seven_segment_math.h`
- Create: `/Users/moolet/Development/SynthUI/tests/seven_segment_test.c`
- Modify: `/Users/moolet/Development/SynthUI/tests/run.sh`

**Interfaces:**
- Produces: `synthui_seven_segment_types.h`, `synthui_seven_segment_math.h`, `synthui_seven_segment_geom_t`, `synthui_seven_segment_cell_geom_t`, `synthui_seven_segment_triangle_t`, `synthui_seven_segment_point_t`, `synthui_seven_segment_circle_t`, `synthui_seven_segment_compute_layout()`, `synthui_seven_segment_get_segment_geom()`, `synthui_seven_segment_get_char_mask()`

- [ ] **Step 1: Write the failing host test**

Create `/Users/moolet/Development/SynthUI/tests/seven_segment_test.c`:
```c
/* seven_segment_test.c - host unit test for pure seven segment geometry & math.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#undef NDEBUG
#include "../src/synthui_seven_segment_math.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int approx_eq(float a, float b) { return fabsf(a - b) < 0.05f; }

int main(void)
{
    synthui_seven_segment_geom_t g;

    /* 1. Normal layout: "140.0" at height 96, default slant 6.0 deg */
    assert(synthui_seven_segment_compute_layout("140.0", 96.0f, 6.0f, &g));
    assert(g.num_cells == 5);
    assert(approx_eq(g.h, 96.0f));
    assert(approx_eq(g.u, 96.0f / 112.0f));
    assert(approx_eq(g.slant_deg, 6.0f));
    assert(approx_eq(g.shear, tanf(6.0f * (float)M_PI / 180.0f)));
    assert(approx_eq(g.overhang, 112.0f * g.shear));

    /* Cell advances: '1', '4', '0' -> 76; '.' -> 44; '0' -> 76 */
    assert(approx_eq(g.cells[0].w, 76.0f));
    assert(approx_eq(g.cells[1].w, 76.0f));
    assert(approx_eq(g.cells[2].w, 76.0f));
    assert(approx_eq(g.cells[3].w, 44.0f)); /* Centered '.' */
    assert(approx_eq(g.cells[4].w, 76.0f));
    assert(approx_eq(g.cells[0].x, 0.0f));
    assert(approx_eq(g.cells[1].x, 76.0f));
    assert(approx_eq(g.cells[2].x, 152.0f));
    assert(approx_eq(g.cells[3].x, 228.0f));
    assert(approx_eq(g.cells[4].x, 272.0f));

    /* Total width in viewbox units = 348 + overhang */
    assert(approx_eq(g.total_view_w, 272.0f + 76.0f + g.overhang));

    /* 2. Character bitmasks */
    assert(synthui_seven_segment_get_char_mask('0') == (SYNTHUI_SEG_A | SYNTHUI_SEG_B | SYNTHUI_SEG_C | SYNTHUI_SEG_D | SYNTHUI_SEG_E | SYNTHUI_SEG_F));
    assert(synthui_seven_segment_get_char_mask('1') == (SYNTHUI_SEG_B | SYNTHUI_SEG_C));
    assert(synthui_seven_segment_get_char_mask('8') == (SYNTHUI_SEG_A | SYNTHUI_SEG_B | SYNTHUI_SEG_C | SYNTHUI_SEG_D | SYNTHUI_SEG_E | SYNTHUI_SEG_F | SYNTHUI_SEG_G));
    assert(synthui_seven_segment_get_char_mask('-') == SYNTHUI_SEG_G);
    assert(synthui_seven_segment_get_char_mask(' ') == 0);
    assert(synthui_seven_segment_get_char_mask('.') == SYNTHUI_SEG_DOT);
    assert(synthui_seven_segment_get_char_mask(':') == SYNTHUI_SEG_COLON);

    /* 3. Punctuation geometry centering */
    synthui_seven_segment_punct_geom_t pg;
    synthui_seven_segment_get_punct_geom(&g.cells[3], '.', &pg);
    assert(pg.is_colon == false);
    assert(pg.num_circles == 1);
    assert(approx_eq(pg.circles[0].cx, 22.0f)); /* Centered in 44-width cell */
    assert(approx_eq(pg.circles[0].cy, 100.0f));
    assert(approx_eq(pg.circles[0].r, 7.0f));

    synthui_seven_segment_cell_geom_t colon_cell;
    colon_cell.x = 100.0f;
    colon_cell.w = 44.0f;
    colon_cell.ch = ':';
    synthui_seven_segment_get_punct_geom(&colon_cell, ':', &pg);
    assert(pg.is_colon == true);
    assert(pg.num_circles == 2);
    assert(approx_eq(pg.circles[0].cx, 22.0f));
    assert(approx_eq(pg.circles[0].cy, 38.0f));
    assert(approx_eq(pg.circles[0].r, 6.0f));
    assert(approx_eq(pg.circles[1].cx, 22.0f));
    assert(approx_eq(pg.circles[1].cy, 76.0f));
    assert(approx_eq(pg.circles[1].r, 6.0f));

    /* 4. Hexagon segment geometry for 'a' (horizontal) */
    synthui_seven_segment_poly_geom_t sg;
    synthui_seven_segment_get_segment_geom(&g.cells[0], 0 /* seg a */, g.shear, &sg);
    assert(sg.num_triangles == 4);
    assert(sg.num_verts == 6);
    /* Vertex 0 unslanted would be (15, 12). Sheared: 15 + (112 - 12) * shear */
    assert(approx_eq(sg.verts[0].y, 12.0f));
    assert(approx_eq(sg.verts[0].x, 15.0f + (112.0f - 12.0f) * g.shear));

    /* 5. Clamping strings > 16 chars */
    assert(synthui_seven_segment_compute_layout("01234567890123456789", 96.0f, 6.0f, &g));
    assert(g.num_cells == SYNTHUI_SEVEN_SEGMENT_MAX_CHARS);

    /* 6. Degenerate inputs */
    assert(!synthui_seven_segment_compute_layout("140", 0.0f, 6.0f, &g));
    assert(!synthui_seven_segment_compute_layout("140", -10.0f, 6.0f, &g));
    assert(!synthui_seven_segment_compute_layout(NULL, 96.0f, 6.0f, &g));

    /* 7. Color constants */
    assert(SYNTHUI_SEVEN_SEGMENT_COLOR_BLUE_ON == 0xDCECFF);
    assert(SYNTHUI_SEVEN_SEGMENT_COLOR_BLUE_GLOW == 0x90A8F0);
    assert(SYNTHUI_SEVEN_SEGMENT_COLOR_CYAN_ON == 0xD8F0F0);
    assert(SYNTHUI_SEVEN_SEGMENT_COLOR_AMBER_ON == 0xFFE0A8);
    assert(SYNTHUI_SEVEN_SEGMENT_COLOR_RED_ON == 0xFFC8C0);

    printf("seven_segment_test: all PASS\n");
    return 0;
}
```

Add to `/Users/moolet/Development/SynthUI/tests/run.sh`:
```sh
cc -Wall -Wextra -Werror -o "$out/seven_segment_test" tests/seven_segment_test.c
"$out/seven_segment_test"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/moolet/Development/SynthUI && ./tests/run.sh`
Expected: FAIL (`fatal error: '../src/synthui_seven_segment_math.h' file not found`).

- [ ] **Step 3: Implement `synthui_seven_segment_types.h` and `synthui_seven_segment_math.h`**

Create `/Users/moolet/Development/SynthUI/src/synthui_seven_segment_types.h`:
```c
/* synthui_seven_segment_types.h - types and constants for SynthUI SevenSegment.
 * Header-only and LVGL-free for host testing.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_SEVEN_SEGMENT_TYPES_H
#define SYNTHUI_SEVEN_SEGMENT_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNTHUI_SEVEN_SEGMENT_MAX_CHARS 16

/* Standard DC reference accent colors (0xRRGGBB) */
#define SYNTHUI_SEVEN_SEGMENT_COLOR_BLUE_ON     0xDCECFF
#define SYNTHUI_SEVEN_SEGMENT_COLOR_BLUE_GLOW   0x90A8F0
#define SYNTHUI_SEVEN_SEGMENT_COLOR_CYAN_ON     0xD8F0F0
#define SYNTHUI_SEVEN_SEGMENT_COLOR_CYAN_GLOW   0x78C8D8
#define SYNTHUI_SEVEN_SEGMENT_COLOR_AMBER_ON    0xFFE0A8
#define SYNTHUI_SEVEN_SEGMENT_COLOR_AMBER_GLOW  0xF0A030
#define SYNTHUI_SEVEN_SEGMENT_COLOR_RED_ON      0xFFC8C0
#define SYNTHUI_SEVEN_SEGMENT_COLOR_RED_GLOW    0xE04848

#define SYNTHUI_SEVEN_SEGMENT_COLOR_DEFAULT_ON   SYNTHUI_SEVEN_SEGMENT_COLOR_BLUE_ON
#define SYNTHUI_SEVEN_SEGMENT_COLOR_DEFAULT_GLOW SYNTHUI_SEVEN_SEGMENT_COLOR_BLUE_GLOW

/* Segment bitmasks (a-g, dot, colon) */
#define SYNTHUI_SEG_A     (1u << 0)
#define SYNTHUI_SEG_B     (1u << 1)
#define SYNTHUI_SEG_C     (1u << 2)
#define SYNTHUI_SEG_D     (1u << 3)
#define SYNTHUI_SEG_E     (1u << 4)
#define SYNTHUI_SEG_F     (1u << 5)
#define SYNTHUI_SEG_G     (1u << 6)
#define SYNTHUI_SEG_DOT   (1u << 7)
#define SYNTHUI_SEG_COLON (1u << 8)

#ifdef __cplusplus
}
#endif
#endif /* SYNTHUI_SEVEN_SEGMENT_TYPES_H */
```

Create `/Users/moolet/Development/SynthUI/src/synthui_seven_segment_math.h`:
```c
/* synthui_seven_segment_math.h - pure geometry arithmetic for SynthUI SevenSegment.
 * Header-only and LVGL-free for direct host unit testing.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_SEVEN_SEGMENT_MATH_H
#define SYNTHUI_SEVEN_SEGMENT_MATH_H

#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include "synthui_seven_segment_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float x, y;
} synthui_seven_segment_point_t;

typedef struct {
    synthui_seven_segment_point_t p[3];
} synthui_seven_segment_triangle_t;

typedef struct {
    float cx, cy, r;
} synthui_seven_segment_circle_t;

typedef struct {
    char ch;
    float x;    /* Local X start in viewbox units */
    float w;    /* Width in viewbox units (76 for digit, 44 for punctuation) */
} synthui_seven_segment_cell_geom_t;

typedef struct {
    float h;
    float u;            /* Scale factor: h / 112.0f */
    float slant_deg;
    float shear;        /* tan(slant_deg) */
    float overhang;     /* 112.0f * shear */
    float total_view_w; /* Total viewbox width */
    int num_cells;
    synthui_seven_segment_cell_geom_t cells[SYNTHUI_SEVEN_SEGMENT_MAX_CHARS];
} synthui_seven_segment_geom_t;

typedef struct {
    int num_verts;
    synthui_seven_segment_point_t verts[6];
    int num_triangles;
    synthui_seven_segment_triangle_t triangles[4];
} synthui_seven_segment_poly_geom_t;

typedef struct {
    bool is_colon;
    int num_circles;
    synthui_seven_segment_circle_t circles[2];
} synthui_seven_segment_punct_geom_t;

static inline uint16_t synthui_seven_segment_get_char_mask(char ch)
{
    switch (ch) {
    case '0': return SYNTHUI_SEG_A | SYNTHUI_SEG_B | SYNTHUI_SEG_C | SYNTHUI_SEG_D | SYNTHUI_SEG_E | SYNTHUI_SEG_F;
    case '1': return SYNTHUI_SEG_B | SYNTHUI_SEG_C;
    case '2': return SYNTHUI_SEG_A | SYNTHUI_SEG_B | SYNTHUI_SEG_G | SYNTHUI_SEG_E | SYNTHUI_SEG_D;
    case '3': return SYNTHUI_SEG_A | SYNTHUI_SEG_B | SYNTHUI_SEG_G | SYNTHUI_SEG_C | SYNTHUI_SEG_D;
    case '4': return SYNTHUI_SEG_F | SYNTHUI_SEG_G | SYNTHUI_SEG_B | SYNTHUI_SEG_C;
    case '5': return SYNTHUI_SEG_A | SYNTHUI_SEG_F | SYNTHUI_SEG_G | SYNTHUI_SEG_C | SYNTHUI_SEG_D;
    case '6': return SYNTHUI_SEG_A | SYNTHUI_SEG_F | SYNTHUI_SEG_G | SYNTHUI_SEG_E | SYNTHUI_SEG_C | SYNTHUI_SEG_D;
    case '7': return SYNTHUI_SEG_A | SYNTHUI_SEG_B | SYNTHUI_SEG_C;
    case '8': return SYNTHUI_SEG_A | SYNTHUI_SEG_B | SYNTHUI_SEG_C | SYNTHUI_SEG_D | SYNTHUI_SEG_E | SYNTHUI_SEG_F | SYNTHUI_SEG_G;
    case '9': return SYNTHUI_SEG_A | SYNTHUI_SEG_B | SYNTHUI_SEG_C | SYNTHUI_SEG_D | SYNTHUI_SEG_F | SYNTHUI_SEG_G;
    case '-': return SYNTHUI_SEG_G;
    case '_': return SYNTHUI_SEG_D;
    case ' ': return 0;
    case '.': return SYNTHUI_SEG_DOT;
    case ':': return SYNTHUI_SEG_COLON;
    case 'A': case 'a': return SYNTHUI_SEG_A | SYNTHUI_SEG_B | SYNTHUI_SEG_C | SYNTHUI_SEG_E | SYNTHUI_SEG_F | SYNTHUI_SEG_G;
    case 'B': case 'b': return SYNTHUI_SEG_C | SYNTHUI_SEG_D | SYNTHUI_SEG_E | SYNTHUI_SEG_F | SYNTHUI_SEG_G;
    case 'C':           return SYNTHUI_SEG_A | SYNTHUI_SEG_D | SYNTHUI_SEG_E | SYNTHUI_SEG_F;
    case 'c':           return SYNTHUI_SEG_D | SYNTHUI_SEG_E | SYNTHUI_SEG_G;
    case 'D': case 'd': return SYNTHUI_SEG_B | SYNTHUI_SEG_C | SYNTHUI_SEG_D | SYNTHUI_SEG_E | SYNTHUI_SEG_G;
    case 'E': case 'e': return SYNTHUI_SEG_A | SYNTHUI_SEG_D | SYNTHUI_SEG_E | SYNTHUI_SEG_F | SYNTHUI_SEG_G;
    case 'F': case 'f': return SYNTHUI_SEG_A | SYNTHUI_SEG_E | SYNTHUI_SEG_F | SYNTHUI_SEG_G;
    case 'P': case 'p': return SYNTHUI_SEG_A | SYNTHUI_SEG_B | SYNTHUI_SEG_E | SYNTHUI_SEG_F | SYNTHUI_SEG_G;
    case 'L': case 'l': return SYNTHUI_SEG_D | SYNTHUI_SEG_E | SYNTHUI_SEG_F;
    case 'O': case 'o': return SYNTHUI_SEG_C | SYNTHUI_SEG_D | SYNTHUI_SEG_E | SYNTHUI_SEG_G;
    case 'R': case 'r': return SYNTHUI_SEG_E | SYNTHUI_SEG_G;
    case 'N': case 'n': return SYNTHUI_SEG_C | SYNTHUI_SEG_E | SYNTHUI_SEG_G;
    case 'U':           return SYNTHUI_SEG_B | SYNTHUI_SEG_C | SYNTHUI_SEG_D | SYNTHUI_SEG_E | SYNTHUI_SEG_F;
    case 'u':           return SYNTHUI_SEG_C | SYNTHUI_SEG_D | SYNTHUI_SEG_E;
    case 'H': case 'h': return SYNTHUI_SEG_B | SYNTHUI_SEG_C | SYNTHUI_SEG_E | SYNTHUI_SEG_F | SYNTHUI_SEG_G;
    case 'T': case 't': return SYNTHUI_SEG_D | SYNTHUI_SEG_E | SYNTHUI_SEG_F | SYNTHUI_SEG_G;
    case 'S': case 's': return SYNTHUI_SEG_A | SYNTHUI_SEG_F | SYNTHUI_SEG_G | SYNTHUI_SEG_C | SYNTHUI_SEG_D;
    case 'Q': case 'q': return SYNTHUI_SEG_A | SYNTHUI_SEG_B | SYNTHUI_SEG_C | SYNTHUI_SEG_F | SYNTHUI_SEG_G;
    case 'J': case 'j': return SYNTHUI_SEG_B | SYNTHUI_SEG_C | SYNTHUI_SEG_D;
    case 'Y': case 'y': return SYNTHUI_SEG_B | SYNTHUI_SEG_C | SYNTHUI_SEG_D | SYNTHUI_SEG_F | SYNTHUI_SEG_G;
    default:  return 0;
    }
}

static inline bool synthui_seven_segment_compute_layout(const char *text, float h, float slant_deg,
                                                        synthui_seven_segment_geom_t *g)
{
    if (!text || h <= 0.0f) return false;

    g->h = h;
    g->u = h / 112.0f;
    g->slant_deg = slant_deg;
    const float rad = slant_deg * (float)M_PI / 180.0f;
    g->shear = tanf(rad);
    g->overhang = 112.0f * g->shear;

    int count = 0;
    float cur_x = 0.0f;
    for (size_t i = 0; text[i] != '\0' && count < SYNTHUI_SEVEN_SEGMENT_MAX_CHARS; i++) {
        char ch = text[i];
        float w = (ch == '.' || ch == ':') ? 44.0f : 76.0f;
        g->cells[count].ch = ch;
        g->cells[count].x = cur_x;
        g->cells[count].w = w;
        cur_x += w;
        count++;
    }
    g->num_cells = count;
    g->total_view_w = cur_x + g->overhang;
    return true;
}

static inline void synthui_seven_segment_get_segment_geom(const synthui_seven_segment_cell_geom_t *cell,
                                                          int seg_idx, float shear,
                                                          synthui_seven_segment_poly_geom_t *poly)
{
    poly->num_verts = 6;
    poly->num_triangles = 4;

    const float t = 6.0f; /* T / 2 = 12 / 2 */
    float base_x = 0.0f, base_y = 0.0f, L = 0.0f;
    bool is_vert = false;

    /* Viewbox coordinates from SevenSegment.dc.html */
    switch (seg_idx) {
    case 0: /* a: h(15, 12, 34) */ base_x = 15.0f; base_y = 12.0f; L = 34.0f; is_vert = false; break;
    case 1: /* b: v(58, 15, 38) */ base_x = 58.0f; base_y = 15.0f; L = 38.0f; is_vert = true;  break;
    case 2: /* c: v(58, 59, 38) */ base_x = 58.0f; base_y = 59.0f; L = 38.0f; is_vert = true;  break;
    case 3: /* d: h(15, 100, 34)*/ base_x = 15.0f; base_y = 100.0f; L = 34.0f; is_vert = false; break;
    case 4: /* e: v(12, 59, 38) */ base_x = 12.0f; base_y = 59.0f; L = 38.0f; is_vert = true;  break;
    case 5: /* f: v(12, 15, 38) */ base_x = 12.0f; base_y = 15.0f; L = 38.0f; is_vert = true;  break;
    case 6: /* g: h(15, 56, 34) */ base_x = 15.0f; base_y = 56.0f; L = 34.0f; is_vert = false; break;
    default: return;
    }

    synthui_seven_segment_point_t raw[6];
    if (!is_vert) {
        /* h(x, y, L) */
        raw[0] = (synthui_seven_segment_point_t){ base_x, base_y };
        raw[1] = (synthui_seven_segment_point_t){ base_x + t, base_y - t };
        raw[2] = (synthui_seven_segment_point_t){ base_x + L - t, base_y - t };
        raw[3] = (synthui_seven_segment_point_t){ base_x + L, base_y };
        raw[4] = (synthui_seven_segment_point_t){ base_x + L - t, base_y + t };
        raw[5] = (synthui_seven_segment_point_t){ base_x + t, base_y + t };
    } else {
        /* v(x, y, L) */
        raw[0] = (synthui_seven_segment_point_t){ base_x, base_y };
        raw[1] = (synthui_seven_segment_point_t){ base_x + t, base_y + t };
        raw[2] = (synthui_seven_segment_point_t){ base_x + t, base_y + L - t };
        raw[3] = (synthui_seven_segment_point_t){ base_x, base_y + L };
        raw[4] = (synthui_seven_segment_point_t){ base_x - t, base_y + L - t };
        raw[5] = (synthui_seven_segment_point_t){ base_x - t, base_y + t };
    }

    /* Apply cell offset and bottom-aligned shear */
    for (int i = 0; i < 6; i++) {
        float x_local = cell->x + raw[i].x;
        float y_local = raw[i].y;
        poly->verts[i].x = x_local + (112.0f - y_local) * shear;
        poly->verts[i].y = y_local;
    }

    /* Decompose 6-gon into 4 triangles:
     * Quad 1: verts 0, 1, 4, 5 -> (0, 1, 5) and (1, 4, 5)
     * Quad 2: verts 1, 2, 3, 4 -> (1, 2, 4) and (2, 3, 4) */
    poly->triangles[0] = (synthui_seven_segment_triangle_t){ { poly->verts[0], poly->verts[1], poly->verts[5] } };
    poly->triangles[1] = (synthui_seven_segment_triangle_t){ { poly->verts[1], poly->verts[4], poly->verts[5] } };
    poly->triangles[2] = (synthui_seven_segment_triangle_t){ { poly->verts[1], poly->verts[2], poly->verts[4] } };
    poly->triangles[3] = (synthui_seven_segment_triangle_t){ { poly->verts[2], poly->verts[3], poly->verts[4] } };
}

static inline void synthui_seven_segment_get_punct_geom(const synthui_seven_segment_cell_geom_t *cell,
                                                        char ch, synthui_seven_segment_punct_geom_t *pg)
{
    const float center_x = cell->w * 0.5f; /* 22.0f for width 44 */

    if (ch == ':') {
        pg->is_colon = true;
        pg->num_circles = 2;
        pg->circles[0] = (synthui_seven_segment_circle_t){ center_x, 38.0f, 6.0f };
        pg->circles[1] = (synthui_seven_segment_circle_t){ center_x, 76.0f, 6.0f };
    } else {
        pg->is_colon = false;
        pg->num_circles = 1;
        pg->circles[0] = (synthui_seven_segment_circle_t){ center_x, 100.0f, 7.0f };
    }
}

#ifdef __cplusplus
}
#endif
#endif /* SYNTHUI_SEVEN_SEGMENT_MATH_H */
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd /Users/moolet/Development/SynthUI && ./tests/run.sh`
Expected: PASS (`seven_segment_test: all PASS`).

- [ ] **Step 5: Commit in SynthUI**

```bash
cd /Users/moolet/Development/SynthUI
git add src/synthui_seven_segment_types.h src/synthui_seven_segment_math.h tests/seven_segment_test.c tests/run.sh
git commit -m "synthui_seven_segment: geometry math, types, and host unit tests (NEW-29)"
```

---

### Task 2: `synthui_seven_segment.h` and `synthui_seven_segment.cpp` (SynthUI repo)

**Files:**
- Create: `/Users/moolet/Development/SynthUI/src/synthui_seven_segment.h`
- Create: `/Users/moolet/Development/SynthUI/src/synthui_seven_segment.cpp`

**Interfaces:**
- Produces: `synthui_seven_segment_class`, `synthui_seven_segment_create()`, `synthui_seven_segment_set_text()`, `synthui_seven_segment_get_text()`, `synthui_seven_segment_set_accent()`, `synthui_seven_segment_get_accent_on()`, `synthui_seven_segment_get_accent_glow()`, `synthui_seven_segment_set_ghost()`, `synthui_seven_segment_get_ghost()`, `synthui_seven_segment_set_slant()`, `synthui_seven_segment_get_slant()`

- [ ] **Step 1: Create `src/synthui_seven_segment.h`**

Create `/Users/moolet/Development/SynthUI/src/synthui_seven_segment.h`:
```c
/* synthui_seven_segment.h - SynthUI SevenSegment, LVGL 9 custom widget.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_SEVEN_SEGMENT_H
#define SYNTHUI_SEVEN_SEGMENT_H

#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>
#include "synthui_seven_segment_types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_obj_class_t synthui_seven_segment_class;

lv_obj_t *synthui_seven_segment_create(lv_obj_t *parent);

/* Text display (clamped to SYNTHUI_SEVEN_SEGMENT_MAX_CHARS) */
void synthui_seven_segment_set_text(lv_obj_t *obj, const char *text);
const char *synthui_seven_segment_get_text(const lv_obj_t *obj);

/* Accent styling */
void synthui_seven_segment_set_accent(lv_obj_t *obj, uint32_t on_hex, uint32_t glow_hex);
uint32_t synthui_seven_segment_get_accent_on(const lv_obj_t *obj);
uint32_t synthui_seven_segment_get_accent_glow(const lv_obj_t *obj);

/* Ghost unlit segments (default true) */
void synthui_seven_segment_set_ghost(lv_obj_t *obj, bool ghost);
bool synthui_seven_segment_get_ghost(const lv_obj_t *obj);

/* Slant angle in degrees (default 6.0 deg, range 0..14) */
void synthui_seven_segment_set_slant(lv_obj_t *obj, float slant_deg);
float synthui_seven_segment_get_slant(const lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
#endif /* SYNTHUI_SEVEN_SEGMENT_H */
```

- [ ] **Step 2: Create `src/synthui_seven_segment.cpp`**

Create `/Users/moolet/Development/SynthUI/src/synthui_seven_segment.cpp`:
```cpp
/* synthui_seven_segment.cpp - SynthUI SevenSegment, LVGL 9 custom widget.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#include "synthui_seven_segment.h"
#include "synthui_seven_segment_math.h"
#include <lvgl_private.h>
#include <math.h>
#include <string.h>

#define MY_CLASS (&synthui_seven_segment_class)

typedef struct {
    lv_obj_t obj;
    char text[SYNTHUI_SEVEN_SEGMENT_MAX_CHARS + 1];
    uint32_t on_color;
    uint32_t glow_color;
    float slant;
    bool ghost;
} synthui_seven_segment_t;

static void seg_constructor(const lv_obj_class_t *cls, lv_obj_t *obj);
static void seg_destructor(const lv_obj_class_t *cls, lv_obj_t *obj);
static void seg_event(const lv_obj_class_t *cls, lv_event_t *e);
static void seg_draw(synthui_seven_segment_t *seg, lv_layer_t *layer);

const lv_obj_class_t synthui_seven_segment_class = {
    .base_class     = &lv_obj_class,
    .constructor_cb = seg_constructor,
    .destructor_cb  = seg_destructor,
    .event_cb       = seg_event,
    .name           = "synthui_seven_segment",
    .width_def      = 180,
    .height_def     = 56,
    .instance_size  = sizeof(synthui_seven_segment_t),
};

lv_obj_t *synthui_seven_segment_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_class_create_obj(&synthui_seven_segment_class, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

static void seg_constructor(const lv_obj_class_t *cls, lv_obj_t *obj)
{
    LV_UNUSED(cls);
    synthui_seven_segment_t *seg = (synthui_seven_segment_t *)obj;
    strncpy(seg->text, "888", SYNTHUI_SEVEN_SEGMENT_MAX_CHARS);
    seg->text[SYNTHUI_SEVEN_SEGMENT_MAX_CHARS] = '\0';
    seg->on_color = SYNTHUI_SEVEN_SEGMENT_COLOR_DEFAULT_ON;
    seg->glow_color = SYNTHUI_SEVEN_SEGMENT_COLOR_DEFAULT_GLOW;
    seg->slant = 6.0f;
    seg->ghost = true;
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void seg_destructor(const lv_obj_class_t *cls, lv_obj_t *obj)
{
    LV_UNUSED(cls);
    LV_UNUSED(obj);
}

static void seg_event(const lv_obj_class_t *cls, lv_event_t *e)
{
    LV_UNUSED(cls);
    if (lv_obj_event_base(MY_CLASS, e) != LV_RESULT_OK) return;
    if (lv_event_get_code(e) == LV_EVENT_DRAW_MAIN) {
        seg_draw((synthui_seven_segment_t *)lv_event_get_current_target_obj(e),
                 lv_event_get_layer(e));
    }
}

static void seg_draw(synthui_seven_segment_t *seg, lv_layer_t *layer)
{
    lv_area_t a;
    lv_obj_get_coords((lv_obj_t *)seg, &a);
    const int32_t w = lv_area_get_width(&a);
    const int32_t h = lv_area_get_height(&a);
    if (w <= 0 || h <= 0) return;

    synthui_seven_segment_geom_t g;
    if (!synthui_seven_segment_compute_layout(seg->text, (float)h, seg->slant, &g)) return;

    const lv_state_t st = lv_obj_get_state((const lv_obj_t *)seg);
    const bool disabled = (st & LV_STATE_DISABLED) != 0;

    const lv_color_t on_color = lv_color_hex(seg->on_color);
    const lv_color_t glow_color = lv_color_hex(seg->glow_color);

    const lv_opa_t lit_core_opa = disabled ? LV_OPA_50 : LV_OPA_COVER;
    const lv_opa_t lit_glow_opa = disabled ? 36 : 71;
    const lv_opa_t ghost_opa = disabled ? 14 : 28;
    const lv_opa_t punct_glow_opa = disabled ? 38 : 76;

    const int32_t glow_stroke_w = (int32_t)lroundf(7.0f * g.u);

    /* Render each cell */
    for (int ci = 0; ci < g.num_cells; ci++) {
        const synthui_seven_segment_cell_geom_t *cell = &g.cells[ci];

        /* Screen bounds of this cell */
        lv_area_t cell_area;
        cell_area.x1 = a.x1 + (int32_t)floorf(cell->x * g.u);
        cell_area.y1 = a.y1;
        cell_area.x2 = a.x1 + (int32_t)ceilf((cell->x + cell->w + g.overhang) * g.u);
        cell_area.y2 = a.y2;

        /* Clip check: skip if cell is outside layer clip area */
        lv_area_t intersect;
        if (!_lv_area_intersect(&intersect, &cell_area, &layer->_clip_area)) {
            continue;
        }

        /* 1. Cell background: #181830 well */
        lv_draw_rect_dsc_t bg_dsc;
        lv_draw_rect_dsc_init(&bg_dsc);
        bg_dsc.bg_color = lv_color_hex(0x181830);
        bg_dsc.bg_opa = LV_OPA_COVER;
        bg_dsc.radius = 0;
        lv_draw_rect(layer, &bg_dsc, &intersect);

        const uint16_t mask = synthui_seven_segment_get_char_mask(cell->ch);

        /* Handle punctuation ('.' and ':') */
        if (cell->ch == '.' || cell->ch == ':') {
            synthui_seven_segment_punct_geom_t pg;
            synthui_seven_segment_get_punct_geom(cell, cell->ch, &pg);

            for (int pi = 0; pi < pg.num_circles; pi++) {
                const float sheared_cx = cell->x + pg.circles[pi].cx + (112.0f - pg.circles[pi].cy) * g.shear;
                const int32_t cx_px = a.x1 + (int32_t)lroundf(sheared_cx * g.u);
                const int32_t cy_px = a.y1 + (int32_t)lroundf(pg.circles[pi].cy * g.u);
                const int32_t r_px = (int32_t)lroundf(pg.circles[pi].r * g.u);

                /* Glow circle */
                if (glow_stroke_w > 0) {
                    const int32_t gr_px = r_px + (glow_stroke_w / 2);
                    lv_area_t glow_circ = { cx_px - gr_px, cy_px - gr_px, cx_px + gr_px, cy_px + gr_px };
                    lv_draw_rect_dsc_t gdsc;
                    lv_draw_rect_dsc_init(&gdsc);
                    gdsc.bg_color = glow_color;
                    gdsc.bg_opa = punct_glow_opa;
                    gdsc.radius = LV_RADIUS_CIRCLE;
                    lv_draw_rect(layer, &gdsc, &glow_circ);
                }

                /* Core circle */
                lv_area_t core_circ = { cx_px - r_px, cy_px - r_px, cx_px + r_px, cy_px + r_px };
                lv_draw_rect_dsc_t cdsc;
                lv_draw_rect_dsc_init(&cdsc);
                cdsc.bg_color = on_color;
                cdsc.bg_opa = lit_core_opa;
                cdsc.radius = LV_RADIUS_CIRCLE;
                lv_draw_rect(layer, &cdsc, &core_circ);
            }
            continue;
        }

        /* 2. Ghost unlit segments (if enabled) */
        if (seg->ghost) {
            for (int s = 0; s < 7; s++) {
                if ((mask & (1u << s)) != 0) continue; /* skip lit segments */

                synthui_seven_segment_poly_geom_t poly;
                synthui_seven_segment_get_segment_geom(cell, s, g.shear, &poly);

                lv_draw_triangle_dsc_t tdsc;
                lv_draw_triangle_dsc_init(&tdsc);
                tdsc.bg_color = glow_color;
                tdsc.bg_opa = ghost_opa;

                for (int ti = 0; ti < 4; ti++) {
                    tdsc.p[0].x = a.x1 + (int32_t)lroundf(poly.triangles[ti].p[0].x * g.u);
                    tdsc.p[0].y = a.y1 + (int32_t)lroundf(poly.triangles[ti].p[0].y * g.u);
                    tdsc.p[1].x = a.x1 + (int32_t)lroundf(poly.triangles[ti].p[1].x * g.u);
                    tdsc.p[1].y = a.y1 + (int32_t)lroundf(poly.triangles[ti].p[1].y * g.u);
                    tdsc.p[2].x = a.x1 + (int32_t)lroundf(poly.triangles[ti].p[2].x * g.u);
                    tdsc.p[2].y = a.y1 + (int32_t)lroundf(poly.triangles[ti].p[2].y * g.u);
                    lv_draw_triangle(layer, &tdsc);
                }
            }
        }

        /* 3. Lit segments: glow stroke pass */
        if (glow_stroke_w > 0) {
            for (int s = 0; s < 7; s++) {
                if ((mask & (1u << s)) == 0) continue;

                synthui_seven_segment_poly_geom_t poly;
                synthui_seven_segment_get_segment_geom(cell, s, g.shear, &poly);

                for (int vi = 0; vi < 6; vi++) {
                    int next = (vi + 1) % 6;
                    lv_draw_line_dsc_t ldsc;
                    lv_draw_line_dsc_init(&ldsc);
                    ldsc.color = glow_color;
                    ldsc.opa = lit_glow_opa;
                    ldsc.width = glow_stroke_w;
                    ldsc.round_start = 1;
                    ldsc.round_end = 1;
                    ldsc.p1.x = a.x1 + (int32_t)lroundf(poly.verts[vi].x * g.u);
                    ldsc.p1.y = a.y1 + (int32_t)lroundf(poly.verts[vi].y * g.u);
                    ldsc.p2.x = a.x1 + (int32_t)lroundf(poly.verts[next].x * g.u);
                    ldsc.p2.y = a.y1 + (int32_t)lroundf(poly.verts[next].y * g.u);
                    lv_draw_line(layer, &ldsc);
                }
            }
        }

        /* 4. Lit segments: core pass */
        for (int s = 0; s < 7; s++) {
            if ((mask & (1u << s)) == 0) continue;

            synthui_seven_segment_poly_geom_t poly;
            synthui_seven_segment_get_segment_geom(cell, s, g.shear, &poly);

            lv_draw_triangle_dsc_t tdsc;
            lv_draw_triangle_dsc_init(&tdsc);
            tdsc.bg_color = on_color;
            tdsc.bg_opa = lit_core_opa;

            for (int ti = 0; ti < 4; ti++) {
                tdsc.p[0].x = a.x1 + (int32_t)lroundf(poly.triangles[ti].p[0].x * g.u);
                tdsc.p[0].y = a.y1 + (int32_t)lroundf(poly.triangles[ti].p[0].y * g.u);
                tdsc.p[1].x = a.x1 + (int32_t)lroundf(poly.triangles[ti].p[1].x * g.u);
                tdsc.p[1].y = a.y1 + (int32_t)lroundf(poly.triangles[ti].p[1].y * g.u);
                tdsc.p[2].x = a.x1 + (int32_t)lroundf(poly.triangles[ti].p[2].x * g.u);
                tdsc.p[2].y = a.y1 + (int32_t)lroundf(poly.triangles[ti].p[2].y * g.u);
                lv_draw_triangle(layer, &tdsc);
            }
        }
    }
}

void synthui_seven_segment_set_text(lv_obj_t *obj, const char *text)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    if (!text) return;
    synthui_seven_segment_t *seg = (synthui_seven_segment_t *)obj;

    if (strncmp(seg->text, text, SYNTHUI_SEVEN_SEGMENT_MAX_CHARS) == 0) return;

    const size_t prev_len = strlen(seg->text);
    const size_t new_len = strlen(text);

    if (prev_len != new_len) {
        /* Layout width changed -> full invalidation */
        strncpy(seg->text, text, SYNTHUI_SEVEN_SEGMENT_MAX_CHARS);
        seg->text[SYNTHUI_SEVEN_SEGMENT_MAX_CHARS] = '\0';
        lv_obj_invalidate(obj);
        return;
    }

    /* Same length: perform cell-level delta invalidation */
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const int32_t h = lv_area_get_height(&a);

    synthui_seven_segment_geom_t g;
    synthui_seven_segment_compute_layout(seg->text, (float)h, seg->slant, &g);

    const int32_t margin = (int32_t)ceilf(8.0f * g.u);

    for (size_t i = 0; i < prev_len && (int)i < g.num_cells; i++) {
        if (seg->text[i] != text[i]) {
            const synthui_seven_segment_cell_geom_t *cell = &g.cells[i];
            lv_area_t dirty;
            dirty.x1 = a.x1 + (int32_t)floorf(cell->x * g.u) - margin;
            dirty.y1 = a.y1;
            dirty.x2 = a.x1 + (int32_t)ceilf((cell->x + cell->w + g.overhang) * g.u) + margin;
            dirty.y2 = a.y2;
            lv_obj_invalidate_area(obj, &dirty);
        }
    }

    strncpy(seg->text, text, SYNTHUI_SEVEN_SEGMENT_MAX_CHARS);
    seg->text[SYNTHUI_SEVEN_SEGMENT_MAX_CHARS] = '\0';
}

const char *synthui_seven_segment_get_text(const lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    const synthui_seven_segment_t *seg = (const synthui_seven_segment_t *)obj;
    return seg->text;
}

void synthui_seven_segment_set_accent(lv_obj_t *obj, uint32_t on_hex, uint32_t glow_hex)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    synthui_seven_segment_t *seg = (synthui_seven_segment_t *)obj;
    if (seg->on_color == on_hex && seg->glow_color == glow_hex) return;
    seg->on_color = on_hex;
    seg->glow_color = glow_hex;
    lv_obj_invalidate(obj);
}

uint32_t synthui_seven_segment_get_accent_on(const lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    const synthui_seven_segment_t *seg = (const synthui_seven_segment_t *)obj;
    return seg->on_color;
}

uint32_t synthui_seven_segment_get_accent_glow(const lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    const synthui_seven_segment_t *seg = (const synthui_seven_segment_t *)obj;
    return seg->glow_color;
}

void synthui_seven_segment_set_ghost(lv_obj_t *obj, bool ghost)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    synthui_seven_segment_t *seg = (synthui_seven_segment_t *)obj;
    if (seg->ghost == ghost) return;
    seg->ghost = ghost;
    lv_obj_invalidate(obj);
}

bool synthui_seven_segment_get_ghost(const lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    const synthui_seven_segment_t *seg = (const synthui_seven_segment_t *)obj;
    return seg->ghost;
}

void synthui_seven_segment_set_slant(lv_obj_t *obj, float slant_deg)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    synthui_seven_segment_t *seg = (synthui_seven_segment_t *)obj;
    if (fabsf(seg->slant - slant_deg) < 0.01f) return;
    seg->slant = slant_deg;
    lv_obj_invalidate(obj);
}

float synthui_seven_segment_get_slant(const lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    const synthui_seven_segment_t *seg = (const synthui_seven_segment_t *)obj;
    return seg->slant;
}
```

- [ ] **Step 3: Run host tests**

Run: `cd /Users/moolet/Development/SynthUI && ./tests/run.sh`
Expected: PASS (`seven_segment_test: all PASS`).

- [ ] **Step 4: Commit in SynthUI**

```bash
cd /Users/moolet/Development/SynthUI
git add src/synthui_seven_segment.h src/synthui_seven_segment.cpp
git commit -m "synthui_seven_segment: LVGL 9 custom widget implementation (NEW-29)"
```

---

### Task 3: `examples/display/synthui_seven_segment_test` Scaffolding & Scene (rt1176-evkb repo)

**Files:**
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_seven_segment_test/CMakeLists.txt`
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_seven_segment_test/synthui_seven_segment_test.cpp`

**Interfaces:**
- Consumes: `synthui_seven_segment.h`, `lvgl_mipi_panel.h`, `Display.h`
- Produces: `synthui_seven_segment_test.elf`

- [ ] **Step 1: Create `CMakeLists.txt`**

Create `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_seven_segment_test/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.24)
project(synthui_seven_segment_test)

add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
import_evkb_synthui()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(synthui_seven_segment_test
    synthui_seven_segment_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp
)
teensy_target_link_libraries(synthui_seven_segment_test cores MipiDisplay PXP)

target_link_libraries(synthui_seven_segment_test.elf SynthUI LVGL stdc++)

target_include_directories(synthui_seven_segment_test.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(SEVEN_SEGMENT_EYEBALL_HOLD)
    add_compile_definitions(SEVEN_SEGMENT_EYEBALL_HOLD=${SEVEN_SEGMENT_EYEBALL_HOLD})
endif()
```

- [ ] **Step 2: Create `synthui_seven_segment_test.cpp`**

Create `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_seven_segment_test/synthui_seven_segment_test.cpp`:
```cpp
/* synthui_seven_segment_test - synthui_seven_segment (NEW-29) on the RK055
 * display panel under QEMU and on silicon.
 *
 * Pipeline: double-buffered hardware pipeline via lvgl_mipi_panel_create_db(Display).
 * Verifies:
 *   1. Full initial render of 6-readout scene -> pinned golden CRC (FNV-1a over 720x1280 fb).
 *   2. 64-step deterministic delta sequence (counter ticks).
 *   3. Delta equality guard: delta-rendered CRC matches fresh full-render CRC.
 *   4. Damage engagement guard: max invalidated area stays small.
 *   5. VSYNC health guard: timeouts=0.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */

#include <Arduino.h>
#include <Display.h>
#include <lvgl.h>
#include "port/lvgl_mipi_panel.h"
#include "synthui_seven_segment.h"
#include <stdio.h>
#include <string.h>

#define RK055_WIDTH  720
#define RK055_HEIGHT 1280

static uint32_t fnv1a_32(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811c9dc5u;
    while (n--) {
        h ^= *p++;
        h *= 0x01000193u;
    }
    return h;
}

static uint32_t g_max_damage = 0;
static uint32_t g_total_damage = 0;

static void damage_monitor_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    if (area) {
        uint32_t w = (uint32_t)lv_area_get_width(area);
        uint32_t h = (uint32_t)lv_area_get_height(area);
        uint32_t dmg = w * h;
        if (dmg > g_max_damage) g_max_damage = dmg;
        g_total_damage += dmg;
    }
}

static lv_obj_t *g_readouts[6];

void setup()
{
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0 < 3000)) {}

    Serial.println("SYNTHUI_SEVEN_SEGMENT_BEGIN");

    Display.begin();
    if (!Display.is_ok()) {
        Serial.println("PANEL_FAIL");
        return;
    }
    Serial.println("PANEL_OK");

    lv_init();
    lv_display_t *disp = lvgl_mipi_panel_create_db(Display);
    if (!disp) {
        Serial.println("LVGL_DISP_FAIL");
        return;
    }
    lv_display_add_event_cb(disp, damage_monitor_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101020), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Scene: 6 readouts on dark synth panel */
    /* 1. Main BPM Readout: "140.0" at y=80, h=96, Blue accent */
    g_readouts[0] = synthui_seven_segment_create(scr);
    lv_obj_set_pos(g_readouts[0], 60, 80);
    lv_obj_set_size(g_readouts[0], 480, 96);
    synthui_seven_segment_set_text(g_readouts[0], "140.0");
    synthui_seven_segment_set_accent(g_readouts[0], SYNTHUI_SEVEN_SEGMENT_COLOR_BLUE_ON, SYNTHUI_SEVEN_SEGMENT_COLOR_BLUE_GLOW);

    /* 2. Bar:Beat Readout: "01:04" at y=230, h=56, Cyan accent */
    g_readouts[1] = synthui_seven_segment_create(scr);
    lv_obj_set_pos(g_readouts[1], 60, 230);
    lv_obj_set_size(g_readouts[1], 320, 56);
    synthui_seven_segment_set_text(g_readouts[1], "01:04");
    synthui_seven_segment_set_accent(g_readouts[1], SYNTHUI_SEVEN_SEGMENT_COLOR_CYAN_ON, SYNTHUI_SEVEN_SEGMENT_COLOR_CYAN_GLOW);

    /* 3. Pattern Readout: "P-A3" at y=340, h=56, Amber accent */
    g_readouts[2] = synthui_seven_segment_create(scr);
    lv_obj_set_pos(g_readouts[2], 60, 340);
    lv_obj_set_size(g_readouts[2], 300, 56);
    synthui_seven_segment_set_text(g_readouts[2], "P-A3");
    synthui_seven_segment_set_accent(g_readouts[2], SYNTHUI_SEVEN_SEGMENT_COLOR_AMBER_ON, SYNTHUI_SEVEN_SEGMENT_COLOR_AMBER_GLOW);

    /* 4. Track Status Readout: "REC.8" at y=450, h=56, Red accent */
    g_readouts[3] = synthui_seven_segment_create(scr);
    lv_obj_set_pos(g_readouts[3], 60, 450);
    lv_obj_set_size(g_readouts[3], 320, 56);
    synthui_seven_segment_set_text(g_readouts[3], "REC.8");
    synthui_seven_segment_set_accent(g_readouts[3], SYNTHUI_SEVEN_SEGMENT_COLOR_RED_ON, SYNTHUI_SEVEN_SEGMENT_COLOR_RED_GLOW);

    /* 5. Ghost-off Readout: "8888" at y=560, h=44, Cyan accent, ghost=false */
    g_readouts[4] = synthui_seven_segment_create(scr);
    lv_obj_set_pos(g_readouts[4], 60, 560);
    lv_obj_set_size(g_readouts[4], 240, 44);
    synthui_seven_segment_set_text(g_readouts[4], "8888");
    synthui_seven_segment_set_accent(g_readouts[4], SYNTHUI_SEVEN_SEGMENT_COLOR_CYAN_ON, SYNTHUI_SEVEN_SEGMENT_COLOR_CYAN_GLOW);
    synthui_seven_segment_set_ghost(g_readouts[4], false);

    /* 6. Disabled Readout: "OFF" at y=660, h=44, Red accent, LV_STATE_DISABLED */
    g_readouts[5] = synthui_seven_segment_create(scr);
    lv_obj_set_pos(g_readouts[5], 60, 660);
    lv_obj_set_size(g_readouts[5], 200, 44);
    synthui_seven_segment_set_text(g_readouts[5], "OFF");
    synthui_seven_segment_set_accent(g_readouts[5], SYNTHUI_SEVEN_SEGMENT_COLOR_RED_ON, SYNTHUI_SEVEN_SEGMENT_COLOR_RED_GLOW);
    lv_obj_add_state(g_readouts[5], LV_STATE_DISABLED);

    Serial.println("seven_segment_scene=6 count=6");

    /* Initial full render */
    lv_refr_now(disp);
    lvgl_mipi_panel_flip_sync();
    Serial.println("LVGL_FLUSHED=PASS");
    Serial.printf("LVGL_BYTES=%u\n", (unsigned)(RK055_WIDTH * RK055_HEIGHT * 4));

    const void *fb0 = lvgl_mipi_panel_scanned_fb();
    uint32_t init_crc = fnv1a_32(fb0, (size_t)RK055_WIDTH * RK055_HEIGHT * 4);
    Serial.printf("seven_segment_crc=0x%08X\n", (unsigned)init_crc);

#ifdef SEVEN_SEGMENT_EYEBALL_HOLD
    Serial.println("SEVEN_SEGMENT_EYEBALL_HOLD active; pausing loop");
    for (;;) { delay(1000); }
#endif

    /* Phase A2: 64-step deterministic delta sequence */
    g_max_damage = 0;
    g_total_damage = 0;

    char bpm_buf[16];
    char bar_buf[16];

    for (int step = 1; step <= 64; step++) {
        /* Increment BPM counter: 140.0 -> 140.1 -> ... */
        int frac = step % 10;
        int whole = 140 + (step / 10);
        snprintf(bpm_buf, sizeof(bpm_buf), "%d.%d", whole, frac);
        synthui_seven_segment_set_text(g_readouts[0], bpm_buf);

        /* Bar:beat progression */
        int beat = (step % 4) + 1;
        int bar = (step / 4) + 1;
        snprintf(bar_buf, sizeof(bar_buf), "%02d:%02d", bar, beat);
        synthui_seven_segment_set_text(g_readouts[1], bar_buf);

        lv_refr_now(disp);
        lvgl_mipi_panel_flip_sync();
    }

    const void *delta_fb = lvgl_mipi_panel_scanned_fb();
    uint32_t delta_crc = fnv1a_32(delta_fb, (size_t)RK055_WIDTH * RK055_HEIGHT * 4);
    Serial.printf("seven_segment_delta_crc=0x%08X\n", (unsigned)delta_crc);

    /* Delta-equality check: force full redraw of final state */
    lv_obj_invalidate(scr);
    lv_refr_now(disp);
    lvgl_mipi_panel_flip_sync();

    const void *fresh_fb = lvgl_mipi_panel_scanned_fb();
    uint32_t fresh_crc = fnv1a_32(fresh_fb, (size_t)RK055_WIDTH * RK055_HEIGHT * 4);
    Serial.printf("seven_segment_fresh_crc=0x%08X\n", (unsigned)fresh_crc);

    if (delta_crc == fresh_crc) {
        Serial.println("seven_segment_delta_eq=PASS");
    } else {
        Serial.println("seven_segment_delta_eq=FAIL");
    }

    Serial.printf("seven_segment_damage max=%u total=%u\n", (unsigned)g_max_damage, (unsigned)g_total_damage);

    lvgl_mipi_vsync_stats_t vs;
    lvgl_mipi_panel_vsync_stats(&vs);
    Serial.printf("seven_segment_vsync flips=%u isrs=%u timeouts=%u\n",
                  (unsigned)vs.flips, (unsigned)vs.isrs, (unsigned)vs.timeouts);

    Serial.println("crc_done");
    Serial.println("PASS: SynthUI seven_segment render verified");
}

void loop()
{
    /* Phase B: continuous animation benchmark */
    static uint32_t frame_count = 0;
    static uint32_t last_report = 0;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (frame_count / 60) % 100, frame_count % 60);
    synthui_seven_segment_set_text(g_readouts[1], buf);

    lv_timer_handler();
    lvgl_mipi_panel_flip_sync();

    frame_count++;
    uint32_t now = millis();
    if (now - last_report >= 1000) {
        float fps = (frame_count * 1000.0f) / (float)(now - last_report);
        Serial.printf("seven_segment_fps=%.1f\n", fps);
        frame_count = 0;
        last_report = now;
    }
}
```

- [ ] **Step 3: Build the example**

```bash
mkdir -p /Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_seven_segment_test/build
cd /Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_seven_segment_test/build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../../../../cmake/toolchain-arm-none-eabi.cmake
cmake --build .
```
Expected: `synthui_seven_segment_test.elf` created successfully.

- [ ] **Step 4: Commit scaffold**

```bash
cd /Users/moolet/Development/rt1170/rt1176-evkb
git add examples/display/synthui_seven_segment_test/CMakeLists.txt examples/display/synthui_seven_segment_test/synthui_seven_segment_test.cpp
git commit -m "synthui_seven_segment_test: example scaffolding and 6-readout test scene (NEW-29)"
```

---

### Task 4: QEMU Gate Runner & Golden Verification (rt1176-evkb repo)

**Files:**
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_seven_segment_test/run_qemu.sh`
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_seven_segment_test/transcript_qemu.txt`

- [ ] **Step 1: Create `run_qemu.sh`**

Create `examples/display/synthui_seven_segment_test/run_qemu.sh`:
```sh
#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
export REAL_QEMU="${REAL_QEMU:-/Users/moolet/Development/qemu-rt1170/build/qemu-system-arm}"
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/synthui_seven_segment_test.elf"
OUT=$(gate_capture_path "$DIR" synthui_seven_segment.uart)
DBG=$(gate_capture_path "$DIR" synthui_seven_segment.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
sleep 20; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "PANEL_OK"          "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "seven_segment_scene=6 count=6" "$OUT" || { echo "FAIL: scene line missing"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
grep -qE "LVGL_BYTES=3686400\r?$" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }

# GOLDEN CHECKSUM -- FNV-1a over presented 720x1280 buffer
# Will be recorded from two bit-identical QEMU runs and pinned here:
grep -qE "seven_segment_crc=0x[0-9A-F]{8}\r?$" "$OUT" || { echo "FAIL: seven_segment checksum"; exit 1; }

# DELTA EQUALITY: 64-step sequence delta CRC must be PIXEL-IDENTICAL to fresh render
DSEQ=$(grep -a -oE "seven_segment_delta_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
DFUL=$(grep -a -oE "seven_segment_fresh_crc=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DSEQ" ] && [ -n "$DFUL" ] || { echo "FAIL: delta guard tokens missing"; exit 1; }
[ "$DSEQ" = "$DFUL" ] || { echo "FAIL: delta render differs from full render ($DSEQ vs $DFUL)"; exit 1; }
grep -qE "seven_segment_delta_eq=PASS\r?$" "$OUT" || { echo "FAIL: delta equality"; exit 1; }

# ENGAGEMENT: largest single invalidated area must stay small (bound 20000 px)
DAREA=$(grep -a -oE "seven_segment_damage max=[0-9]+" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DAREA" ] && [ "$DAREA" -gt 0 ] || { echo "FAIL: delta damage guard missing or zero"; exit 1; }
[ "$DAREA" -le 20000 ] || { echo "FAIL: delta damage not engaged (max=$DAREA)"; exit 1; }

# VSYNC health
grep -qE "seven_segment_vsync flips=[0-9]+ isrs=[0-9]+ timeouts=0\r?$" "$OUT" || { echo "FAIL: vsync fence unhealthy or missing"; exit 1; }
grep -q "crc_done" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
grep -q "PASS: SynthUI seven_segment render verified" "$OUT" || { echo "FAIL: render verification token missing"; exit 1; }
echo "PASS: SynthUI seven_segment render verified"
```
Make executable: `chmod +x examples/display/synthui_seven_segment_test/run_qemu.sh`

- [ ] **Step 2: Run in QEMU to record initial golden CRC**

Run QEMU across two consecutive boots, confirm bit-identical golden CRC, and pin it into `run_qemu.sh`.

- [ ] **Step 3: Demonstrate Tripwires RED**

Temporarily flip the expected CRC in `run_qemu.sh` to `0xDEADBEEF`, run `./run_qemu.sh`, and assert failure. Then restore true golden.

- [ ] **Step 4: Verify gate passes GREEN**

Run: `cd /Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_seven_segment_test && ./run_qemu.sh`
Expected: `PASS: SynthUI seven_segment render verified`. Save output to `transcript_qemu.txt`.

- [ ] **Step 5: Commit gate and test results**

```bash
cd /Users/moolet/Development/rt1170/rt1176-evkb
git add examples/display/synthui_seven_segment_test/run_qemu.sh examples/display/synthui_seven_segment_test/transcript_qemu.txt
git commit -m "synthui_seven_segment_test: QEMU gate verified green with golden CRC (NEW-29)"
```

---

### Task 5: Update SynthUI Pin in `evkb.cmake` (rt1176-evkb repo)

**Files:**
- Modify: `/Users/moolet/Development/rt1170/rt1176-evkb/evkb.cmake`

- [ ] **Step 1: Check SynthUI HEAD commit SHA**

Run: `git -C /Users/moolet/Development/SynthUI rev-parse HEAD`

- [ ] **Step 2: Bump pin in `evkb.cmake`**

Update the SynthUI declaration line in `/Users/moolet/Development/rt1170/rt1176-evkb/evkb.cmake` with the new commit SHA and comment referencing `synthui_seven_segment (NEW-29)`.

- [ ] **Step 3: Commit pin update**

```bash
cd /Users/moolet/Development/rt1170/rt1176-evkb
git add evkb.cmake
git commit -m "build: bump SynthUI pin to <SHA> (synthui_seven_segment, NEW-29)"
```
