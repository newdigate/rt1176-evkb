/* deck_math.h - the DJ deck's pure logic: pitch and rate, the BPM readout,
 * time text, the CDJ cue button, which files are audio and how they sort,
 * sidecar names.  No Arduino, no LVGL: host-tested in tests/deck_math_test.cpp.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ---- pitch ------------------------------------------------------------- */
/* Fader value 0..1 (1 = top = faster) to a pitch of -range..+range.  Within
 * 2 % of travel from the centre it is exactly 0: the fader has no detent. */
static inline float deck_pitch_from_fader(float v01, float range)
{
    if (!(v01 == v01)) return 0.0f;                 /* NaN */
    if (v01 < 0.0f) v01 = 0.0f;
    if (v01 > 1.0f) v01 = 1.0f;
    const float d = v01 - 0.5f;
    if (d > -0.02f && d < 0.02f) return 0.0f;
    return d * 2.0f * range;
}
static inline float deck_fader_from_pitch(float pitch, float range)
{
    float v = 0.5f + (range > 0.0f ? pitch / (2.0f * range) : 0.0f);
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
static inline float deck_rate_from_pitch(float pitch) { return 1.0f + pitch; }

/* Analysed tempo (Q16.16 BPM) at the current pitch, in hundredths of a BPM. */
static inline uint32_t deck_bpm_x100(uint32_t bpm_q16, float pitch)
{
    if (bpm_q16 == 0) return 0;
    const double bpm = (double)bpm_q16 / 65536.0 * (1.0 + (double)pitch);
    return (uint32_t)(bpm * 100.0 + 0.5);
}

/* ---- time text --------------------------------------------------------- */
/* "MM:SS.cc", or "-MM:SS.c" for time remaining; "--:--.--" with no track.
 * Clamped to 99:59.99; out[] is always NUL-terminated within 12 bytes. */
static inline void deck_time_text(char out[12], double frames, uint32_t rate, bool negative)
{
    if (rate == 0) { memcpy(out, "--:--.--", 9); return; }
    if (!(frames > 0.0)) frames = 0.0;
    uint64_t cs = (uint64_t)(frames * 100.0 / (double)rate);            /* centiseconds, truncated */
    if (cs > 599999u) cs = 599999u;
    const unsigned m = (unsigned)(cs / 6000u), s = (unsigned)(cs / 100u % 60u), c = (unsigned)(cs % 100u);
    char *p = out;
    if (negative) *p++ = '-';
    *p++ = (char)('0' + m / 10); *p++ = (char)('0' + m % 10); *p++ = ':';
    *p++ = (char)('0' + s / 10); *p++ = (char)('0' + s % 10); *p++ = '.';
    *p++ = (char)('0' + c / 10);
    if (!negative) *p++ = (char)('0' + c % 10);
    *p = 0;
}

/* ---- names ------------------------------------------------------------- */
static inline char deck_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static inline int deck_casecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        const int d = (unsigned char)deck_lower(*a) - (unsigned char)deck_lower(*b);
        if (d || !*a) return d;
    }
}
/* The formats this version loads.  Dot-files (including macOS "._" forks) and
 * the deck's own sidecars are not audio. */
static inline bool deck_is_audio_name(const char *name)
{
    if (!name || name[0] == '.' || name[0] == 0) return false;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return false;
    static const char *const ext[] = {"wav", "aif", "aiff", "flac", "mp3"};
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
        if (deck_casecmp(dot + 1, ext[i]) == 0) return true;
    return false;
}
/* Folders before files, then by name without regard to case. */
static inline int deck_name_compare(const char *a, bool a_dir, const char *b, bool b_dir)
{
    if (a_dir != b_dir) return a_dir ? -1 : 1;
    return deck_casecmp(a, b);
}
/* "<audio path>.<ext>".  If it does not fit, out is the empty string: a
 * shortened sidecar name would belong to some other file. */
static inline void deck_sidecar_name(char *out, size_t cap, const char *audio_path, const char *ext)
{
    const size_t a = strlen(audio_path), e = strlen(ext);
    if (cap == 0) return;
    if (a + 1 + e + 1 > cap) { out[0] = 0; return; }
    memcpy(out, audio_path, a);
    out[a] = '.';
    memcpy(out + a + 1, ext, e + 1);
}

/* ---- the cue button, as on a CDJ --------------------------------------- */
struct CueState {
    int64_t cue = 0;            /* frames */
    bool previewing = false;    /* held down on the cue: playing until release */
};
enum class CueAct { None, SetCue, JumpAndPause, PreviewStart, PreviewStop };

/* Press.  Playing: go back to the cue and pause.  Paused away from the cue:
 * the cue is now here.  Paused on the cue: play for as long as it is held.
 * The caller carries the action out (seek, pause, play, show the marker). */
static inline CueAct cue_press(CueState &s, bool playing, int64_t pos)
{
    if (s.previewing) return CueAct::None;
    if (playing) return CueAct::JumpAndPause;
    const int64_t d = pos - s.cue;
    if (d > 1 || d < -1) { s.cue = pos; return CueAct::SetCue; }
    s.previewing = true;
    return CueAct::PreviewStart;
}
/* Release.  After a preview: back to the cue, paused. */
static inline CueAct cue_release(CueState &s)
{
    if (!s.previewing) return CueAct::None;
    s.previewing = false;
    return CueAct::PreviewStop;
}
