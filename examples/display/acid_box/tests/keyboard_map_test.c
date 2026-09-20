/* keyboard_map_test.c -- host test for keyboard_map.h (spec 2026-09-20).
 * PASS:/FAIL: per check, count at the end (the tree's convention:
 * `grep -c "^PASS:"` on a live run is the case count). */
#include <stdio.h>
#include "../keyboard_map.h"

static int pass = 0, fail = 0;
static void check(const char *name, int got, int want)
{
    if (got == want) { printf("PASS: %s (%d)\n", name, got); pass++; }
    else { printf("FAIL: %s got %d want %d\n", name, got, want); fail++; }
}

/* LVGL's hit test, modelled: blacks are created after the whites, so they win. */
static int hit(int px, int py)
{
    for (int pass_ = 0; pass_ < 2; pass_++)
        for (int k = 0; k < KB_KEYS; k++) {
            if (kb_is_black(k) != (pass_ == 0)) continue;
            int x, y, w, h;
            kb_key_rect(k, &x, &y, &w, &h);
            if (px >= x && px < x + w && py >= y && py < y + h) return k;
        }
    return -1;
}

/* The GT911 model's integer landing for a touch_script "P a b" line, mapped
 * back to LOGICAL landscape coordinates (touch_script.txt's own header):
 * physical (720*a/100, 1280*b/100); logical x = physical y, y = 719 - physical x. */
static int land_x(int b) { return 1280 * b / 100; }
static int land_y(int a) { return 719 - 720 * a / 100; }

int main(void)
{
    /* view base */
    check("base view 1 = C1", kb_base(1), 24);
    check("base view 2 = C2", kb_base(2), 36);
    check("base view 3 = C3", kb_base(3), 48);

    /* note -> key, with the top-C boundary seen from both sides */
    check("A1 in view 1 is key 9",          kb_key_for_note(33, 1), 9);
    check("note36 in view 1 is the TOP C",  kb_key_for_note(36, 1), 12);
    check("note36 in view 2 is the BOTTOM C", kb_key_for_note(36, 2), 0);
    check("note23 below view 1 is off",     kb_key_for_note(23, 1), -1);
    check("note37 above view 1 is off",     kb_key_for_note(37, 1), -1);
    check("C4 in view 3 is the top C",      kb_key_for_note(60, 3), 12);
    check("note61 above view 3 is off",     kb_key_for_note(61, 3), -1);

    /* keep-or-snap */
    check("visible note keeps view 1",      kb_view_for_note(33, 1), 1);
    check("top C keeps view 1",             kb_view_for_note(36, 1), 1);
    check("bottom C keeps view 2",          kb_view_for_note(36, 2), 2);
    check("note37 snaps view 1 -> 2",       kb_view_for_note(37, 1), 2);
    check("C4 snaps view 1 -> 3 (clamped)", kb_view_for_note(60, 1), 3);
    check("C1 snaps view 3 -> 1",           kb_view_for_note(24, 3), 1);
    check("note12 clamps low to view 1",    kb_view_for_note(12, 2), 1);
    check("note127 clamps high to view 3",  kb_view_for_note(127, 1), 3);
    check("note0 clamps low to view 1",     kb_view_for_note(0, 3), 1);

    /* black / white */
    {
        const int want[KB_KEYS] = {0,1,0,1,0,0,1,0,1,0,1,0,0};
        int bad = 0;
        for (int k = 0; k < KB_KEYS; k++) if ((int)kb_is_black(k) != want[k]) bad++;
        check("black/white pattern over 13 keys (mismatches)", bad, 0);
    }

    /* rects (spec section 5) */
    {
        int x, y, w, h;
        kb_key_rect(0, &x, &y, &w, &h);
        check("white C x", x, 16);  check("white C y", y, 384);
        check("white C w", w, 105); check("white C h", h, 130);
        kb_key_rect(12, &x, &y, &w, &h);
        check("top C x", x, 765);   check("top C right edge", x + w - 1, 869);
        kb_key_rect(1, &x, &y, &w, &h);
        check("C# x", x, 90);       check("C# w", w, 64); check("C# h", h, 78);
        kb_key_rect(3, &x, &y, &w, &h);  check("D# x", x, 197);
        kb_key_rect(6, &x, &y, &w, &h);  check("F# x", x, 411);
        kb_key_rect(8, &x, &y, &w, &h);  check("G# x", x, 518);
        kb_key_rect(10, &x, &y, &w, &h); check("A# x", x, 625);
    }
    /* whites never overlap; the keyboard stays inside the band y 379..519 */
    {
        int bad = 0, prev_right = -1;
        for (int k = 0; k < KB_KEYS; k++) {
            if (kb_is_black(k)) continue;
            int x, y, w, h;
            kb_key_rect(k, &x, &y, &w, &h);
            if (x <= prev_right) bad++;
            if (y < 379 || y + h - 1 > 519) bad++;
            prev_right = x + w - 1;
        }
        check("white keys disjoint and inside the band (violations)", bad, 0);
    }

    /* the gate's taps, pinned to the geometry: a geometry edit that strands a
     * tap fails HERE, by name, not as a mystery in QEMU */
    check("P 32 47 lands x", land_x(47), 601);
    check("P 32 47 lands y", land_y(32), 489);
    check("P 32 47 hits white A (key 9)",    hit(land_x(47), land_y(32)), 9);
    check("P 32 64 hits the top C (key 12)", hit(land_x(64), land_y(32)), 12);
    check("P 42 51 hits black A# (key 10)",  hit(land_x(51), land_y(42)), 10);
    /* the same x, lower down, is the white A -- what a z-order bug would hit */
    check("x 652 low on the key is white A", hit(652, 489), 9);

    printf("keyboard_map_test: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
