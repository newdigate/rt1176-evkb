/* ui_deck.cpp - see ui_deck.h.  Layout A on the 1280x720 landscape canvas.
 * Copyright (c) 2026 Nicholas Newdigate.  SPDX-License-Identifier: MIT */
#include <Arduino.h>
#include "ui_deck.h"
#include "deck_math.h"
#include "synthui_waveform.h"
#include "synthui_fader.h"

#define C_BG     0x141C26
#define C_BTN    0x1E2A38
#define C_BTN_ON 0x4A3C18
#define C_TEXT   0xB9CFE0
#define C_DIM    0x5C6F82
#define C_AMBER  0xFFC861

namespace {
AudioPlayer *g_player;
DeckUiCallbacks g_cb;
lv_obj_t *g_wave, *g_fader, *g_name, *g_fmt, *g_time, *g_left, *g_bpm, *g_pitch, *g_status;
lv_obj_t *g_play, *g_rew, *g_cue, *g_track, *g_zin, *g_zout, *g_range, *g_files;
lv_obj_t *g_play_lbl, *g_track_lbl, *g_range_lbl;

synthui_wave_table_t g_tables[3];
bool g_have_track = false, g_playable = false;
TrackInfo g_info;
uint64_t g_total = 0;
CueState g_cue_state;
float g_range_val = 0.08f, g_pitch_val = 0.0f;
uint32_t g_bpm_q16 = 0;
uint32_t g_last_text_ms = 0, g_fader_press_ms = 0, g_notice_until = 0;
bool g_fader_moved = false;
double g_last_shown = -1e18;

lv_obj_t *label(lv_obj_t *parent, int x, int y, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}
lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h, const char *text, lv_event_cb_t cb, lv_obj_t **lbl = nullptr)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_BTN), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_BTN_ON), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_BTN_ON), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_ALL, nullptr);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    if (lbl) *lbl = l;
    return b;
}
void enable(lv_obj_t *o, bool on) { if (on) lv_obj_remove_state(o, LV_STATE_DISABLED); else lv_obj_add_state(o, LV_STATE_DISABLED); }
void checked(lv_obj_t *o, bool on) { if (on) lv_obj_add_state(o, LV_STATE_CHECKED); else lv_obj_remove_state(o, LV_STATE_CHECKED); }
bool clicked(lv_event_t *e) { return lv_event_get_code(e) == LV_EVENT_CLICKED && !lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_DISABLED); }

void show_play() { const bool p = g_player->isPlaying(); lv_label_set_text(g_play_lbl, p ? "PAUSE" : "PLAY"); checked(g_play, p); }
void apply_pitch()
{
    g_player->setRate(deck_rate_from_pitch(g_pitch_val));
    const long m = lroundf(g_pitch_val * 1000.0f);
    lv_label_set_text_fmt(g_pitch, "%s%ld.%ld%%", m < 0 ? "-" : "+", labs(m) / 10, labs(m) % 10);
    if (g_bpm_q16) { const uint32_t b = deck_bpm_x100(g_bpm_q16, g_pitch_val); lv_label_set_text_fmt(g_bpm, "%lu.%02lu", (unsigned long)(b / 100), (unsigned long)(b % 100)); }
    else lv_label_set_text(g_bpm, "---.--");
}
void set_controls()
{
    for (lv_obj_t *o : {g_play, g_rew, g_cue, g_track, g_zin, g_zout}) enable(o, g_have_track && g_playable);
}

