/* synthui_waveform_test - gate for SynthUI's waveform widget.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Landscape 1280x720 (rotated present, 32 bpp).  The widget is fed a
 * deterministic synthetic "track" (6 minutes at 44.1 kHz, stereo, three peak
 * tables at 128 / 2048 / 32768 frames per point, 720 beats) and put through
 * nine fixed states.  Each prints the FNV-1a checksum of the scanned-out
 * frame:   wf_crc[<name>]=0x%08X   (goldens pinned in run_qemu.sh).
 *
 *   empty    no track
 *   fit      whole track across the lane, head at 0, tracking
 *   mid      3 min in, 2048 frames per pixel
 *   close    3 min in, 128 frames per pixel
 *   loading  only the first minute valid, head at 30 s, fit
 *   free     tracking off, view at 1 min, head at 3 min (off-screen: chevron)
 *   cue      cue 1 s before the head, 2048 frames per pixel
 *   end      head on the last frame: empty lane right of the end
 *   grid     5000 beats at fit zoom: wf_grid_lines must stay <= 184
 */
#include <Arduino.h>
#include <string.h>
#include "Display.h"
#include "PXP.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_waveform.h"

static const uint32_t RATE = 44100;
static const uint64_t TOTAL = 6ull * 60ull * RATE;                 /* 15,876,000 frames */
static const uint32_t N0 = (uint32_t)(TOTAL / 128), N1 = N0 / 16, N2 = N1 / 16;
EXTMEM static int8_t lod0[N0 * 4], lod1[N1 * 4 + 4], lod2[N2 * 4 + 4];
EXTMEM static synthui_wave_beat_t beats[720], many_beats[5000];
static synthui_wave_table_t tables[3];
static lv_obj_t *wf;

/* ---- the platform the widget runs on here: SDRAM and the PXP ---- */
static void *wf_alloc(size_t n) { return extmem_malloc(n); }
static void wf_release(void *p) { extmem_free(p); }
static void wf_blit(const uint32_t *src, uint32_t src_pitch, uint32_t *dst, uint32_t dst_pitch, uint16_t w, uint16_t h)
{
    PXPSurface s((void *)src, w, h, PXP_XRGB8888, (uint16_t)src_pitch), d(dst, w, h, PXP_XRGB8888, (uint16_t)dst_pitch);
    if (PXP.op().source(s).output(d).alphaOut(0xFF).run() != PXP_OK) Serial1.println("wf_blit_err");
}

static void make_track(void)
{
    for (uint32_t i = 0; i < N0; i++) {
        const uint32_t f = i * 128u;
        const uint32_t ph = f % 22050u;                               /* a kick every half second */
        int a = 120 - (int)(ph / 90u); if (a < 14) a = 14;
        a = a * (int)(70u + ((f / 1323000u) % 4u) * 18u) / 124;       /* sections of 30 s */
        const int j = (int)((i * 2654435761u) >> 28);                 /* 0..15 */
        lod0[4 * i + 0] = (int8_t)-(a - j);      lod0[4 * i + 1] = (int8_t)(a - j / 2);
        lod0[4 * i + 2] = (int8_t)-(a * 3 / 4);  lod0[4 * i + 3] = (int8_t)(a * 3 / 4 - j);
    }
    const int8_t *src = lod0; int8_t *dsts[2] = { lod1, lod2 }; uint32_t n = N0;
    for (int l = 0; l < 2; l++) {
        const uint32_t m = n / 16; int8_t *d = dsts[l];
        for (uint32_t i = 0; i < m; i++) {
            int8_t v[4] = { 127, -128, 127, -128 };
            for (uint32_t k = 0; k < 16; k++) {
                const int8_t *p = src + 4 * (16 * i + k);
                if (p[0] < v[0]) v[0] = p[0];
                if (p[1] > v[1]) v[1] = p[1];
                if (p[2] < v[2]) v[2] = p[2];
                if (p[3] > v[3]) v[3] = p[3];
            }
            memcpy(d + 4 * i, v, 4);
        }
        src = d; n = m;
    }
    tables[0] = { lod0, N0, 128 }; tables[1] = { lod1, N1, 2048 }; tables[2] = { lod2, N2, 32768 };
    for (uint32_t i = 0; i < 720; i++) beats[i] = { i * 22050u, (uint8_t)((i % 4u) == 0u) };
    for (uint32_t i = 0; i < 5000; i++) many_beats[i] = { i * 3175u, (uint8_t)((i % 4u) == 0u) };
}

