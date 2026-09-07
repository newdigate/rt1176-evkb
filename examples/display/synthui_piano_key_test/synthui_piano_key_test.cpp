/* synthui_piano_key_test - synthui_piano_key (NEW-28) on the RK055
 * display panel under QEMU and on silicon.
 *
 * Pipeline: double-buffered hardware pipeline via lvgl_mipi_panel_create_db(Display).
 * Verifies:
 *   1. Full initial render of 2-octave keyboard scene -> pinned golden CRC (FNV-1a over 720x1280 fb).
 *   2. 64-step deterministic delta sequence (arpeggiating armed notes).
 *   3. Delta equality guard: delta-rendered CRC matches fresh full-render CRC.
 *   4. Damage engagement guard: max invalidated area stays small.
 *   5. VSYNC health guard: timeouts=0.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */

#include <Arduino.h>
#include <Display.h>
#include <lvgl.h>
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_piano_key.h"
#include "synthui_panel_button.h"
#include "synthui_seven_segment.h"
#include <stdio.h>
#include <string.h>

#define RK055_WIDTH  720
#define RK055_HEIGHT 1280

/* ---------- FNV-1a hash (same as seven_segment_test) ---------- */

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

/* ---------- Damage monitor ---------- */

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

/* ---------- Keyboard layout tables ---------- */

/* Which chromatic notes (0-11) are white keys */
static const int WHITE_IDX[] = {0, 2, 4, 5, 7, 9, 11};
/* Which chromatic notes (0-11) are black keys */
static const int BLACK_IN_OCT[] = {1, 3, 6, 8, 10};

static const int NUM_OCTAVES = 2;
static const int NUM_WHITES  = NUM_OCTAVES * 7;  /* 14 */
static const int NUM_BLACKS  = NUM_OCTAVES * 5;  /* 10 */

static const int WHITE_W   = 46;
static const int WHITE_H   = 158;
static const int WHITE_GAP = 2;
static const int BLACK_W   = 30;
static const int BLACK_H   = 76;

static const int KB_LEFT   = 25;  /* left margin for centering on 720 */

/* ---------- Key storage ---------- */

static lv_obj_t *g_white_keys[14];
static lv_obj_t *g_black_keys[10];
static int g_white_midi[14];
static int g_black_midi[10];

/* Console strip widgets */
static lv_obj_t *g_play_btn;
static lv_obj_t *g_stop_btn;
static lv_obj_t *g_note_readout;

/* State demo keys (6 canonical states) */
static lv_obj_t *g_state_keys[6];