/* ---- transport ---- */
void on_files(lv_event_t *e) { if (clicked(e) && g_cb.open_browser) g_cb.open_browser(); }
void on_play(lv_event_t *e)
{
    if (!clicked(e)) return;
    if (g_player->isPlaying()) g_player->pause(); else g_player->play();
    show_play();
    Serial1.printf("UI_PLAY=%d\n", (int)g_player->isPlaying());
}
void on_rew(lv_event_t *e)
{
    if (!clicked(e)) return;
    g_player->seek(0.0);
    Serial1.println("UI_REW");
}
void on_cue(lv_event_t *e)
{
    /* LV_EVENT_ALL also delivers the events raised while the button is still
     * being built, before g_cue is set: look at the code first. */
    const lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_PRESSED && c != LV_EVENT_RELEASED && c != LV_EVENT_PRESS_LOST) return;
    if (lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_DISABLED)) return;
    CueAct act = CueAct::None;
    if (c == LV_EVENT_PRESSED) act = cue_press(g_cue_state, g_player->isPlaying(), (int64_t)llround(g_player->positionFrames()));
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) act = cue_release(g_cue_state);
    else return;
    const char *name = "None";
    switch (act) {
    case CueAct::SetCue:       name = "SetCue"; synthui_waveform_set_cue(g_wave, g_cue_state.cue); break;
    case CueAct::JumpAndPause: name = "JumpAndPause"; g_player->pause(); g_player->seek((double)g_cue_state.cue); break;
    case CueAct::PreviewStart: name = "PreviewStart"; g_player->play(); break;
    case CueAct::PreviewStop:  name = "PreviewStop"; g_player->pause(); g_player->seek((double)g_cue_state.cue); break;
    default: return;
    }
    show_play();
    Serial1.printf("UI_CUE=%s frame=%ld\n", name, (long)g_cue_state.cue);
}
void on_track(lv_event_t *e)
{
    if (!clicked(e)) return;
    const bool on = !synthui_waveform_get_tracking(g_wave);
    synthui_waveform_set_tracking(g_wave, on);
    checked(g_track, on);
    Serial1.printf("UI_TRACK=%d\n", (int)on);
}
void on_zoom(lv_event_t *e)
{
    if (!clicked(e)) return;
    synthui_waveform_zoom_step(g_wave, lv_event_get_target_obj(e) == g_zin ? +1 : -1);
    Serial1.printf("UI_ZOOM=%ld\n", (long)lround(synthui_waveform_get_zoom(g_wave)));
}
void on_range(lv_event_t *e)
{
    if (!clicked(e)) return;
    g_range_val = g_range_val < 0.1f ? 0.16f : 0.08f;
    lv_label_set_text(g_range_lbl, g_range_val < 0.1f ? "8%" : "16%");
    synthui_fader_set_value(g_fader, deck_fader_from_pitch(g_pitch_val, g_range_val));     /* the pitch stays; the cap moves */
    g_pitch_val = deck_pitch_from_fader(synthui_fader_get_value(g_fader), g_range_val);
    apply_pitch();
}
void on_fader(lv_event_t *e)
{
    const lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        const uint32_t now = millis();
        if (!g_fader_moved && now - g_fader_press_ms < 350) {                             /* double tap: back to zero */
            synthui_fader_set_value(g_fader, 0.5f);
            g_pitch_val = 0.0f;
            apply_pitch();
        }
        g_fader_press_ms = now;
        g_fader_moved = false;
    } else if (c == LV_EVENT_VALUE_CHANGED) {
        g_fader_moved = true;
        g_pitch_val = deck_pitch_from_fader(synthui_fader_get_value(g_fader), g_range_val);
        apply_pitch();
    }
}
/* ---- waveform gestures ---- */
void on_wave(lv_event_t *e)
{
    const lv_event_code_t c = lv_event_get_code(e);
    const double *f = (const double *)lv_event_get_param(e);
    if (c == synthui_waveform_event(SYNTHUI_WAVE_EV_GRAB_BEGIN)) { g_player->scratchBegin(); Serial1.printf("UI_SCRATCH=begin frame=%ld\n", f ? (long)llround(*f) : -1L); }
    else if (c == synthui_waveform_event(SYNTHUI_WAVE_EV_GRAB_MOVE)) { if (f) g_player->scratchTo(*f); }
    else if (c == synthui_waveform_event(SYNTHUI_WAVE_EV_GRAB_END)) { g_player->scratchEnd(); Serial1.printf("UI_SCRATCH=end frame=%ld\n", f ? (long)llround(*f) : -1L); }
    else if (c == synthui_waveform_event(SYNTHUI_WAVE_EV_SEEK)) { if (f) g_player->seek(*f); }
    else if (c == synthui_waveform_event(SYNTHUI_WAVE_EV_TRACK_REQUEST)) { synthui_waveform_set_tracking(g_wave, true); checked(g_track, true); }
}
void take_tables(const PeakBuilder &peaks)
{
    for (int l = 0; l < 3; l++) { g_tables[l].pts = peaks.table(l); g_tables[l].count = peaks.points(l); g_tables[l].frames_per_pt = peaks.framesPerPoint(l); }
}
}  // namespace

