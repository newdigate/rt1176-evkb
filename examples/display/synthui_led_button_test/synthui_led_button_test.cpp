/* synthui_led_button_test - synthui_led_button (NEW-25) on the RK055
 * (720x1280 XRGB8888, db pipeline), checksummed.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Scene: a 4x4 bank of 16 keys, row-major index i = row*4 + col:
 *    0 off red 100        1 lit red 100         2 lit amber 100       3 lit green 100
 *    4 lit blue 100       5 latched off red 100 6 lit+latched red 100 7 cue off red 100
 *    8 cue+lit red 100    9 disabled off 100   10 disabled+lit 100   11 32 px lit (no dots)
 *   12 34 px lit (dots)  13 150 px lit         14 120x80 lit (centred) 15 100 lit + LV_STATE_PRESSED
 *
 * Phase A (gated): led_button_crc golden -> 64-step LCG delta sequence over
 * lit/pressed/cue/color on keys 0..12 -> a 6-step SCRIPTED TAIL on keys
 * 12/14/15 -> led_button_delta_crc vs led_button_fresh_crc, per-op damage
 * maxima (led_button_damage_op), led_button_damage engagement,
 * led_button_vsync, crc_done, and a PASS/FAIL token gated on delta equality.
 * Keys 13..15 are excluded from the LCG so every per-op bound is exactly the
 * op's box on a 100 px key: press 6640 (the cap group at both offsets), lit
 * and colour 1450 (the halo), cue 900 (ONE bezel-ring strip, 100 x 9 --
 * NEW-50; it was 10000, the whole key, before).  A 150 px key would make
 * each of those larger and say nothing about engagement.
 * led_button_tasks_op (NEW-50) counts draw tasks per setter call, max per
 * op, from LV_EVENT_DRAW_TASK_ADDED on the keys: it is the only number that
 * can see led_draw losing its per-layer clip test, which would make a cue
 * change cost 48 tasks (four strips x twelve layers) while every golden and
 * the delta-equality guard stayed green.
 * The LCG alone cannot exercise every damage box: keys 0..12 are all square
 * and none is held by LV_STATE_PRESSED, so a centring-offset error in a
 * non-square key's damage box, or in the lit box at the LV_STATE_PRESSED
 * offset, would never move a single LCG-driven pixel.  It also never
 * touches led_on_press_edge() (SynthUI src/synthui_led_button.cpp), because
 * the LCG's LED_OP_PRESSED steps all go through
 * synthui_led_button_set_pressed() -- the LATCH setter, a different code
 * path from a real finger's PRESSED/RELEASED events.  The scripted tail
 * covers all of that: (a) press the non-square 120x80 key 14 (press box at
 * ox=20), (b) un-light it while pressed (lit box at ox=20, pressed offset),
 * (c) change the colour of key 15, which is held pressed via
 * lv_obj_add_state(LV_STATE_PRESSED) (lit box at the LV_STATE_PRESSED
 * offset), (d) un-light key 15 (halo removed at that same offset), (e) drive
 * a SCRIPTED lv_indev_t onto key 12's centre and press it -- the real
 * PRESSED event, reaching led_on_press_edge() for the first time in this
 * suite -- (f) release at the same point.  A press followed by a release
 * leaves the drawn state UNCHANGED (key 12 was never latched or given
 * LV_STATE_PRESSED), so steps (e)+(f) must NOT move led_button_fresh_crc;
 * if the RELEASED/PRESS_LOST branch of led_on_press_edge() were ever
 * deleted, the key would stay drawn sunk and only delta equality -- not any
 * golden -- would catch it, which is why that branch had no coverage before.
 * The tail also closes a hole a whole-key-invalidate regression could hide
 * behind: with square, unpressed keys only, set_lit/set_color/set_pressed
 * falling back to lv_obj_invalidate(obj) still passes delta equality (a
 * bigger box is still a CORRECT box) -- per-op damage maxima
 * (led_button_damage_op) are what catch it, since lit/press/color would then
 * read 10000 instead of their small legitimate boxes.
 * ★ Until NEW-50 the OVERALL max could not see that regression either,
 * because a cue change legitimately repainted the whole 10000 px key and the
 * overall bound was 10000 to suit it.  Now that cue damages only the bezel
 * ring (900), the overall bound is press's 6640, so a whole-key fallback
 * trips that too -- the per-op maxima are still what NAME which setter
 * regressed, which is the reason they exist.
 * Phase B (after crc_done, ungated): a playhead cue sweep + LED chaser
 * across all 16 keys, measuring frame time (led_button_fps). */