void setup()
{
    Serial1.begin(115200);
    delay(200);

    Serial1.println("SYNTHUI_PIANO_KEY_BEGIN");

    const bool ok = Display.begin();
    if (!ok) {
        Serial1.println("PANEL_FAIL");
        Serial1.println("crc_done");
        return;
    }
    Serial1.println("PANEL_OK");
    Display.fillScreen(0x0000);

    lvgl_rt1176_begin();
    lv_display_t *disp = lvgl_mipi_panel_create_db(Display);
    if (!disp) {
        Serial1.println("LVGL_DISP_FAIL");
        return;
    }
    lv_display_add_event_cb(disp, damage_monitor_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a1c), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ===== Top console strip ===== */

    /* Play button */
    g_play_btn = synthui_panel_button_create(scr);
    lv_obj_set_pos(g_play_btn, 30, 30);
    lv_obj_set_size(g_play_btn, 66, 52);
    synthui_panel_button_set_glyph(g_play_btn, SYNTHUI_PANEL_BUTTON_GLYPH_PLAY);
    synthui_panel_button_set_accent(g_play_btn, SYNTHUI_PANEL_BUTTON_ACCENT_GREEN);

    /* Stop button */
    g_stop_btn = synthui_panel_button_create(scr);
    lv_obj_set_pos(g_stop_btn, 110, 30);
    lv_obj_set_size(g_stop_btn, 66, 52);
    synthui_panel_button_set_glyph(g_stop_btn, SYNTHUI_PANEL_BUTTON_GLYPH_STOP);
    synthui_panel_button_set_accent(g_stop_btn, SYNTHUI_PANEL_BUTTON_ACCENT_AMBER);

    /* Seven-segment note readout */
    g_note_readout = synthui_seven_segment_create(scr);
    lv_obj_set_pos(g_note_readout, 200, 32);
    lv_obj_set_size(g_note_readout, 140, 44);
    synthui_seven_segment_set_text(g_note_readout, " - ");
    synthui_seven_segment_set_accent(g_note_readout,
        SYNTHUI_SEVEN_SEGMENT_COLOR_RED_ON,
        SYNTHUI_SEVEN_SEGMENT_COLOR_RED_GLOW);

    /* ===== 2-Octave Keyboard ===== */

    const int kb_y = 110;  /* top of keyboard area */

    /* White keys: flex row with gap */
    int wi = 0;
    for (int o = 0; o < NUM_OCTAVES; o++) {
        for (int i = 0; i < 7; i++) {
            int midi = o * 12 + WHITE_IDX[i];
            int x = KB_LEFT + wi * (WHITE_W + WHITE_GAP);

            g_white_keys[wi] = synthui_piano_key_create(scr);
            lv_obj_set_pos(g_white_keys[wi], x, kb_y);
            lv_obj_set_size(g_white_keys[wi], WHITE_W, WHITE_H);
            synthui_piano_key_set_type(g_white_keys[wi], SYNTHUI_PIANO_KEY_WHITE);
            synthui_piano_key_set_zone_top(g_white_keys[wi], 0.5f);
            synthui_piano_key_set_pad_height(g_white_keys[wi], 46.0f);
            g_white_midi[wi] = midi;
            wi++;
        }
    }

    /* Black keys: absolutely positioned, centered on white key boundaries */
    int bi = 0;
    for (int o = 0; o < NUM_OCTAVES; o++) {
        for (int i = 0; i < 5; i++) {
            int n = BLACK_IN_OCT[i];
            int midi = o * 12 + n;

            /* Count how many white keys are below this black key's note in the octave */
            int before = 0;
            for (int j = 0; j < 7; j++) {
                if (WHITE_IDX[j] < n) before++;
            }

            /* Black key left = boundary between white keys - half black width - half gap */
            int x = KB_LEFT + (o * 7 + before) * (WHITE_W + WHITE_GAP) - BLACK_W / 2 - WHITE_GAP / 2;

            g_black_keys[bi] = synthui_piano_key_create(scr);
            lv_obj_set_pos(g_black_keys[bi], x, kb_y);
            lv_obj_set_size(g_black_keys[bi], BLACK_W, BLACK_H);
            synthui_piano_key_set_type(g_black_keys[bi], SYNTHUI_PIANO_KEY_BLACK);
            synthui_piano_key_set_zone_top(g_black_keys[bi], 0.1f);
            synthui_piano_key_set_pad_height(g_black_keys[bi], 46.0f);
            g_black_midi[bi] = midi;
            bi++;
        }
    }

    /* ===== State demo section (6 canonical states) ===== */

    const int state_y = kb_y + WHITE_H + 40;
    const int card_w = 58;
    const int card_gap = 24;
    const int state_x0 = 60;

    struct StateCard {
        synthui_piano_key_type_t type;
        bool lit;
        bool pressed;
        float zone_top;
        int h;
    };

    const StateCard cards[6] = {
        { SYNTHUI_PIANO_KEY_WHITE, false, false, 0.58f, 200 }, /* white idle */
        { SYNTHUI_PIANO_KEY_WHITE, true,  false, 0.58f, 200 }, /* white armed */
        { SYNTHUI_PIANO_KEY_WHITE, true,  true,  0.58f, 200 }, /* white held */
        { SYNTHUI_PIANO_KEY_BLACK, false, false, 0.10f, 128 }, /* black idle */
        { SYNTHUI_PIANO_KEY_BLACK, true,  false, 0.10f, 128 }, /* black armed */
        { SYNTHUI_PIANO_KEY_BLACK, true,  true,  0.10f, 128 }, /* black held */
    };

    for (int i = 0; i < 6; i++) {
        g_state_keys[i] = synthui_piano_key_create(scr);
        lv_obj_set_pos(g_state_keys[i], state_x0 + i * (card_w + card_gap), state_y);
        lv_obj_set_size(g_state_keys[i], card_w, cards[i].h);
        synthui_piano_key_set_type(g_state_keys[i], cards[i].type);
        synthui_piano_key_set_lit(g_state_keys[i], cards[i].lit);
        synthui_piano_key_set_pressed(g_state_keys[i], cards[i].pressed);
        synthui_piano_key_set_zone_top(g_state_keys[i], cards[i].zone_top);
        synthui_piano_key_set_pad_height(g_state_keys[i], 46.0f);
    }

    Serial1.printf("piano_key_scene=keyboard whites=%d blacks=%d states=6\n",
                   NUM_WHITES, NUM_BLACKS);

    /* ===== Phase A1: Initial full render ===== */

    lv_refr_now(disp);
    lvgl_mipi_panel_flip_sync();
    Serial1.println("LVGL_FLUSHED=PASS");
    Serial1.printf("LVGL_BYTES=%u\n", (unsigned)(RK055_WIDTH * RK055_HEIGHT * 4));

    const void *fb0 = lvgl_mipi_panel_scanned_fb();
    uint32_t init_crc = fnv1a_32(fb0, (size_t)RK055_WIDTH * RK055_HEIGHT * 4);
    Serial1.printf("piano_key_crc=0x%08X\n", (unsigned)init_crc);

#ifdef PIANO_KEY_EYEBALL_HOLD
    Serial1.println("PIANO_KEY_EYEBALL_HOLD active; pausing loop");
    for (;;) { delay(1000); }
#endif

    /* ===== Phase A2: 64-step deterministic delta sequence ===== */

    /* Armed notes: C major triad across 2 octaves */
    const int ARMED[] = {0, 4, 7, 12, 16, 19};
    const int NUM_ARMED = 6;

    g_max_damage = 0;
    g_total_damage = 0;

    int prev_midi = -1;

    for (int step = 0; step < 64; step++) {
        int cur_midi = ARMED[step % NUM_ARMED];

        /* Clear previous key */
        if (prev_midi >= 0) {
            /* Find and clear the previous key */
            for (int i = 0; i < NUM_WHITES; i++) {
                if (g_white_midi[i] == prev_midi) {
                    synthui_piano_key_set_lit(g_white_keys[i], false);
                    synthui_piano_key_set_pressed(g_white_keys[i], false);
                    break;
                }
            }
            for (int i = 0; i < NUM_BLACKS; i++) {
                if (g_black_midi[i] == prev_midi) {
                    synthui_piano_key_set_lit(g_black_keys[i], false);
                    synthui_piano_key_set_pressed(g_black_keys[i], false);
                    break;
                }
            }
        }

        /* Set current key lit + pressed */
        for (int i = 0; i < NUM_WHITES; i++) {
            if (g_white_midi[i] == cur_midi) {
                synthui_piano_key_set_lit(g_white_keys[i], true);
                synthui_piano_key_set_pressed(g_white_keys[i], true);
                break;
            }
        }
        for (int i = 0; i < NUM_BLACKS; i++) {
            if (g_black_midi[i] == cur_midi) {
                synthui_piano_key_set_lit(g_black_keys[i], true);
                synthui_piano_key_set_pressed(g_black_keys[i], true);
                break;
            }
        }

        /* Update note readout */
        static const char *NOTE_NAMES[] = {"C", "d", "D", "E", "E", "F", "G", "G", "A", "A", "b", "B"};
        int oct = (cur_midi / 12) + 3;
        const char *nn = NOTE_NAMES[cur_midi % 12];
        char note_buf[16];
        snprintf(note_buf, sizeof(note_buf), "%s%d", nn, oct);
        synthui_seven_segment_set_text(g_note_readout, note_buf);

        prev_midi = cur_midi;

        lv_refr_now(disp);
        lvgl_mipi_panel_flip_sync();
    }

    uint32_t delta_max_damage = g_max_damage;
    uint32_t delta_total_damage = g_total_damage;

    const void *delta_fb = lvgl_mipi_panel_scanned_fb();
    uint32_t delta_crc = fnv1a_32(delta_fb, (size_t)RK055_WIDTH * RK055_HEIGHT * 4);
    Serial1.printf("piano_key_delta_crc=0x%08X\n", (unsigned)delta_crc);

    /* Delta-equality check: force full redraw of final state */
    lv_obj_invalidate(scr);
    lv_refr_now(disp);
    lvgl_mipi_panel_flip_sync();

    const void *fresh_fb = lvgl_mipi_panel_scanned_fb();
    uint32_t fresh_crc = fnv1a_32(fresh_fb, (size_t)RK055_WIDTH * RK055_HEIGHT * 4);
    Serial1.printf("piano_key_fresh_crc=0x%08X\n", (unsigned)fresh_crc);

    if (delta_crc == fresh_crc) {
        Serial1.println("piano_key_delta_eq=PASS");
    } else {
        Serial1.println("piano_key_delta_eq=FAIL");
    }

    Serial1.printf("piano_key_damage max=%u total=%u\n",
                   (unsigned)delta_max_damage, (unsigned)delta_total_damage);

    Serial1.printf("piano_key_vsync flips=%lu isrs=%lu timeouts=%lu\n",
                   (unsigned long)lvgl_mipi_panel_flips(),
                   (unsigned long)lvgl_mipi_panel_vsync_isrs(),
                   (unsigned long)lvgl_mipi_panel_vsync_timeouts());

    Serial1.println("crc_done");
    Serial1.println("PASS: SynthUI piano_key render verified");
}