void ui_deck_create(lv_obj_t *scr, AudioPlayer &player, DeckUiCallbacks cb)
{
    g_player = &player; g_cb = cb;
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    g_name = label(scr, 16, 14, &lv_font_montserrat_28, C_TEXT, "no track");
    lv_obj_set_width(g_name, 560); lv_label_set_long_mode(g_name, LV_LABEL_LONG_DOT);
    g_fmt = label(scr, 16, 54, &lv_font_montserrat_14, C_DIM, "tap FILES to load one");
    g_status = label(scr, 320, 54, &lv_font_montserrat_14, C_AMBER, "");
    g_time = label(scr, 600, 22, &lv_font_montserrat_28, C_AMBER, "--:--.--");
    g_left = label(scr, 780, 22, &lv_font_montserrat_28, C_TEXT, "--:--.--");
    g_bpm  = label(scr, 990, 22, &lv_font_montserrat_28, C_AMBER, "---.--");
    label(scr, 1104, 34, &lv_font_montserrat_14, C_DIM, "BPM");

    g_wave = synthui_waveform_create(scr);
    lv_obj_set_pos(g_wave, 16, 96);
    lv_obj_set_size(g_wave, 1120, 408);
    lv_obj_add_event_cb(g_wave, on_wave, LV_EVENT_ALL, nullptr);

    g_fader = synthui_fader_create(scr);
    lv_obj_set_pos(g_fader, 1152, 96);
    lv_obj_set_size(g_fader, 112, 420);
    synthui_fader_set_center(g_fader, true);
    synthui_fader_set_panel(g_fader, SYNTHUI_FADER_PANEL_DEEP);
    lv_obj_add_event_cb(g_fader, on_fader, LV_EVENT_ALL, nullptr);
    g_pitch = label(scr, 1160, 524, &lv_font_montserrat_28, C_TEXT, "+0.0%");

    const int y = 528, h = 176;
    g_files = button(scr, 16, y, 176, h, "FILES", on_files);
    g_rew   = button(scr, 200, y, 176, h, "REW", on_rew);
    g_cue   = button(scr, 384, y, 176, h, "CUE", on_cue);
    g_play  = button(scr, 568, y, 248, h, "PLAY", on_play, &g_play_lbl);
    g_track = button(scr, 824, y, 152, h, "TRACK", on_track, &g_track_lbl);
    g_zout  = button(scr, 984, y, 72, h, "-", on_zoom);
    g_zin   = button(scr, 1064, y, 72, h, "+", on_zoom);
    g_range = button(scr, 1152, 572, 112, 132, "8%", on_range, &g_range_lbl);
    checked(g_track, true);
    set_controls();
    apply_pitch();
}

