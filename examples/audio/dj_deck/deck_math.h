/* deck_math.h - the DJ deck's pure logic: pitch and rate, the BPM readout,
 * time text, the CDJ cue button, which files are audio and how they sort,
 * sidecar names, and when to load the next track by itself.  No Arduino, no LVGL: host-tested in tests/deck_math_test.cpp.
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
/* Auto-next: is `cand` a better "file after `current`" than `best` ("" for
 * none yet)?  Offer every name in the folder; what is left in best is the
 * audio file that follows current in the list's order, or "" at the end. */
static inline bool deck_next_better(const char *current, const char *cand, const char *best)
{
    if (!cand || !deck_is_audio_name(cand)) return false;
    if (deck_casecmp(cand, current ? current : "") <= 0) return false;
    return !best || best[0] == 0 || deck_casecmp(cand, best) < 0;
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
    bool parked = false;        /* put on the cue by this button and not moved since */
};
enum class CueAct { None, SetCue, JumpAndPause, PreviewStart, PreviewStop };

/* Press.  Playing: go back to the cue and pause.  Paused away from the cue:
 * the cue is now here.  Paused on the cue: play for as long as it is held.
 * The caller carries the action out (seek, pause, play, show the marker).
 * "On the cue" is the parked flag, not only the position: a player that is
 * pausing still runs its ramp-down past the frame it was sent to. */
static inline CueAct cue_press(CueState &s, bool playing, int64_t pos)
{
    if (s.previewing) return CueAct::None;
    if (playing) { s.parked = true; return CueAct::JumpAndPause; }
    const int64_t d = pos - s.cue;
    if (!s.parked && (d > 1 || d < -1)) { s.cue = pos; s.parked = true; return CueAct::SetCue; }
    s.parked = true;
    s.previewing = true;
    return CueAct::PreviewStart;
}
/* The position was moved by something else (play, seek, scratch, rewind). */
static inline void cue_moved(CueState &s) { if (!s.previewing) s.parked = false; }
/* Release.  After a preview: back to the cue, paused. */
static inline CueAct cue_release(CueState &s)
{
    if (!s.previewing) return CueAct::None;
    s.previewing = false;
    return CueAct::PreviewStop;
}

/* ---- auto-next: load the following track when this one has played out --- */
struct AutoIn {
    bool playing = false;       /* the player is playing */
    bool scratching = false;    /* a finger holds the waveform */
    bool at_end = false;        /* the position is the last frame of a fully loaded track */
    bool near_end = false;      /* ... or within the last few seconds of one */
    bool loaded = false;        /* the loader has finished with the track that is playing, and holds nothing else */
    bool busy = false;          /* the loader is working (decode, analysis or saving) */
    bool failed = false;        /* the last load failed ... */
    bool no_memory = false;     /* ... for want of memory */
    bool playable = false;      /* the track on the deck is loaded far enough to play */
};
struct AutoNext {
    bool enabled = false;       /* the AUTO switch */
    bool was_playing = false;
    bool ended = false;         /* the track played out; waiting for the loader to be free */
    bool auto_load = false;     /* the load in hand was started from here, not by the user */
    bool want_play = false;     /* ... and is to be played as soon as it can be */
    bool bg = false;            /* the next track is loading, or loaded, beside the one that is playing */
    bool no_preload = false;    /* loading early was tried for this track and is not to be tried again */
};
enum class AutoAct { None, LoadNext, Play, Preload, DropPreload, Swap };

/* Once per main loop.  The caller carries the action out:
 *   LoadNext     find the file after the current one and load it, then call
 *                autonext_loading(), or autonext_none_left() if there is none.
 *   Play         start the player.
 *   Preload      near the end of a track: load the file after it (or after
 *                the early load that has just failed) while this one plays on,
 *                then autonext_preloading(), or autonext_no_preload() if
 *                there is none.
 *   DropPreload  the early load found no memory beside the playing track:
 *                cancel it.  LoadNext follows when the track has ended.
 *   Swap         the track has played out and the next is waiting: put it on
 *                the deck.  Play follows as soon as it can be played.
 * A track has played out when the player stops by itself on its last frame;
 * a pause, a seek or a scratch to the end is not that.  LoadNext waits until
 * the loader has finished with the current track, so its analysis files are
 * saved first.  A load started from here that fails asks for the file after
 * it; one the user picked stays failed. */
static inline AutoAct autonext_tick(AutoNext &s, const AutoIn &in)
{
    if (!s.enabled) { s.ended = s.auto_load = s.want_play = false; }

    if (s.bg && in.failed && s.enabled) {
        if (in.no_memory) { s.bg = false; s.no_preload = true; return AutoAct::DropPreload; }
        return AutoAct::Preload;
    }
    if (!s.bg) {
        if (s.auto_load && in.failed) return AutoAct::LoadNext;
        if (s.want_play && in.playable && !in.failed) { s.want_play = false; s.was_playing = true; return AutoAct::Play; }
    }

    if (in.scratching) s.ended = false;
    else if (s.enabled && s.was_playing && !in.playing && in.at_end) s.ended = true;
    if (s.ended && (in.playing || !in.at_end)) s.ended = false;
    s.was_playing = in.playing && !in.scratching;
    if (s.ended && s.bg) {
        s.ended = false; s.bg = false; s.no_preload = false;
        s.auto_load = s.want_play = true;
        return AutoAct::Swap;
    }
    if (s.ended && !in.busy) { s.ended = false; return AutoAct::LoadNext; }
    if (s.enabled && !s.bg && !s.no_preload && in.near_end && in.playing && !in.scratching && in.loaded) return AutoAct::Preload;
    return AutoAct::None;
}
/* The caller started the automatic load. */
static inline void autonext_loading(AutoNext &s) { s.auto_load = true; s.want_play = true; s.ended = false; s.was_playing = false; s.bg = false; s.no_preload = false; }
/* There was no file to load. */
static inline void autonext_none_left(AutoNext &s) { s.auto_load = s.want_play = s.ended = false; }
/* The caller started the early load. */
static inline void autonext_preloading(AutoNext &s) { s.bg = true; }
/* There was no file to load early: do not look again for this track. */
static inline void autonext_no_preload(AutoNext &s) { s.bg = false; s.no_preload = true; }
/* The user picked a file. */
static inline void autonext_manual(AutoNext &s) { s.auto_load = s.want_play = s.ended = s.bg = s.no_preload = false; s.was_playing = false; }
