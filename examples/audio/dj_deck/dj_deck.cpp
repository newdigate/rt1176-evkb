/* dj_deck - a one-deck DJ player for the Nosh board.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Browse the SD card, load a WAV / AIFF / FLAC / MP3 into SDRAM, and play it
 * with a pitch fader, scratching and reverse on a touch waveform, and CDJ cue.
 * The track is decoded progressively (playback can start after a second),
 * and its analysis files -- waveform peaks, spectrum, tempo and beat grid --
 * are made on first load and saved beside it as <name>.apv / .asv / .att.
 *
 * Design: docs/superpowers/specs/2026-11-19-nosh-dj-deck-design.md.
 * Pieces: AudioPlayer (Audio/play_buffer.*), synthui_waveform (SynthUI),
 * audio-codecs (decoders, spectrum, tempo), and in this folder the loader,
 * the format adapters, the peak builder and the two screens.
 *
 * Console (Serial1, J11, 115200): DISPLAY_OK TOUCH_OK AUDIO_OK SD_OK|SD_FAIL
 * DJ_READY, then per load LOAD_START, PROBE_DONE, FORMAT, PEAKS_SRC,
 * DECODE_DONE, TEMPO, SIDECARS, LOAD_DONE | LOAD_FAIL, and UI_* per touch.
 * With AUTO on: AUTO_PRELOAD <path> when the next track starts loading beside
 * the one that is playing (its last 5 s), AUTO_NOROOM if the two do not fit in
 * memory together, AUTO_NEXT <path> when a track has played out and the next
 * goes on the deck, AUTO_PLAY gap_ms=N when it starts (N: the silence between), AUTO_SKIP <path> <why> for one
 * that would not load, AUTO_END when the folder has no more.
 */
#include <Arduino.h>
#include <Wire.h>
#include <SD.h>
#include "Display.h"
#include "PXP.h"
#include "gt911.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "lvgl_gt911_indev.h"
#include "synthui_waveform.h"
#include "AudioStream.h"
#include "play_buffer.h"
#include "output_i2s.h"
#include "control_sgtl5000.h"
#include "board_io.h"
#include "loader.h"
#include "ui_deck.h"
#include "ui_browser.h"
#include "deck_math.h"

static const uint64_t PCM_BUDGET_BYTES = 96ull << 20;        /* about 9 minutes of 16-bit stereo at 44.1 kHz */
#ifndef DJ_LOADER_STEP_US
#define DJ_LOADER_STEP_US 16000
#endif

static AudioPlayer          player;
static AudioOutputI2S       i2s_out;
static AudioConnection      patch_l(player, 0, i2s_out, 0), patch_r(player, 1, i2s_out, 1);
static AudioControlSGTL5000 codec;
static GT911                touch(Wire2, 9, 6);              /* reset, interrupt: as the other RK055 examples */
static Loader               loader(board_storage(), board_mem(), PCM_BUDGET_BYTES);
static lv_obj_t            *scr_deck, *scr_browser;

/* ---- the waveform widget's platform: SDRAM and the PXP ---- */
static void *wf_alloc(size_t n) { return extmem_malloc(n); }
static void wf_release(void *p) { extmem_free(p); }
static void wf_blit(const uint32_t *src, uint32_t src_pitch, uint32_t *dst, uint32_t dst_pitch, uint16_t w, uint16_t h)
{
    PXPSurface s((void *)src, w, h, PXP_XRGB8888, (uint16_t)src_pitch), d(dst, w, h, PXP_XRGB8888, (uint16_t)dst_pitch);
    PXP.op().source(s).output(d).alphaOut(0xFF).run();
}

/* ---- load state the main loop follows ---- */
static uint32_t   g_gen = 0, g_t_start = 0, g_t_stage = 0;
static LoadStage  g_stage = LoadStage::Idle;
static bool       g_track_shown = false, g_tempo_shown = false;
static char       g_name[100], g_path[320];
/* auto-next.  While g_bg, the loader holds the NEXT track (g_bg_path) and the
 * one on the deck lives in g_held: nothing of the loader's reaches the player
 * or the screen until bring_forward(). */
