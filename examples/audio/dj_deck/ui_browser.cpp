/* ui_browser.cpp - see ui_browser.h.
 *
 * The list is PAGED, seven rows at a time with page buttons, not a scrolling
 * list: every pixel LVGL redraws here is a slow CPU write to SDRAM (the core
 * runs with the data cache off), and a list dragged under a finger would be
 * redrawn in full many times a second.  A page turn redraws once.
 * Copyright (c) 2026 Nicholas Newdigate.  SPDX-License-Identifier: MIT */
#include <Arduino.h>
#include <SD.h>
#include "ui_browser.h"
#include "board_io.h"
#include "deck_math.h"
#include "audio_codecs/preview/preview_types.h"
#include "audio_codecs/tempo/tempo_types.h"

#define C_BG   0x141C26
#define C_ROW  0x1E2A38
#define C_ON   0x4A3C18
#define C_TEXT 0xB9CFE0
#define C_DIM  0x5C6F82
#define C_AMBER 0xFFC861

namespace {
const int ROWS = 7, MAX_ENTRIES = 512, NAME_MAX_LEN = 96;
struct Entry { char name[NAME_MAX_LEN]; bool dir; bool meta_done; uint32_t dur_ms; uint32_t bpm_x100; };
EXTMEM Entry g_entries[MAX_ENTRIES];
int g_count = 0, g_page = 0;
bool g_truncated = false, g_card_ok = false;
char g_dir[200] = "/";
BrowserCallbacks g_cb;
lv_obj_t *g_path, *g_msg, *g_pagelbl, *g_rows[ROWS], *g_row_name[ROWS], *g_row_meta[ROWS], *g_prev, *g_next, *g_retry;
int g_meta_next = 0;

int pages() { return g_count ? (g_count + ROWS - 1) / ROWS : 1; }

void join(char *out, size_t cap, const char *dir, const char *name)
{
    snprintf(out, cap, "%s%s%s", dir, (dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/", name);
}
void list_dir()
{
    g_count = 0; g_truncated = false; g_page = 0; g_meta_next = 0;
    g_card_ok = SD.exists("/") || board_sd_begin();
    if (!g_card_ok) { Serial1.println("SD_FAIL"); return; }
    File d = SD.open(g_dir);
    if (!d || !d.isDirectory()) { g_card_ok = false; Serial1.println("SD_FAIL"); return; }
    if (strcmp(g_dir, "/") != 0) { Entry &e = g_entries[g_count++]; strcpy(e.name, ".."); e.dir = true; e.meta_done = true; e.dur_ms = e.bpm_x100 = 0; }
    for (;;) {
        File f = d.openNextFile();
        if (!f) break;
        /* copy the name out before close(): the pointer belongs to the File */
        char n[NAME_MAX_LEN + 1];
        const char *fn = f.name();
        const size_t len = fn ? strlen(fn) : 0;
        const bool dir = f.isDirectory();
        if (len && len < (size_t)NAME_MAX_LEN) memcpy(n, fn, len + 1); else n[0] = 0;
        f.close();
        if (!n[0] || n[0] == '.') continue;
        if (!dir && !deck_is_audio_name(n)) continue;
        if (g_count >= MAX_ENTRIES) { g_truncated = true; break; }
        Entry &e = g_entries[g_count++];
        strcpy(e.name, n); e.dir = dir; e.meta_done = dir; e.dur_ms = e.bpm_x100 = 0;
    }
    d.close();
    /* insertion sort: folders first, then by name; ".." stays on top */
    const int first = (g_count && !strcmp(g_entries[0].name, "..")) ? 1 : 0;
    for (int i = first + 1; i < g_count; i++) {
        const Entry key = g_entries[i];
        int j = i - 1;
        while (j >= first && deck_name_compare(g_entries[j].name, g_entries[j].dir, key.name, key.dir) > 0) { g_entries[j + 1] = g_entries[j]; j--; }
        g_entries[j + 1] = key;
    }
    Serial1.printf("BROWSE dir=%s n=%d\n", g_dir, g_count - first);
}
void show_row(int r)
{
    const int i = g_page * ROWS + r;
    if (i >= g_count || !g_card_ok) { lv_obj_add_flag(g_rows[r], LV_OBJ_FLAG_HIDDEN); return; }
    const Entry &e = g_entries[i];
    lv_obj_remove_flag(g_rows[r], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(g_row_name[r], e.name);
    lv_obj_set_style_text_color(g_row_name[r], lv_color_hex(e.dir ? C_AMBER : C_TEXT), 0);
    if (e.dir) lv_label_set_text(g_row_meta[r], "folder");
    else if (!e.meta_done) lv_label_set_text(g_row_meta[r], "");
    else if (e.dur_ms == 0) lv_label_set_text(g_row_meta[r], "-");
    else if (e.bpm_x100) lv_label_set_text_fmt(g_row_meta[r], "%lu:%02lu   %lu.%lu", (unsigned long)(e.dur_ms / 60000u), (unsigned long)(e.dur_ms / 1000u % 60u),
                                               (unsigned long)(e.bpm_x100 / 100u), (unsigned long)(e.bpm_x100 / 10u % 10u));
    else lv_label_set_text_fmt(g_row_meta[r], "%lu:%02lu", (unsigned long)(e.dur_ms / 60000u), (unsigned long)(e.dur_ms / 1000u % 60u));
}
void show_page()
{
    lv_label_set_text(g_path, g_dir);
    if (!g_card_ok) { lv_label_set_text(g_msg, "No SD card"); lv_obj_remove_flag(g_msg, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(g_retry, LV_OBJ_FLAG_HIDDEN); }
    else if (g_count == 0) { lv_label_set_text(g_msg, "No audio files here"); lv_obj_remove_flag(g_msg, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(g_retry, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(g_msg, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(g_retry, LV_OBJ_FLAG_HIDDEN); }
    for (int r = 0; r < ROWS; r++) show_row(r);
    lv_label_set_text_fmt(g_pagelbl, "%d / %d%s", g_page + 1, pages(), g_truncated ? " +" : "");
    g_meta_next = g_page * ROWS;
}
/* Duration from the .apv header and BPM from the .att header, if the sidecars are there. */
void read_meta(Entry &e)
{
    char path[320], side[330];
    join(path, sizeof(path), g_dir, e.name);
    e.meta_done = true;
    deck_sidecar_name(side, sizeof(side), path, "apv");
    File f = side[0] ? SD.open(side, FILE_READ) : File();
    if (!f) return;
    audio_codecs::preview::ApvHeader ah;
    const bool ok = f.read((uint8_t *)&ah, sizeof(ah)) == (int)sizeof(ah) && ah.magic == audio_codecs::preview::APV_MAGIC;
    f.close();
    if (!ok) return;
    e.dur_ms = ah.duration_ms ? ah.duration_ms : 1;
    deck_sidecar_name(side, sizeof(side), path, "att");
    f = SD.open(side, FILE_READ);
    if (!f) return;
    audio_codecs::tempo::AttHeader th;
    if (f.read((uint8_t *)&th, sizeof(th)) == (int)sizeof(th) && !memcmp(th.magic, audio_codecs::tempo::ATT_MAGIC, 4))
        e.bpm_x100 = deck_bpm_x100(th.global_bpm_q16, 0.0f);
    f.close();
}
bool clicked(lv_event_t *e) { return lv_event_get_code(e) == LV_EVENT_CLICKED; }
void on_row(lv_event_t *e)
{
    if (!clicked(e)) return;
    const int r = (int)(intptr_t)lv_event_get_user_data(e), i = g_page * ROWS + r;
    if (i >= g_count) return;
    const Entry &en = g_entries[i];
    if (en.dir) {
        if (!strcmp(en.name, "..")) {
            char *slash = strrchr(g_dir, '/');
            if (slash && slash != g_dir) *slash = 0; else strcpy(g_dir, "/");
        } else {
            char next[sizeof(g_dir)];
            join(next, sizeof(next), g_dir, en.name);
            if (strlen(next) + 1 < sizeof(g_dir)) strcpy(g_dir, next);
        }
        list_dir(); show_page();
        return;
    }
    char path[320];
    join(path, sizeof(path), g_dir, en.name);
    Serial1.printf("BROWSE_PICK %s\n", path);
    if (g_cb.load) g_cb.load(path);
}
void on_page(lv_event_t *e)
{
    if (!clicked(e)) return;
    const int d = (int)(intptr_t)lv_event_get_user_data(e);
    const int p = g_page + d;
    if (p < 0 || p >= pages()) return;
    g_page = p; show_page();
}
void on_back(lv_event_t *e) { if (clicked(e) && g_cb.back) g_cb.back(); }
void on_retry(lv_event_t *e) { if (clicked(e)) { board_sd_begin(); list_dir(); show_page(); } }

lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y); lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_ROW), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_ON), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_ALL, user);
    return b;
}
lv_obj_t *text(lv_obj_t *parent, int x, int y, int w, const lv_font_t *font, uint32_t color, const char *t)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    if (w) { lv_obj_set_width(l, w); lv_label_set_long_mode(l, LV_LABEL_LONG_DOT); }
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, t);
    return l;
}
}  // namespace

