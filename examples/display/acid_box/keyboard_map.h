/* keyboard_map.h -- pure note<->key arithmetic and key geometry for acid_box's
 * 13-key keyboard (docs/superpowers/specs/2026-09-20-acid-box-piano-keyboard-design.md).
 * LVGL-free C99 so tests/keyboard_map_test.c builds it on the host: the top-C
 * boundary and the keep-or-snap rule are off-by-one territory that no pixel
 * golden can see.
 *
 * The 13 keys of view octave `view` are notes kb_base(view) + 0..12.  The top
 * key of one view and the bottom key of the next are the SAME note, which is
 * why the view is state and cannot be derived from a note alone. */
#ifndef ACIDBOX_KEYBOARD_MAP_H
#define ACIDBOX_KEYBOARD_MAP_H

#include <stdbool.h>
#include <stdint.h>

#define KB_KEYS        13
#define KB_OCT_MIN     1      /* keys C1..C2 */
#define KB_OCT_MAX     3      /* keys C3..C4 */
/* Geometry, logical 1280x720 (spec section 5).  The white pitch 107 tracks the
 * step lane's 108 px columns; the band is y 379..519. */
#define KB_X0          16
#define KB_Y0          384
#define KB_WHITE_W     105
#define KB_WHITE_H     130
#define KB_WHITE_PITCH 107
#define KB_BLACK_W     64
#define KB_BLACK_H     78

static inline int kb_base(int view) { return 12 * (view + 1); }

/* 0..12, or -1 when the note is not on this view's keys. */
static inline int kb_key_for_note(int note, int view)
{
    const int d = note - kb_base(view);
    return (d >= 0 && d <= 12) ? d : -1;
}

/* Keep the view if the note is already on it (so a note entered as the TOP C
 * stays shown as the top C); otherwise snap to the note's own octave. */
static inline int kb_view_for_note(int note, int view)
{
    if (kb_key_for_note(note, view) >= 0) return view;
    int v = note / 12 - 1;
    if (v < KB_OCT_MIN) v = KB_OCT_MIN;
    if (v > KB_OCT_MAX) v = KB_OCT_MAX;
    return v;
}

static inline bool kb_is_black(int k)
{ return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }

/* k must be 0..KB_KEYS-1.  A black key is centred on the 2 px gap between the
 * two whites it sits over. */
static inline void kb_key_rect(int k, int *x, int *y, int *w, int *h)
{
    static const int8_t whites_below[KB_KEYS] = {0,1,1,2,2,3,4,4,5,5,6,6,7};
    const int edge = KB_X0 + whites_below[k] * KB_WHITE_PITCH;
    *y = KB_Y0;
    if (kb_is_black(k)) { *x = edge - 1 - KB_BLACK_W / 2; *w = KB_BLACK_W; *h = KB_BLACK_H; }
    else                { *x = edge;                      *w = KB_WHITE_W; *h = KB_WHITE_H; }
}

#endif /* ACIDBOX_KEYBOARD_MAP_H */