static AutoNext   g_auto;
static HeldTrack  g_held;
static bool       g_bg = false, g_fmt_printed = false;
static char       g_bg_path[sizeof(g_path)];
static uint32_t   g_fg_rate = 44100, g_t_ended = 0;
static const uint32_t PRELOAD_SECONDS = 5;

static void open_browser() { lv_screen_load(scr_browser); ui_browser_show(); }
static void back_to_deck() { lv_screen_load(scr_deck); }
static void load_path(const char *path, bool show_deck = true)
{
    /* the old track goes first: the player and the screen must not touch memory the loader is about to free */
    player.pause();
    player.setSource(nullptr, 0, 2, 44100.0f);
    ui_deck_clear_track();
    loader.release(g_held);                                  /* a track kept alive for an early load: nothing points at it now */
    g_bg = false;
    const char *slash = strrchr(path, '/');
    snprintf(g_name, sizeof(g_name), "%s", slash ? slash + 1 : path);
    if (path != g_path) snprintf(g_path, sizeof(g_path), "%s", path);
    Serial1.printf("LOAD_START %s\n", path);
    g_t_start = g_t_stage = millis();
    g_track_shown = g_tempo_shown = g_fmt_printed = false;
    loader.start(path);
    g_gen = loader.generation();
    g_stage = LoadStage::Idle;                               /* so the first stage change is seen below */
    if (show_deck) lv_screen_load(scr_deck);
}
static void user_load(const char *path) { autonext_manual(g_auto); load_path(path); }
static void set_auto_next(bool on) { g_auto.enabled = on; }