void ui_browser_create(lv_obj_t *scr, BrowserCallbacks cb)
{
    g_cb = cb;
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    g_path = text(scr, 16, 28, 1040, &lv_font_montserrat_28, C_TEXT, "/");
    lv_obj_t *back = box(scr, 1104, 16, 160, 64, on_back, nullptr);
    lv_obj_center(text(back, 0, 0, 0, &lv_font_montserrat_28, C_TEXT, "BACK"));
    for (int r = 0; r < ROWS; r++) {
        g_rows[r] = box(scr, 16, 96 + r * 88, 1072, 80, on_row, (void *)(intptr_t)r);
        g_row_name[r] = text(g_rows[r], 20, 22, 760, &lv_font_montserrat_28, C_TEXT, "");
        g_row_meta[r] = text(g_rows[r], 820, 22, 240, &lv_font_montserrat_28, C_DIM, "");
    }
    g_prev = box(scr, 1104, 96, 160, 256, on_page, (void *)(intptr_t)-1);
    lv_obj_center(text(g_prev, 0, 0, 0, &lv_font_montserrat_28, C_TEXT, "PREV"));
    g_next = box(scr, 1104, 448, 160, 256, on_page, (void *)(intptr_t)+1);
    lv_obj_center(text(g_next, 0, 0, 0, &lv_font_montserrat_28, C_TEXT, "NEXT"));
    g_pagelbl = text(scr, 1120, 384, 0, &lv_font_montserrat_28, C_DIM, "1 / 1");
    g_msg = text(scr, 400, 300, 0, &lv_font_montserrat_28, C_AMBER, "");
    g_retry = box(scr, 440, 380, 200, 96, on_retry, nullptr);
    lv_obj_center(text(g_retry, 0, 0, 0, &lv_font_montserrat_28, C_TEXT, "RETRY"));
    lv_obj_add_flag(g_msg, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(g_retry, LV_OBJ_FLAG_HIDDEN);
}
void ui_browser_show() { list_dir(); show_page(); }
void ui_browser_tick()
{
    /* one row of the page on screen per call: two small reads from the card */
    const int end = g_page * ROWS + ROWS < g_count ? g_page * ROWS + ROWS : g_count;
    while (g_meta_next < end && g_entries[g_meta_next].meta_done) g_meta_next++;
    if (!g_card_ok || g_meta_next >= end) return;
    read_meta(g_entries[g_meta_next]);
    show_row(g_meta_next - g_page * ROWS);
    g_meta_next++;
}