static uint32_t settle_and_sum(void)
{
    for (int pass = 0; pass < 3; pass++) {                 /* both scanout buffers, then one more */
        const uint32_t f = lvgl_mipi_panel_flips(), t = millis();
        lv_obj_invalidate(lv_screen_active());
        while (lvgl_mipi_panel_flips() == f && millis() - t < 3000) lvgl_rt1176_loop();
    }
    lvgl_mipi_panel_flip_sync();
    lvgl_sum_reset();
    lvgl_sum_feed(lvgl_mipi_panel_scanned_fb(), PANEL_FB_BYTES);
    return lvgl_sum_value();
}
static void state(const char *name)
{
    Serial1.printf("wf_crc[%s]=0x%08lX\n", name, (unsigned long)settle_and_sum());
#ifdef WF_HOLD            /* bench: stop on one state so the frame can be read back over SWD */
    if (strcmp(name, WF_HOLD) == 0) { Serial1.println("wf_hold"); for (;;) lvgl_rt1176_loop(); }
#endif
}
static void load_track(const synthui_wave_beat_t *b, uint32_t nb)
{
    synthui_waveform_set_tables(wf, tables, 3, 2, TOTAL, RATE);
    synthui_waveform_set_valid_frames(wf, TOTAL);
    synthui_waveform_set_beats(wf, b, nb);
    synthui_waveform_set_cue(wf, -1);
    synthui_waveform_set_tracking(wf, true);
}

void setup()
{
    Serial1.begin(115200);
    while (!Serial1 && millis() < 2000) {}
    Serial1.println("=== BOOT ===");
    Serial1.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n", "synthui_waveform_test", 1, __DATE__, __TIME__);
    if (!Display.begin() || !PXP.begin()) { Serial1.println("PANEL_FAIL"); return; }
    Serial1.println("PANEL_OK");
    make_track();

    lvgl_rt1176_begin();
    lvgl_mipi_panel_set_rot_threshold(0xFFFFFFFFu);
    lvgl_mipi_panel_create_rotated(Display, LVGL_PANEL_PRESENT_CW90);
    static const synthui_waveform_platform_t plat = { wf_alloc, wf_release, wf_blit };
    synthui_waveform_set_platform(&plat);
    lvgl_mipi_panel_set_preflip_cb(synthui_waveform_compose_into);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x141C26), 0);
    wf = synthui_waveform_create(scr);
    lv_obj_set_pos(wf, 16, 96);
    lv_obj_set_size(wf, 1120, 408);                       /* lane 336, gap 8, rail 64 */

    state("empty");
    load_track(beats, 720);
    synthui_waveform_zoom_fit(wf); synthui_waveform_set_position(wf, 0.0);
    state("fit");
    const double three_min = 3.0 * 60.0 * RATE;
    synthui_waveform_set_zoom(wf, 2048.0); synthui_waveform_set_position(wf, three_min);
    state("mid");
    synthui_waveform_set_zoom(wf, 128.0);
    state("close");
    synthui_waveform_zoom_fit(wf); synthui_waveform_set_valid_frames(wf, 60ull * RATE);
    synthui_waveform_set_position(wf, 30.0 * RATE);
    state("loading");
    synthui_waveform_set_valid_frames(wf, TOTAL);
    synthui_waveform_set_zoom(wf, 2048.0);
    synthui_waveform_set_position(wf, 60.0 * RATE);       /* tracking: view centred on 1 min ... */
    synthui_waveform_set_tracking(wf, false);             /* ... which the free view then keeps */
    synthui_waveform_set_position(wf, three_min);
    state("free");
    synthui_waveform_set_tracking(wf, true);
    synthui_waveform_set_cue(wf, (int64_t)three_min - RATE);
    state("cue");
    synthui_waveform_set_cue(wf, -1);
    synthui_waveform_set_position(wf, (double)(TOTAL - 1));
    state("end");
    load_track(many_beats, 5000);
    synthui_waveform_zoom_fit(wf); synthui_waveform_set_position(wf, 0.0);
    state("grid");
    Serial1.printf("wf_grid_lines=%lu\n", (unsigned long)synthui_waveform_grid_columns(wf));
    synthui_waveform_clear(wf);
    Serial1.printf("wf_crc_cleared=0x%08lX\n", (unsigned long)settle_and_sum());
    Serial1.println("crc_done");
}

void loop() { lvgl_rt1176_loop(); }