/* ---- auto-next ---- */
/* The audio file after `path` in its own folder, in the browser's order. */
static bool next_in_folder(const char *path, char *out, size_t cap)
{
    const char *slash = strrchr(path, '/');
    if (!slash) return false;
    char dir[256], best[96] = "";
    const size_t dl = slash == path ? 1 : (size_t)(slash - path);
    if (dl >= sizeof(dir)) return false;
    memcpy(dir, path, dl); dir[dl] = 0;
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) return false;
    for (;;) {
        File f = d.openNextFile();
        if (!f) break;
        const char *fn = f.name();                           /* belongs to the File: use it before close() */
        if (!f.isDirectory() && fn && strlen(fn) < sizeof(best) && deck_next_better(slash + 1, fn, best)) strcpy(best, fn);
        f.close();
    }
    d.close();
    if (!best[0]) return false;
    const int n = snprintf(out, cap, "%s%s%s", dir, dir[dl - 1] == '/' ? "" : "/", best);
    return n > 0 && (size_t)n < cap;
}
/* Start loading `next` beside the track that is playing. */
static void preload_path(const char *next)
{
    snprintf(g_bg_path, sizeof(g_bg_path), "%s", next);
    Serial1.printf("AUTO_PRELOAD %s\n", next);
    Serial1.printf("LOAD_START %s\n", next);
    g_t_start = g_t_stage = millis();
    g_fmt_printed = false;
    if (g_bg) loader.start(next);                            /* the deck's track is in g_held already */
    else loader.startKeeping(next, g_held);
    g_bg = true;
    g_gen = loader.generation();
    g_stage = LoadStage::Idle;
}
/* The track on the deck has played out: the one the loader holds takes its place. */
static void bring_forward()
{
    player.pause();
    player.setSource(nullptr, 0, 2, 44100.0f);
    ui_deck_clear_track();
    loader.release(g_held);
    g_bg = false;
    snprintf(g_path, sizeof(g_path), "%s", g_bg_path);
    const char *slash = strrchr(g_path, '/');
    snprintf(g_name, sizeof(g_name), "%.99s", slash ? slash + 1 : g_path);
    g_track_shown = g_tempo_shown = false;                   /* follow_loader() shows it on its next pass */
}
static void auto_step()
{
    const LoadStage st = loader.stage();
    AutoIn in;
    in.playing = player.isPlaying();
    in.scratching = player.isScratching();
    in.busy = st != LoadStage::Idle && st != LoadStage::Done && st != LoadStage::Failed;
    in.failed = st == LoadStage::Failed;
    in.no_memory = in.failed && loader.outOfMemory();
    /* the track on the deck: complete if it has been taken out of the loader for an early load, or is past its decode */
    const bool held = g_held.pcm != nullptr;
    in.loaded = !held && st == LoadStage::Done;
    const bool complete = held || (st > LoadStage::Decode && !in.failed);
    const uint32_t len = player.lengthFrames();
    const double pos = player.positionFrames();
    in.at_end = g_track_shown && complete && len > 0 && pos >= (double)(len - 1);
    in.near_end = g_track_shown && complete && len > 0 && (double)len - pos <= (double)PRELOAD_SECONDS * (double)g_fg_rate;
    in.playable = g_track_shown && !held && !in.failed && (st > LoadStage::Decode || loader.decodedFrames() >= loader.info().sample_rate);
    char next[sizeof(g_path)], notice[40] = "";
    switch (autonext_tick(g_auto, in)) {
    case AutoAct::LoadNext:
        if (in.failed) {
            Serial1.printf("AUTO_SKIP %s %s\n", g_path, loader.error());
            snprintf(notice, sizeof(notice), "skipped %.30s", g_name);
        }
        if (next_in_folder(g_path, next, sizeof(next))) {
            Serial1.printf("AUTO_NEXT %s\n", next);
            if (!in.failed) g_t_ended = millis();            /* a skip does not restart the clock */
            load_path(next, false);                          /* the browser, if it is open, stays open */
            autonext_loading(g_auto);
            if (notice[0]) ui_deck_notice(notice);
        } else {
            Serial1.println("AUTO_END");
            autonext_none_left(g_auto);
        }
        break;
    case AutoAct::Preload: {
        /* the file after the deck's track, or after the early load that has just failed */
        const char *after = g_bg ? g_bg_path : g_path;
        if (g_bg) {
            Serial1.printf("AUTO_SKIP %s %s\n", g_bg_path, loader.error());
            const char *slash = strrchr(g_bg_path, '/');
            snprintf(notice, sizeof(notice), "skipped %.30s", slash ? slash + 1 : g_bg_path);
        }
        if (next_in_folder(after, next, sizeof(next))) {
            preload_path(next);
            autonext_preloading(g_auto);
        } else {
            if (g_bg) { loader.cancel(); g_bg = false; g_stage = LoadStage::Idle; }      /* the deck's track stays in g_held */
            autonext_no_preload(g_auto);
        }
        if (notice[0]) ui_deck_notice(notice);
        break;
    }
    case AutoAct::DropPreload:
        Serial1.printf("AUTO_NOROOM %s\n", g_bg_path);
        loader.cancel();
        g_bg = false;
        g_stage = LoadStage::Idle;
        break;
    case AutoAct::Swap:
        Serial1.printf("AUTO_NEXT %s\n", g_bg_path);
        g_t_ended = millis();
        bring_forward();
        break;
    case AutoAct::Play:
        player.play();
        Serial1.printf("AUTO_PLAY gap_ms=%lu\n", (unsigned long)(millis() - g_t_ended));     /* silence between the two tracks */
        break;
    default: break;
    }
}

