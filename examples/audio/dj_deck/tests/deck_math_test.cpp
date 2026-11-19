// Host test for deck_math.h: pitch, BPM, time text, the CDJ cue machine, file names.
#undef NDEBUG
#include "deck_math.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

int main() {
    // --- pitch fader: 0..1, up is faster, snap near the centre ---
    assert(near(deck_pitch_from_fader(0.5f, 0.08f), 0.0f));
    assert(near(deck_pitch_from_fader(0.51f, 0.08f), 0.0f));          // inside the snap zone
    assert(near(deck_pitch_from_fader(0.489f, 0.08f), 0.0f));
    assert(near(deck_pitch_from_fader(1.0f, 0.08f), 0.08f));
    assert(near(deck_pitch_from_fader(0.0f, 0.16f), -0.16f));
    assert(near(deck_pitch_from_fader(0.75f, 0.08f), 0.04f));
    assert(near(deck_pitch_from_fader(0.25f, 0.08f), -0.04f));
    assert(near(deck_pitch_from_fader(7.0f, 0.08f), 0.08f) && near(deck_pitch_from_fader(-3.0f, 0.08f), -0.08f));
    assert(near(deck_pitch_from_fader(NAN, 0.08f), 0.0f));
    assert(near(deck_rate_from_pitch(0.04f), 1.04f) && near(deck_rate_from_pitch(-0.16f), 0.84f));
    // the fader position that shows a given pitch (for the range switch and the reset)
    assert(near(deck_fader_from_pitch(0.04f, 0.08f), 0.75f) && near(deck_fader_from_pitch(0.04f, 0.16f), 0.625f));
    assert(near(deck_fader_from_pitch(0.5f, 0.08f), 1.0f) && near(deck_fader_from_pitch(0.0f, 0.08f), 0.5f));

    // --- BPM readout: analysed tempo (Q16.16) times the pitch ---
    assert(deck_bpm_x100((uint32_t)(128.4 * 65536.0), 0.024f) == 13148);
    assert(deck_bpm_x100((uint32_t)(120.0 * 65536.0), 0.0f) == 12000);
    assert(deck_bpm_x100(0, 0.05f) == 0);

    // --- time text ---
    char t[12];
    deck_time_text(t, 3414222.0, 44100, false); assert(!strcmp(t, "01:17.42"));
    deck_time_text(t, 11117610.0, 44100, true); assert(!strcmp(t, "-04:12.1"));
    deck_time_text(t, 0.0, 44100, false);       assert(!strcmp(t, "00:00.00"));
    deck_time_text(t, 0.0, 44100, true);        assert(!strcmp(t, "-00:00.0"));
    deck_time_text(t, 48000.0 * 61.5, 48000, false); assert(!strcmp(t, "01:01.50"));
    deck_time_text(t, 44100.0 * 6000.0, 44100, false); assert(!strcmp(t, "99:59.99"));     // clamped, never overruns
    deck_time_text(t, -5.0, 44100, false);      assert(!strcmp(t, "00:00.00"));
    deck_time_text(t, 1000.0, 0, false);        assert(!strcmp(t, "--:--.--"));           // no track

    // --- which names are audio files ---
    assert(deck_is_audio_name("a.WAV") && deck_is_audio_name("b.Mp3") && deck_is_audio_name("c.aiff") &&
           deck_is_audio_name("d.aif") && deck_is_audio_name("e.FLAC") && deck_is_audio_name("take 04.wav"));
    assert(!deck_is_audio_name("a.wav.apv") && !deck_is_audio_name("a.wav.asv") && !deck_is_audio_name("a.wav.att"));
    assert(!deck_is_audio_name(".hidden.wav") && !deck_is_audio_name("._take.wav"));
    assert(!deck_is_audio_name("notes.txt") && !deck_is_audio_name("x.ogg") && !deck_is_audio_name("x.m4a"));
    assert(!deck_is_audio_name("wav") && !deck_is_audio_name(".wav") && !deck_is_audio_name("") && !deck_is_audio_name(nullptr));
    assert(!deck_is_audio_name("song.mp3x") && !deck_is_audio_name("song.wa"));

    // --- list order: folders first, then names without regard to case ---
    assert(deck_name_compare("zeta", true, "alpha.wav", false) < 0);
    assert(deck_name_compare("alpha.wav", false, "zeta", true) > 0);
    assert(deck_name_compare("b.wav", false, "C.wav", false) < 0);
    assert(deck_name_compare("B.wav", false, "a.wav", false) > 0);
    assert(deck_name_compare("Sub", true, "sets", true) > 0);
    assert(deck_name_compare("same.wav", false, "same.wav", false) == 0);

    // --- sidecar names ---
    char p[64];
    deck_sidecar_name(p, sizeof p, "/music/take 04.wav", "att"); assert(!strcmp(p, "/music/take 04.wav.att"));
    deck_sidecar_name(p, sizeof p, "a.mp3", "apv");              assert(!strcmp(p, "a.mp3.apv"));
    deck_sidecar_name(p, 12, "/music/take 04.wav", "att");       assert(p[0] == 0);       // would not fit: empty, never cut short
    deck_sidecar_name(p, 10, "a.mp3", "apv");                    assert(!strcmp(p, "a.mp3.apv"));   // exactly fits with its NUL
    deck_sidecar_name(p, 9, "a.mp3", "apv");                     assert(p[0] == 0);

    // --- the CDJ cue button ---
    CueState c;                                              // cue at 0
    assert(cue_press(c, false, 0) == CueAct::PreviewStart && c.previewing);   // paused on the cue: hold to play
    assert(cue_press(c, true, 500) == CueAct::None);         // a second press while previewing is ignored
    assert(cue_release(c) == CueAct::PreviewStop && !c.previewing);
    assert(cue_release(c) == CueAct::None);
    cue_moved(c);                                            // the user seeks away
    assert(cue_press(c, false, 44100) == CueAct::SetCue && c.cue == 44100 && !c.previewing);   // paused elsewhere: set it
    assert(cue_release(c) == CueAct::None);
    assert(cue_press(c, false, 44100) == CueAct::PreviewStart);             // now on it
    assert(cue_release(c) == CueAct::PreviewStop);
    assert(cue_press(c, false, 44101) == CueAct::PreviewStart);             // within a frame counts as on it
    assert(cue_release(c) == CueAct::PreviewStop);
    assert(cue_press(c, true, 99999) == CueAct::JumpAndPause && c.cue == 44100);   // playing: back to the cue
    assert(cue_release(c) == CueAct::None);
    // parked on the cue: the player's ramp-down leaves it a few frames past (64 seen), which must still count as "on it"
    assert(cue_press(c, false, 44100 + 64) == CueAct::PreviewStart);
    assert(cue_release(c) == CueAct::PreviewStop);
    assert(cue_press(c, false, 44100 + 64) == CueAct::PreviewStart);        // and again after a preview
    assert(cue_release(c) == CueAct::PreviewStop);
    cue_moved(c);                                                            // play, seek, scratch or rewind: no longer parked
    assert(cue_press(c, false, 44100 + 64) == CueAct::SetCue && c.cue == 44164);
    assert(cue_press(c, false, 44164 + 64) == CueAct::PreviewStart);        // a cue just set is parked too
    assert(cue_release(c) == CueAct::PreviewStop);
    c.cue = 44100; cue_moved(c);
    c = CueState();
    assert(c.cue == 0 && !c.previewing);

    std::printf("deck_math_test: all assertions passed\n");
    return 0;
}