#include <Arduino.h>
#include <string.h>
#include <math.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_led_button.h"

#ifdef LED_BUTTON_EYEBALL_HOLD
static void eyeball_hold(int n)
{
    if (n != LED_BUTTON_EYEBALL_HOLD) return;
    Serial1.printf("LED_BUTTON_EYEBALL_HOLD=%d\n", n);
    for (;;) { }
}
#else
#define eyeball_hold(n) ((void)0)
#endif

struct KeyConfig {
    int32_t w, h;
    bool lit, pressed, cue, disabled;
    bool lv_state;            /* also lv_obj_add_state(LV_STATE_PRESSED) -- draws as pressed */
    synthui_led_button_color_t color;
};

static const KeyConfig kKeys[16] = {
    { 100, 100, false, false, false, false, false, SYNTHUI_LED_BUTTON_RED   },   /*  0 */
    { 100, 100, true,  false, false, false, false, SYNTHUI_LED_BUTTON_RED   },   /*  1 */
    { 100, 100, true,  false, false, false, false, SYNTHUI_LED_BUTTON_AMBER },   /*  2 */
    { 100, 100, true,  false, false, false, false, SYNTHUI_LED_BUTTON_GREEN },   /*  3 */
    { 100, 100, true,  false, false, false, false, SYNTHUI_LED_BUTTON_BLUE  },   /*  4 */
    { 100, 100, false, true,  false, false, false, SYNTHUI_LED_BUTTON_RED   },   /*  5 latched */
    { 100, 100, true,  true,  false, false, false, SYNTHUI_LED_BUTTON_RED   },   /*  6 lit + latched */
    { 100, 100, false, false, true,  false, false, SYNTHUI_LED_BUTTON_RED   },   /*  7 cue */
    { 100, 100, true,  false, true,  false, false, SYNTHUI_LED_BUTTON_RED   },   /*  8 cue + lit */
    { 100, 100, false, false, false, true,  false, SYNTHUI_LED_BUTTON_RED   },   /*  9 disabled */
    { 100, 100, true,  false, false, true,  false, SYNTHUI_LED_BUTTON_RED   },   /* 10 disabled + lit: no halo */
    {  32,  32, true,  false, false, false, false, SYNTHUI_LED_BUTTON_RED   },   /* 11 no dots */
    {  34,  34, true,  false, false, false, false, SYNTHUI_LED_BUTTON_RED   },   /* 12 dots */
    { 150, 150, true,  false, false, false, false, SYNTHUI_LED_BUTTON_RED   },   /* 13 large */
    { 120,  80, true,  false, false, false, false, SYNTHUI_LED_BUTTON_RED   },   /* 14 non-square */
    { 100, 100, true,  false, false, false, true,  SYNTHUI_LED_BUTTON_RED   },   /* 15 LV_STATE_PRESSED */
};

#define LCG_KEYS 13   /* keys 0..12 take part in the delta sequence */

static lv_obj_t *g_key[16];
static bool g_lit[16], g_pressed[16], g_cue[16];
static synthui_led_button_color_t g_color[16];

static void opaque_bg(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
}