/* Follow the loader: hand new data to the player and the screen, report stage changes. */
static void follow_loader()
{
    const LoadStage st = loader.stage();
    if (st == LoadStage::Idle) return;
    const bool have = st >= LoadStage::Decode && st <= LoadStage::Done && loader.pcm();
    if (have && !g_fmt_printed) {
        const TrackInfo &i = loader.info();
        Serial1.printf("PROBE_DONE ms=%lu\n", (unsigned long)(millis() - g_t_stage));
        Serial1.printf("FORMAT rate=%lu ch=%u bits=%u frames=%lu\n", (unsigned long)i.sample_rate, (unsigned)i.channels, (unsigned)i.bit_depth,
                       (unsigned long)loader.totalFrames());
        Serial1.printf("PEAKS_SRC=%s\n", loader.peaksFromFile() ? "file" : "built");
        g_fmt_printed = true;
        g_t_stage = millis();
    }
    if (have && !g_bg && !g_track_shown) {
        const TrackInfo &i = loader.info();
        player.setSource(loader.pcm(), (uint32_t)loader.totalFrames(), i.channels, (float)i.sample_rate);
        player.setAvailableFrames(0);
        ui_deck_set_track(g_name, i, loader.pcm(), loader.peaks());
        /* a track that was loaded early may be past its decode already: its length and tables are final */
        if (st > LoadStage::Decode) ui_deck_retable(loader.peaks(), loader.totalFrames());
        g_fg_rate = i.sample_rate ? i.sample_rate : 44100;
        g_track_shown = true;
    }
    if (g_track_shown && !g_bg) {
        player.setAvailableFrames((uint32_t)loader.decodedFrames());
        /* The screen takes the new extent five times a second, not every loop: each
         * update redraws the waveform (~40 ms), and a loop that draws no frame gives
         * its whole time to the loader. */
        static uint32_t t_ui = 0;
        if (st != g_stage || millis() - t_ui >= 200) {
            ui_deck_set_loaded(loader.decodedFrames(), loader.totalFrames(), loader.peaks(), st, loader.progressPct(), nullptr);
            t_ui = millis();
        }
    }
    if (g_track_shown && !g_bg && !g_tempo_shown && loader.tempoReady()) {
        ui_deck_set_tempo(loader.bpmQ16(), loader.beats(), loader.beatCount());
        g_tempo_shown = true;
    }
    if (st == g_stage) return;
    /* a stage has just ended */
    const uint32_t now = millis();
    if (g_stage == LoadStage::Decode && st != LoadStage::Failed) {
        /* the track's real length is known and the peak tables have been packed: take both again */
        if (g_track_shown && !g_bg) {
            const double pos = player.positionFrames();
            const bool was_playing = player.isPlaying();
            if (loader.totalFrames() != player.lengthFrames()) {
                player.setSource(loader.pcm(), (uint32_t)loader.totalFrames(), loader.info().channels, (float)loader.info().sample_rate);
                player.seek(pos);
                if (was_playing) player.play();
            }
            ui_deck_retable(loader.peaks(), loader.totalFrames());
            if (loader.tempoReady()) ui_deck_set_tempo(loader.bpmQ16(), loader.beats(), loader.beatCount());
        }
        Serial1.printf("DECODE_DONE ms=%lu frames=%lu\n", (unsigned long)(now - g_t_stage), (unsigned long)loader.totalFrames());
        g_t_stage = now;
    }
    if (st == LoadStage::Save || (st == LoadStage::Done && g_stage != LoadStage::Save)) {
        if (loader.tempoReady()) {
            const uint32_t b = (uint32_t)(((uint64_t)loader.bpmQ16() * 100u + 32768u) >> 16);
            Serial1.printf("TEMPO bpm_x100=%lu beats=%lu src=%s ms=%lu\n", (unsigned long)b, (unsigned long)loader.beatCount(),
                           st == LoadStage::Save ? "built" : "file", (unsigned long)(now - g_t_stage));
        }
        g_t_stage = now;
    }
    if (st == LoadStage::Done) {
        const SidecarResult s = loader.sidecars();
        Serial1.printf("SIDECARS=%s\n", s == SidecarResult::Saved ? "SAVED" : (s == SidecarResult::Reused ? "REUSED" : "FAILED"));
        if (s == SidecarResult::SaveFailed && !g_bg) ui_deck_notice("analysis not saved");
        Serial1.printf("LOAD_DONE ms=%lu\n", (unsigned long)(now - g_t_start));
    }
    if (st == LoadStage::Failed) {
        Serial1.printf("LOAD_FAIL %s\n", loader.error());
        if (!g_bg) {                                         /* an early load failing is nothing to the track on the deck */
            player.pause();
            player.setSource(nullptr, 0, 2, 44100.0f);
            ui_deck_clear_track();
            ui_deck_set_loaded(0, 0, loader.peaks(), st, 0, loader.error());
            g_track_shown = false;
        }
    }
    g_stage = st;
}