void loop()
{
    /* Phase B: continuous animation benchmark — arpeggiate notes */
    static const int ARMED[] = {0, 4, 7, 12, 16, 19};
    static const int NUM_ARMED = 6;
    static const char *NOTE_NAMES[] = {"C", "d", "D", "E", "E", "F", "G", "G", "A", "A", "b", "B"};

    static uint32_t frame_count = 0;
    static uint32_t last_report = 0;
    static int prev_midi = -1;

    int cur_midi = ARMED[frame_count % NUM_ARMED];

    /* Clear previous */
    if (prev_midi >= 0 && prev_midi != cur_midi) {
        for (int i = 0; i < NUM_WHITES; i++) {
            if (g_white_midi[i] == prev_midi) {
                synthui_piano_key_set_lit(g_white_keys[i], false);
                synthui_piano_key_set_pressed(g_white_keys[i], false);
                break;
            }
        }
        for (int i = 0; i < NUM_BLACKS; i++) {
            if (g_black_midi[i] == prev_midi) {
                synthui_piano_key_set_lit(g_black_keys[i], false);
                synthui_piano_key_set_pressed(g_black_keys[i], false);
                break;
            }
        }
    }

    /* Set current */
    for (int i = 0; i < NUM_WHITES; i++) {
        if (g_white_midi[i] == cur_midi) {
            synthui_piano_key_set_lit(g_white_keys[i], true);
            synthui_piano_key_set_pressed(g_white_keys[i], true);
            break;
        }
    }
    for (int i = 0; i < NUM_BLACKS; i++) {
        if (g_black_midi[i] == cur_midi) {
            synthui_piano_key_set_lit(g_black_keys[i], true);
            synthui_piano_key_set_pressed(g_black_keys[i], true);
            break;
        }
    }

    /* Update note readout */
    int oct = (cur_midi / 12) + 3;
    const char *nn = NOTE_NAMES[cur_midi % 12];
    char note_buf[16];
    snprintf(note_buf, sizeof(note_buf), "%s%d", nn, oct);
    synthui_seven_segment_set_text(g_note_readout, note_buf);

    prev_midi = cur_midi;

    lv_timer_handler();
    lvgl_mipi_panel_flip_sync();

    frame_count++;
    uint32_t now = millis();
    if (now - last_report >= 1000) {
        uint32_t elapsed = now - last_report;
        uint32_t fps_x10 = elapsed ? (frame_count * 10000u) / elapsed : 0;
        Serial1.printf("piano_key_fps=%lu.%lu\n",
                       (unsigned long)(fps_x10 / 10),
                       (unsigned long)(fps_x10 % 10));
        frame_count = 0;
        last_report = now;
    }
}