/* Forward declaration: build_bank() wires this to LV_EVENT_DRAW_TASK_ADDED,
 * but its definition sits below (with the rest of the delta-guard state) to
 * keep that block together rather than moving it above build_bank(). */
static void delta_task_cb(lv_event_t *e);

/* Build the bank from the mutable state arrays. Both the golden and the fresh
 * reference pass use this, so the object hierarchy is identical. */
static lv_obj_t *build_bank(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    opaque_bg(scr);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SynthUI LedButton");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    for (int i = 0; i < 16; i++) {
        const int row = i / 4, col = i % 4;
        const KeyConfig *k = &kKeys[i];
        lv_obj_t *b = synthui_led_button_create(scr);
        lv_obj_set_size(b, k->w, k->h);
        const int32_t cx = 90 + col * 180;
        const int32_t cy = 200 + row * 180;
        lv_obj_set_pos(b, cx - k->w / 2, cy - k->h / 2);
        synthui_led_button_set_color(b, g_color[i]);
        synthui_led_button_set_lit(b, g_lit[i]);
        synthui_led_button_set_pressed(b, g_pressed[i]);
        synthui_led_button_set_cue(b, g_cue[i]);
        synthui_led_button_set_disabled(b, k->disabled);
        if (k->lv_state) lv_obj_add_state(b, LV_STATE_PRESSED);
        lv_obj_add_flag(b, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        lv_obj_add_event_cb(b, delta_task_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
        g_key[i] = b;
    }
    return scr;
}

/* Checksum the PRESENTED buffer: flip_sync() first, then scanned_fb(). */
static uint32_t sum_active_screen(void)
{
    lvgl_mipi_panel_flip_sync();
    lvgl_sum_reset();
    lvgl_sum_feed(lvgl_mipi_panel_scanned_fb(), PANEL_FB_BYTES);
    return lvgl_sum_value();
}

static uint32_t sum_screen(lv_obj_t *scr)
{
    lv_screen_load(scr);
    lv_obj_invalidate(scr);
    lv_refr_now(NULL);
    return sum_active_screen();
}

/* --- delta guards: per-invalidate area recorder --- */
enum {
    LED_OP_LIT = 0,
    LED_OP_PRESSED,
    LED_OP_CUE,
    LED_OP_COLOR,
    LED_OP_COUNT
};

static int32_t s_delta_maxarea = 0;
static long    s_delta_total = 0;
static bool    s_delta_record = false;
static int     s_delta_op = 0;
static int32_t s_op_max[LED_OP_COUNT];

/* NEW-50: draw tasks per single setter call, max per op.  Counted from
 * LV_EVENT_DRAW_TASK_ADDED on the keys (LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS,
 * set in build_bank so the golden and fresh passes are identical), so the
 * title label never enters the count.  This is the number that sees what
 * no area bound and no golden can: LVGL renders one pass per invalidated
 * area and lv_draw_rect allocates a task BEFORE any clip test, so a
 * narrower damage box that led_draw does not clip against costs MORE
 * tasks, not fewer -- four strips x 12 = 48 against the whole key's 12
 * (12 = led_draw's task count for a LIT key: the bezel's single
 * lv_draw_rect yields both a FILL and a BORDER task, one of twelve layers;
 * 11 for an unlit key, which skips the halo). */
static int32_t s_op_tasks[LED_OP_COUNT];
static int32_t s_tasks_step = 0;

static void delta_task_cb(lv_event_t *e)
{
    (void)e;
    if (s_delta_record) s_tasks_step++;
}

/* One refresh per setter: fold this step's task count into its op's max.
 * TRAP: any new TRACKED step must call delta_refr(), never lv_refr_now(NULL)
 * directly -- s_op_tasks is reset and folded ONLY here, so a raw
 * lv_refr_now(NULL) still counts into s_tasks_step (delta_task_cb fires
 * regardless) but that count is never folded into s_op_tasks and is
 * silently dropped from the bound, with no error anywhere.  Unlike
 * delta_inv_cb (a persistent LV_EVENT_INVALIDATE_AREA callback that cannot
 * be bypassed this way), this guard is opt-in per call site.  The one
 * remaining raw lv_refr_now(NULL) in this file (the untracked key-12
 * restore in delta_run_sequence) is safe only because s_delta_record is
 * false there, so neither guard is recording. */
static void delta_refr(void)
{
    s_tasks_step = 0;
    lv_refr_now(NULL);
    if (s_tasks_step > s_op_tasks[s_delta_op]) s_op_tasks[s_delta_op] = s_tasks_step;
}

static void delta_inv_cb(lv_event_t *e)
{
    if (!s_delta_record) return;
    const lv_area_t *a = (const lv_area_t *)lv_event_get_param(e);
    const int32_t px = lv_area_get_width(a) * lv_area_get_height(a);
    s_delta_total += px;
    if (px > s_delta_maxarea) s_delta_maxarea = px;
    if (px > s_op_max[s_delta_op]) s_op_max[s_delta_op] = px;
}

/* --- scripted pointer indev: drives led_on_press_edge() for real, through
 * LVGL's own PRESSED/RELEASED events, rather than the widget's setters.
 * No qemu2 touch model needed -- the read_cb is fed from two file-scope
 * variables the tail sets directly, and lv_indev_read() is called to force
 * an immediate synchronous read+process instead of waiting on the read
 * timer's period. */
static int32_t s_touch_x = 0, s_touch_y = 0;
static bool    s_touch_down = false;
static lv_indev_t *g_touch_indev = NULL;

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_touch_x;
    data->point.y = s_touch_y;
    data->state = s_touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* Aim the scripted indev at obj's centre and force one synchronous read. */
static void touch_drive(lv_obj_t *obj, bool down)
{
    lv_area_t c;
    lv_obj_get_coords(obj, &c);
    s_touch_x = (c.x1 + c.x2) / 2;
    s_touch_y = (c.y1 + c.y2) / 2;
    s_touch_down = down;
    lv_indev_read(g_touch_indev);
}

#define LED_BUTTON_DELTA_STEPS 64
#define LED_BUTTON_TAIL_STEPS  6
static uint32_t s_lcg;
static uint32_t lcg_next(void)
{
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return s_lcg;
}

/* Runs ON the already-rendered golden bank; evolves the state arrays in place.
 * After the 64 LCG steps (keys 0..12 only, all square and unpressed), a
 * 6-step SCRIPTED TAIL exercises damage boxes -- and, for its last two
 * steps, an EVENT PATH -- the LCG cannot reach.  See the file-top comment
 * for why each step exists. */
static uint32_t delta_run_sequence(void)
{
    lv_display_add_event_cb(lv_display_get_default(), delta_inv_cb,
                            LV_EVENT_INVALIDATE_AREA, NULL);
    s_lcg = 0x5EEDF00Du;
    s_delta_maxarea = 0;
    s_delta_total = 0;
    for (int i = 0; i < LED_OP_COUNT; i++) s_op_max[i] = 0;
    for (int i = 0; i < LED_OP_COUNT; i++) s_op_tasks[i] = 0;
    s_delta_record = true;

    for (int step = 0; step < LED_BUTTON_DELTA_STEPS; step++) {
        const uint32_t r = lcg_next();
        const int idx = (int)((r >> 16) % LCG_KEYS);
        switch ((r >> 24) & 3) {
        case LED_OP_LIT:
            s_delta_op = LED_OP_LIT;
            g_lit[idx] = !g_lit[idx];
            synthui_led_button_set_lit(g_key[idx], g_lit[idx]);
            break;
        case LED_OP_PRESSED:
            s_delta_op = LED_OP_PRESSED;
            g_pressed[idx] = !g_pressed[idx];
            synthui_led_button_set_pressed(g_key[idx], g_pressed[idx]);
            break;
        case LED_OP_CUE:
            s_delta_op = LED_OP_CUE;
            g_cue[idx] = !g_cue[idx];
            synthui_led_button_set_cue(g_key[idx], g_cue[idx]);
            break;
        default:
            s_delta_op = LED_OP_COLOR;
            g_color[idx] = (synthui_led_button_color_t)(((int)g_color[idx] + 1) & 3);
            synthui_led_button_set_color(g_key[idx], g_color[idx]);
            break;
        }
        delta_refr();
    }

    /* Scripted tail: keys 14 (non-square, unpressed) and 15 (held pressed via
     * LV_STATE_PRESSED) are never touched by the LCG above. */
    s_delta_op = LED_OP_PRESSED;                    /* (a) non-square press box, ox=20 */
    g_pressed[14] = true;
    synthui_led_button_set_pressed(g_key[14], true);
    delta_refr();

    s_delta_op = LED_OP_LIT;                         /* (b) lit box at the pressed offset */
    g_lit[14] = false;
    synthui_led_button_set_lit(g_key[14], false);
    delta_refr();

    s_delta_op = LED_OP_COLOR;                       /* (c) LV_STATE_PRESSED key's lit box */
    g_color[15] = SYNTHUI_LED_BUTTON_AMBER;
    synthui_led_button_set_color(g_key[15], SYNTHUI_LED_BUTTON_AMBER);
    delta_refr();

    s_delta_op = LED_OP_LIT;                         /* (d) halo removed, LV_STATE_PRESSED offset */
    g_lit[15] = false;
    synthui_led_button_set_lit(g_key[15], false);
    delta_refr();

    /* Key 12 takes part in the LCG above (LCG_KEYS=13 is keys 0..12), and
     * with this file's fixed seed it deterministically ends the 64 steps
     * LATCHED (g_pressed[12]==true) -- verified by replaying the exact LCG.
     * led_on_press_edge() early-returns whenever the latch is already set
     * (a latched key does not move for a finger, correctly), so scripting a
     * press+release on it AS LATCHED would prove nothing.  Restore key 12 to
     * unlatched first, UNTRACKED (not one of the tail's six recorded steps,
     * just a precondition -- like (a)-(d) setting their own keys' state
     * directly): a small, legitimate invalidate of its own, then the real
     * scripted press can exercise led_on_press_edge() on an object that
     * actually changes appearance. */
    s_delta_record = false;
    g_pressed[12] = false;
    synthui_led_button_set_pressed(g_key[12], false);
    lv_refr_now(NULL);
    s_delta_record = true;

    /* (e)/(f): a real finger on key 12 (the 34 px key -- its 28x28 press box
     * is 784 px, well under the 6640 press bound; key 13's 150 px press box
     * would be 14880 and blow it).  Now unlatched and not given
     * LV_STATE_PRESSED, so press-then-release must leave it drawn exactly as
     * it started: these two steps are what makes delta equality FAIL if
     * led_on_press_edge()'s RELEASED/PRESS_LOST branch is ever lost -- the
     * PRESSED event alone would still draw it sunk, and with no RELEASED
     * handling nothing ever un-sinks it back to match a fresh render. */
    s_delta_op = LED_OP_PRESSED;                     /* (e) key 12 pressed via the scripted indev */
    touch_drive(g_key[12], true);
    delta_refr();

    s_delta_op = LED_OP_PRESSED;                     /* (f) key 12 released at the same point */
    touch_drive(g_key[12], false);
    delta_refr();

    s_delta_record = false;
    return sum_active_screen();
}

/* --- Phase B: fps measurement and a continuous cue sweep + LED chaser --- */
#define LED_BUTTON_FPS_MAX 512
static uint32_t g_fps_us[LED_BUTTON_FPS_MAX];
static volatile uint32_t g_fps_n = 0, g_fps_frames = 0;
static volatile bool g_fps_timing = false, g_fps_skip = false;
static volatile bool g_fps_rendered = false;
static volatile uint32_t g_fps_t0 = 0;
static uint32_t g_anim_step = 0;

static void fps_refr_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:   g_fps_t0 = micros(); break;
    case LV_EVENT_RENDER_READY: g_fps_rendered = true; break;
    case LV_EVENT_REFR_READY:
        if (g_fps_rendered && g_fps_timing) {
            if (g_fps_skip) g_fps_skip = false;
            else {
                g_fps_frames++;
                if (g_fps_n < LED_BUTTON_FPS_MAX)
                    g_fps_us[g_fps_n++] = micros() - g_fps_t0;
            }
        }
        g_fps_rendered = false;
        break;
    default: break;
    }
}