void ui_deck_set_track(const char *name, const TrackInfo &info, const int16_t *pcm, const PeakBuilder &peaks)
{
    (void)pcm;
    g_info = info; g_total = info.total_frames; g_have_track = true; g_playable = false;
    g_cue_state = CueState(); g_bpm_q16 = 0; g_last_shown = -1e18;
    lv_label_set_text(g_name, name);
    lv_label_set_text_fmt(g_fmt, "%lu Hz  %u bit  %s", (unsigned long)info.sample_rate, (unsigned)info.bit_depth, info.channels == 1 ? "mono" : "stereo");
    take_tables(peaks);
    synthui_waveform_set_tables(g_wave, g_tables, 3, info.channels, g_total, info.sample_rate);
    synthui_waveform_set_beats(g_wave, nullptr, 0);
    synthui_waveform_set_cue(g_wave, 0);
    synthui_waveform_set_tracking(g_wave, true);
    synthui_waveform_zoom_fit(g_wave);
    synthui_waveform_set_position(g_wave, 0.0);
    checked(g_track, true);
    set_controls(); show_play(); apply_pitch();
}
void ui_deck_clear_track()
{
    g_have_track = g_playable = false; g_total = 0; g_bpm_q16 = 0; g_info = TrackInfo();
    synthui_waveform_clear(g_wave);
    lv_label_set_text(g_name, "no track");
    lv_label_set_text(g_fmt, "");
    lv_label_set_text(g_status, "");
    lv_label_set_text(g_time, "--:--.--"); lv_label_set_text(g_left, "--:--.--");
    set_controls(); show_play(); apply_pitch();
}
void ui_deck_set_loaded(uint64_t decoded, uint64_t total, const PeakBuilder &peaks, LoadStage stage, uint8_t pct, const char *error)
{
    static uint8_t last_pct = 255; static LoadStage last_stage = LoadStage::Idle;
    if (g_have_track) {
        for (int l = 0; l < 3; l++) g_tables[l].count = peaks.points(l);
        synthui_waveform_set_valid_frames(g_wave, decoded);
        g_total = total;
        const bool playable = decoded >= g_info.sample_rate || stage == LoadStage::Done || stage > LoadStage::Decode;
        if (playable != g_playable) { g_playable = playable; set_controls(); }
    }
    if (error) { lv_label_set_text(g_status, error); last_pct = 255; return; }
    if ((int32_t)(millis() - g_notice_until) < 0) return;
    if (pct == last_pct && stage == last_stage) return;
    last_pct = pct; last_stage = stage;
    static const char *const names[] = {"", "reading", "loading", "analysing", "tempo", "saving", "", ""};
    if (stage == LoadStage::Done || stage == LoadStage::Idle) lv_label_set_text(g_status, "");
    else lv_label_set_text_fmt(g_status, "%s %u%%", names[(int)stage], (unsigned)pct);
}
void ui_deck_retable(const PeakBuilder &peaks, uint64_t total)
{
    if (!g_have_track) return;
    const double pos = g_player->positionFrames(), zoom = synthui_waveform_get_zoom(g_wave);
    const bool tracking = synthui_waveform_get_tracking(g_wave);
    take_tables(peaks);
    g_total = total;
    synthui_waveform_set_tables(g_wave, g_tables, 3, g_info.channels, total, g_info.sample_rate);
    synthui_waveform_set_valid_frames(g_wave, total);
    synthui_waveform_set_zoom(g_wave, zoom);
    synthui_waveform_set_tracking(g_wave, tracking);
    synthui_waveform_set_cue(g_wave, g_cue_state.cue);
    synthui_waveform_set_position(g_wave, pos);
}
void ui_deck_set_tempo(uint32_t bpm_q16, const LoaderBeat *beats, uint32_t n)
{
    g_bpm_q16 = bpm_q16;
    static_assert(sizeof(LoaderBeat) == sizeof(synthui_wave_beat_t), "LoaderBeat must have the layout of synthui_wave_beat_t");
    synthui_waveform_set_beats(g_wave, (const synthui_wave_beat_t *)beats, n);
    apply_pitch();
}
void ui_deck_notice(const char *text)
{
    lv_label_set_text(g_status, text);
    g_notice_until = millis() + 5000;
}
void ui_deck_tick()
{
    if (!g_have_track) return;
    const double pos = g_player->positionFrames();
    /* While tracking, the lane only changes when the head has moved a whole column: at fit zoom
     * that is a few times a second, and telling the widget more often re-copies the lane for nothing. */
    const double zoom = synthui_waveform_get_zoom(g_wave);
    const bool fine = g_player->isScratching() || !synthui_waveform_get_tracking(g_wave);
    if (fine ? pos != g_last_shown : floor(pos / zoom) != floor(g_last_shown / zoom)) {
        synthui_waveform_set_position(g_wave, pos);
        g_last_shown = pos;
    }
    const uint32_t now = millis();
    if (now - g_last_text_ms >= 100) {
        g_last_text_ms = now;
        char t[12];
        deck_time_text(t, pos, g_info.sample_rate, false); lv_label_set_text(g_time, t);
        deck_time_text(t, (double)g_total - pos, g_info.sample_rate, true); lv_label_set_text(g_left, t);
        static bool was = false;
        if (g_player->isPlaying() != was) { was = g_player->isPlaying(); show_play(); }
    }
}
