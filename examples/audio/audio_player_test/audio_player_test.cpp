/* audio_player_test - AudioPlayer (Audio/play_buffer.*) on the RT1176.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * AudioPlayer plays a buffer in SDRAM into two record queues (checked here)
 * and into the I2S output (so a board run is audible on line out).
 *
 * Source, 65536 stereo frames:  LEFT is a ramp, L[i] = i - 32768, so a
 * recorded left sample names the frame it came from and the difference
 * between neighbours is the playback rate;  RIGHT is a 441 Hz sine.
 *
 * Stages (one STAGE_x=PASS|FAIL line each, with the measured numbers):
 *   UNITY    rate 1: left steps by exactly 1; right equals the source sample
 *   HALF     rate 0.5: left advances half a frame per sample
 *   REVERSE  rate -1: left steps by exactly -1
 *   RAMP     1 -> 2 changes over one block, never jumps
 *   SCRATCH  hold a target 2000 frames back: arrives, output goes constant,
 *            playback resumes at rate 1 on scratchEnd
 *   STARVE   setAvailableFrames(3000): holds below it in silence, still
 *            "playing"; resumes when the limit is lifted
 *   POLL     200,000 positionFrames() reads from the main loop while playing:
 *            never backwards, never a jump (a torn 64-bit read would be one)
 */
#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"
#include "play_buffer.h"
#include "output_i2s.h"
#include "record_queue.h"
#include <math.h>

#if defined(ARDUINO_NOSH)
#include "control_sgtl5000.h"
#define BOARD_CODEC_T AudioControlSGTL5000
#else
#include "control_wm8962.h"
#define BOARD_CODEC_T AudioControlWM8962
#endif
#define CONSOLE Serial1

static const uint32_t FRAMES = 65536;
EXTMEM static int16_t pcm[FRAMES * 2];

AudioPlayer       player;
AudioRecordQueue  qL, qR;
AudioOutputI2S    out;
AudioConnection   c1(player, 0, qL, 0), c2(player, 1, qR, 0);
AudioConnection   c3(player, 0, out, 0), c4(player, 1, out, 1);
BOARD_CODEC_T     codec;

static const int MAXBLK = 40;
static int16_t recL[MAXBLK * AUDIO_BLOCK_SAMPLES], recR[MAXBLK * AUDIO_BLOCK_SAMPLES];
static bool all_ok = true;