static void key_anim_cb(lv_timer_t *t)
{
    (void)t;
    g_anim_step++;
    const uint32_t head = g_anim_step % 16;
    for (int i = 0; i < 16; i++) {
        synthui_led_button_set_cue(g_key[i], i == (int)head);
        synthui_led_button_set_lit(g_key[i], ((g_anim_step / 4) + i) % 3 == 0);
    }
}

static void key_fps_phase(const char *tag, uint32_t target_frames)
{
    static bool cbs_added = false;
    if (!cbs_added) {
        lv_display_t *disp = lv_display_get_default();
        lv_display_add_event_cb(disp, fps_refr_cb, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(disp, fps_refr_cb, LV_EVENT_RENDER_READY, NULL);
        lv_display_add_event_cb(disp, fps_refr_cb, LV_EVENT_REFR_READY, NULL);
        cbs_added = true;
    }
    g_fps_n = 0;
    g_fps_frames = 0;
    g_fps_skip = true;
    g_fps_timing = true;

    lv_timer_t *anim = lv_timer_create(key_anim_cb, 15, NULL);
    const uint32_t t0 = millis();
    while (g_fps_n < target_frames && (millis() - t0) < 10000u) {
        lvgl_rt1176_loop();
    }
    g_fps_timing = false;
    lv_timer_delete(anim);

    if (g_fps_n == 0) {
        /* No frame was ever timed (target_frames==0, or the 10 s bound hit
         * first): the array is zeroed, and dividing 1e9 by a zero median
         * printed a fictitious mfps_med=1000000000.  Say what happened
         * instead of deriving stats from nothing. */
        Serial1.printf("%s frames=0\n", tag);
        return;
    }

    uint32_t s[LED_BUTTON_FPS_MAX];
    const uint32_t n = g_fps_n;
    memcpy(s, (const void *)g_fps_us, n * sizeof(uint32_t));
    for (uint32_t i = 1; i < n; i++) {
        const uint32_t v = s[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
        s[j + 1] = v;
    }
    Serial1.printf("%s frames=%lu mfps_med=%lu us_med=%lu us_min=%lu us_max=%lu\n",
                   tag,
                   (unsigned long)g_fps_n,
                   (unsigned long)(1000000000ull / (s[n / 2] ? s[n / 2] : 1)),
                   (unsigned long)s[n / 2],
                   (unsigned long)s[0],
                   (unsigned long)s[n - 1]);
}

void setup()
{
    Serial1.begin(115200);
    while (!Serial1 && millis() < 2000) {}
    Serial1.println("=== BOOT ===");
    Serial1.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n", "synthui_led_button_test", 1, __DATE__, __TIME__);
    Serial1.println("SYNTHUI_LED_BUTTON_BEGIN");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) {
        Serial1.println("crc_done");
        return;
    }
    Display.fillScreen(0x0000);

    lvgl_rt1176_begin();
    lvgl_mipi_panel_create_db(Display);

    /* Scripted pointer indev, created before ANY render: proves its mere
     * existence does not perturb led_button_crc (checked below, against the
     * golden, not merely asserted here). */
    g_touch_indev = lv_indev_create();
    lv_indev_set_type(g_touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(g_touch_indev, touch_read_cb);
    lv_indev_set_display(g_touch_indev, lv_display_get_default());

    Serial1.println("led_button_scene=16 grid=4x4");

    /* Phase A1: full initial render -> golden led_button_crc */
    for (int i = 0; i < 16; i++) {
        g_lit[i] = kKeys[i].lit;
        g_pressed[i] = kKeys[i].pressed;
        g_cue[i] = kKeys[i].cue;
        g_color[i] = kKeys[i].color;
    }
    lv_screen_load(build_bank());
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000) {
        lvgl_rt1176_loop();
    }
    Serial1.printf("LVGL_FLUSHED=%s\n",
                   lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    Serial1.printf("LVGL_BYTES=%lu\n",
                   (unsigned long)(lvgl_mipi_panel_flushed_px()
                                   * PANEL_BYTES_PER_PIXEL));
    const uint32_t led_button_crc = sum_active_screen();
    Serial1.printf("led_button_crc=0x%08lX\n", (unsigned long)led_button_crc);
    eyeball_hold(1);

    /* Phase A2: 64-step deterministic delta sequence vs a fresh full render */
    const uint32_t d_seq = delta_run_sequence();
    eyeball_hold(2);
    const uint32_t d_full = sum_screen(build_bank());
    eyeball_hold(3);

    const bool delta_eq = (d_seq == d_full);
    Serial1.printf("led_button_delta_crc=0x%08lX\n", (unsigned long)d_seq);
    Serial1.printf("led_button_fresh_crc=0x%08lX\n", (unsigned long)d_full);
    Serial1.printf("led_button_delta_eq=%s\n", delta_eq ? "PASS" : "FAIL");
    Serial1.printf("led_button_damage max=%ld total=%ld steps=%d tail=%d\n",
                   (long)s_delta_maxarea, s_delta_total, LED_BUTTON_DELTA_STEPS,
                   LED_BUTTON_TAIL_STEPS);
    Serial1.printf("led_button_damage_op lit=%ld press=%ld cue=%ld color=%ld\n",
                   (long)s_op_max[LED_OP_LIT], (long)s_op_max[LED_OP_PRESSED],
                   (long)s_op_max[LED_OP_CUE], (long)s_op_max[LED_OP_COLOR]);
    Serial1.printf("led_button_tasks_op lit=%ld press=%ld cue=%ld color=%ld\n",
                   (long)s_op_tasks[LED_OP_LIT], (long)s_op_tasks[LED_OP_PRESSED],
                   (long)s_op_tasks[LED_OP_CUE], (long)s_op_tasks[LED_OP_COLOR]);
    Serial1.printf("led_button_vsync flips=%lu isrs=%lu timeouts=%lu\n",
                   (unsigned long)lvgl_mipi_panel_flips(),
                   (unsigned long)lvgl_mipi_panel_vsync_isrs(),
                   (unsigned long)lvgl_mipi_panel_vsync_timeouts());
    Serial1.println("crc_done");
    if (delta_eq) {
        Serial1.println("PASS: SynthUI led_button render verified");
    } else {
        Serial1.println("FAIL: SynthUI led_button delta equality");
    }

    /* Phase B: cue sweep + LED chaser, fps measured (silicon is the answer) */
    key_fps_phase("led_button_fps", 64);

    {
        lv_mem_monitor_t mm;
        lv_mem_monitor(&mm);
        Serial1.printf("led_button_mem total=%lu used_pct=%u max_used=%lu frag_pct=%u\n",
                       (unsigned long)mm.total_size, (unsigned)mm.used_pct,
                       (unsigned long)mm.max_used, (unsigned)mm.frag_pct);
    }

    /* keep animating for an eyes/camera pass */
    lv_timer_create(key_anim_cb, 15, NULL);
}

void loop()
{
    lvgl_rt1176_loop();
}