void setup()
{
    Serial1.begin(115200);
    while (!Serial1 && millis() < 2000) {}
    Serial1.println("=== BOOT ===");
    Serial1.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n", "dj_deck", 1, __DATE__, __TIME__);

    if (!Display.begin() || !PXP.begin()) { Serial1.println("DISPLAY_FAIL"); return; }
    lvgl_rt1176_begin();
    lvgl_mipi_panel_set_rot_threshold(0xFFFFFFFFu);
    lv_display_t *disp = lvgl_mipi_panel_create_rotated(Display, LVGL_PANEL_PRESENT_CW90);
    static const synthui_waveform_platform_t plat = {wf_alloc, wf_release, wf_blit};
    synthui_waveform_set_platform(&plat);
    lvgl_mipi_panel_set_preflip_cb(synthui_waveform_compose_into);
    Serial1.println("DISPLAY_OK");

    Wire2.begin();
    Wire2.setClock(400000);
    if (touch.begin()) { lvgl_gt911_indev_create_rotated(disp, touch, LVGL_PANEL_PRESENT_CW90); Serial1.println("TOUCH_OK"); }
    else Serial1.println("TOUCH_FAIL");

    AudioMemory(16);
    const bool codec_ok = codec.enable();
    codec.lineOutLevel(13);                                  /* 3.16 Vpp full scale */
    codec.volume(0.6f);
    Serial1.println(codec_ok ? "AUDIO_OK" : "AUDIO_FAIL");

    Serial1.println(board_sd_begin() ? "SD_OK" : "SD_FAIL");

    scr_deck = lv_screen_active();
    scr_browser = lv_obj_create(nullptr);
    ui_deck_create(scr_deck, player, DeckUiCallbacks{open_browser, set_auto_next});
    ui_browser_create(scr_browser, BrowserCallbacks{user_load, back_to_deck});
    Serial1.println("DJ_READY");
}

#ifdef DJ_BENCH
/* Bench mode (-DDJ_BENCH=1): no hands needed.  Writes the probe's clips to
 * /djbench on the card, removes their sidecars, then loads each one twice --
 * the first load builds and saves the analysis, the second must reuse it --
 * and leaves the last one playing.  Tokens: BENCH_FILE, BENCH_DONE. */
extern "C" const uint8_t clip_short_wav[], clip_short_wav_end[], clip_short_aiff[], clip_short_aiff_end[],
                         clip_click_flac[], clip_click_flac_end[], clip_click_mp3[], clip_click_mp3_end[];
static void bench_step()
{
    struct Clip { const char *path; const uint8_t *data, *end; };
    static const Clip clips[] = {
        {"/djbench/short.wav", clip_short_wav, clip_short_wav_end}, {"/djbench/short.aiff", clip_short_aiff, clip_short_aiff_end},
        {"/djbench/click.flac", clip_click_flac, clip_click_flac_end}, {"/djbench/click.mp3", clip_click_mp3, clip_click_mp3_end}};
    static int n = -1;                                       /* -1: write the files; then two loads per clip */
    const int total = 2 * (int)(sizeof(clips) / sizeof(clips[0]));
    if (n >= total) return;
    if (n < 0) {
        SD.mkdir("/djbench");
        for (const Clip &c : clips) {
            char side[64];
            for (const char *ext : {".apv", ".asv", ".att"}) { snprintf(side, sizeof(side), "%s%s", c.path, ext); SD.remove(side); }
            SD.remove(c.path);
            File f = SD.open(c.path, FILE_WRITE);
            const size_t len = (size_t)(c.end - c.data), put = f ? f.write(c.data, len) : 0;
            f.close();
            Serial1.printf("BENCH_FILE %s bytes=%lu %s\n", c.path, (unsigned long)len, put == len ? "OK" : "WRITE_FAIL");
        }
        n = 0;
        load_path(clips[0].path);
        return;
    }
    const LoadStage st = loader.stage();
    if (st != LoadStage::Done && st != LoadStage::Failed) return;
    if (++n < total) { load_path(clips[n / 2].path); return; }
    player.play();
    Serial1.println("BENCH_DONE");
}
#endif

void loop()
{
    lvgl_rt1176_loop();
    if (lv_screen_active() == scr_deck) ui_deck_tick(); else ui_browser_tick();
    loader.step(DJ_LOADER_STEP_US, board_now_us);
    follow_loader();
    auto_step();
#ifdef DJ_BENCH
    bench_step();
#endif
#ifdef DJ_FPS
    static uint32_t t_fps = 0, f0 = 0;
    if (millis() - t_fps >= 1000) {
        const uint32_t f = lvgl_mipi_panel_flips();
        if (loader.stage() != LoadStage::Idle && loader.stage() != LoadStage::Done && loader.stage() != LoadStage::Failed)
            Serial1.printf("FPS=%lu stage=%d\n", (unsigned long)(f - f0), (int)loader.stage());
        f0 = f; t_fps = millis();
    }
#endif
}