/* Record `blocks` consecutive blocks of both outputs.  Returns samples, 0 on timeout. */
static int capture(int blocks) {
    qL.clear(); qR.clear();
    qL.begin(); qR.begin();
    int got = 0;
    const uint32_t t0 = millis();
    while (got < blocks && millis() - t0 < 4000) {
        if (qL.available() > 0 && qR.available() > 0) {
            memcpy(recL + got * AUDIO_BLOCK_SAMPLES, qL.readBuffer(), AUDIO_BLOCK_SAMPLES * 2);
            memcpy(recR + got * AUDIO_BLOCK_SAMPLES, qR.readBuffer(), AUDIO_BLOCK_SAMPLES * 2);
            qL.freeBuffer(); qR.freeBuffer();
            got++;
        }
    }
    qL.end(); qR.end();
    return got == blocks ? blocks * AUDIO_BLOCK_SAMPLES : 0;
}
static void verdict(const char *name, bool ok) {
    CONSOLE.printf("STAGE_%s=%s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) all_ok = false;
}
/* min and max of left[i] - left[i-1] over n samples */
static void diffs(int n, int &mn, int &mx) {
    mn = 100000; mx = -100000;
    for (int i = 1; i < n; i++) { int d = recL[i] - recL[i - 1]; if (d < mn) mn = d; if (d > mx) mx = d; }
}
static void wait_ms(uint32_t ms) { const uint32_t t0 = millis(); while (millis() - t0 < ms) yield(); }

void setup() {
    CONSOLE.begin(115200);
    while (!CONSOLE && millis() < 2000) {}
    CONSOLE.println("=== BOOT ===");
    CONSOLE.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n", "audio_player_test", 1, __DATE__, __TIME__);

    for (uint32_t i = 0; i < FRAMES; i++) {
        pcm[2 * i] = (int16_t)((int32_t)i - 32768);
        pcm[2 * i + 1] = (int16_t)lroundf(20000.0f * sinf(2.0f * 3.14159265f * 441.0f * (float)i / 44100.0f));
    }
    AudioMemory(120);
    codec.enable();
#if defined(ARDUINO_NOSH)
    codec.lineOutLevel(29);          // quiet: full scale 1.29 Vpp
#endif
    player.setSource(pcm, FRAMES, 2, 44100.0f);
    int n, mn, mx;

    /* UNITY */
    player.seek(4096); player.setRate(1.0f); player.play();
    wait_ms(50);
    n = capture(MAXBLK);
    diffs(n, mn, mx);
    int bad_r = 0;
    for (int i = 0; i < n; i++) { uint32_t f = (uint32_t)(recL[i] + 32768); if (recR[i] != pcm[2 * f + 1]) bad_r++; }
    CONSOLE.printf("info unity n=%d diff_min=%d diff_max=%d right_mismatch=%d\n", n, mn, mx, bad_r);
    verdict("UNITY", n > 0 && mn == 1 && mx == 1 && bad_r == 0);

    /* HALF */
    player.seek(4096); player.setRate(0.5f);
    wait_ms(50);
    n = capture(MAXBLK);
    diffs(n, mn, mx);
    const int adv_half = n ? recL[n - 1] - recL[0] : 0;
    CONSOLE.printf("info half n=%d advance=%d expect=%d diff_min=%d diff_max=%d\n", n, adv_half, (n - 1) / 2, mn, mx);
    verdict("HALF", n > 0 && abs(adv_half - (n - 1) / 2) <= 2 && mn >= 0 && mx <= 1);

    /* REVERSE */
    player.seek(60000); player.setRate(-1.0f);
    wait_ms(50);
    n = capture(MAXBLK);
    diffs(n, mn, mx);
    CONSOLE.printf("info reverse n=%d diff_min=%d diff_max=%d\n", n, mn, mx);
    verdict("REVERSE", n > 0 && mn == -1 && mx == -1);

    /* RAMP: 1 -> 2 while recording */
    player.seek(4096); player.setRate(1.0f);
    wait_ms(50);
    qL.clear(); qR.clear(); qL.begin(); qR.begin();
    wait_ms(30);
    player.setRate(2.0f);
    wait_ms(60);
    qL.end(); qR.end();
    n = 0;
    while (qL.available() > 0 && n < MAXBLK * AUDIO_BLOCK_SAMPLES) {
        memcpy(recL + n, qL.readBuffer(), AUDIO_BLOCK_SAMPLES * 2); qL.freeBuffer(); n += AUDIO_BLOCK_SAMPLES;
    }
    qR.clear();
    int first2 = -1, last1 = -1;
    diffs(n, mn, mx);
    for (int i = 1; i < n; i++) { int d = recL[i] - recL[i - 1]; if (d == 2 && first2 < 0) first2 = i; if (d == 1) last1 = i; }
    const int span = (first2 >= 0 && last1 >= 0) ? last1 - first2 : -1;
    CONSOLE.printf("info ramp n=%d diff_min=%d diff_max=%d first2=%d last1=%d span=%d\n", n, mn, mx, first2, last1, span);
    verdict("RAMP", n >= 4 * AUDIO_BLOCK_SAMPLES && mn == 1 && mx == 2 && first2 > 0 && span >= 16 && span <= AUDIO_BLOCK_SAMPLES);

    /* SCRATCH: hold a target 2000 frames behind */
    player.seek(30000); player.setRate(1.0f);
    wait_ms(50);
    const double target = player.positionFrames() - 2000.0;
    player.scratchBegin();
    for (int i = 0; i < 30; i++) { player.scratchTo(target); wait_ms(10); }      /* 300 ms */
    const double arrived = player.positionFrames();
    qL.clear(); qR.clear(); qL.begin(); qR.begin();
    for (int i = 0; i < 6; i++) { player.scratchTo(target); wait_ms(10); }
    qL.end(); qR.end();
    n = 0;
    while (qL.available() > 0 && n < MAXBLK * AUDIO_BLOCK_SAMPLES) {
        memcpy(recL + n, qL.readBuffer(), AUDIO_BLOCK_SAMPLES * 2); qL.freeBuffer(); n += AUDIO_BLOCK_SAMPLES;
    }
    qR.clear();
    diffs(n, mn, mx);
    const long err_milli = lround((arrived - target) * 1000.0);
    player.scratchEnd();
    wait_ms(50);
    int n2 = capture(8), mn2, mx2;
    diffs(n2, mn2, mx2);
    CONSOLE.printf("info scratch err_milliframes=%ld held_n=%d held_diff_min=%d held_diff_max=%d resume_diff_min=%d resume_diff_max=%d\n",
                   err_milli, n, mn, mx, mn2, mx2);
    verdict("SCRATCH", labs(err_milli) < 1000 && n >= 2 * AUDIO_BLOCK_SAMPLES && mn >= -1 && mx <= 1
                       && !player.isScratching() && n2 > 0 && mn2 == 1 && mx2 == 1);

    /* STARVE */
    player.pause(); wait_ms(20);
    player.seek(1000); player.setAvailableFrames(3000); player.setRate(1.0f); player.play();
    wait_ms(300);
    const long held = lround(player.positionFrames());
    const bool still = player.isPlaying();
    n = capture(4);
    int nz = 0;
    for (int i = 0; i < n; i++) if (recL[i] != 0 || recR[i] != 0) nz++;
    player.setAvailableFrames(FRAMES);
    wait_ms(100);
    const long after = lround(player.positionFrames());
    CONSOLE.printf("info starve held_at=%ld playing=%d nonzero=%d of %d after=%ld\n", held, (int)still, nz, n, after);
    verdict("STARVE", held <= 2999 && held >= 2990 && still && n > 0 && nz == 0 && after > 3000);

    /* POLL */
    player.seek(4096);
    wait_ms(20);
    double prev = player.positionFrames();
    long back = 0, jump = 0, moved = 0;
    for (long i = 0; i < 200000; i++) {
        const double p = player.positionFrames();
        const double d = p - prev;
        if (d < 0) back++;
        if (d > 2.0 * AUDIO_BLOCK_SAMPLES) jump++;
        if (d > 0) moved++;
        prev = p;
    }
    CONSOLE.printf("info poll backwards=%ld jumps=%ld steps=%ld\n", back, jump, moved);
    verdict("POLL", back == 0 && jump == 0 && moved > 0);

    player.pause();
    CONSOLE.println(all_ok ? "AUDIO_PLAYER_ALL=PASS" : "AUDIO_PLAYER_ALL=FAIL");
}

void loop() {}
