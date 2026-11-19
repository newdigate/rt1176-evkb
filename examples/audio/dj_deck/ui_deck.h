/* ui_deck.h - the deck screen: header, waveform, transport, pitch.
 * Copyright (c) 2026 Nicholas Newdigate.  SPDX-License-Identifier: MIT */
#pragma once
#include <lvgl.h>
#include "play_buffer.h"
#include "loader.h"

struct DeckUiCallbacks { void (*open_browser)(); };

void ui_deck_create(lv_obj_t *screen, AudioPlayer &player, DeckUiCallbacks cb);
/* A new track: the PCM (still filling) and its peak tables.  Resets cue, zoom and tracking. */
void ui_deck_set_track(const char *name, const TrackInfo &info, const int16_t *pcm, const PeakBuilder &peaks);
void ui_deck_clear_track();
/* Load progress; error text (or nullptr) and a one-off notice (or nullptr) for the header. */
void ui_deck_set_loaded(uint64_t decoded_frames, uint64_t total_frames, const PeakBuilder &peaks, LoadStage stage, uint8_t pct, const char *error);
/* The peak tables moved (the loader packed them when the track ended): take them again. */
void ui_deck_retable(const PeakBuilder &peaks, uint64_t total_frames);
void ui_deck_set_tempo(uint32_t bpm_q16, const LoaderBeat *beats, uint32_t n);
void ui_deck_notice(const char *text);
void ui_deck_tick();                   /* once per loop */
