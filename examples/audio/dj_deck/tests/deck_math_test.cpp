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

    // --- auto-next: loading the next track early, beside the one that is playing ---
    {
        // near: playing in the last seconds; loaded: the loader has finished with the track that is playing
        auto in = [](bool playing, bool near, bool at_end, bool loaded, bool busy = false, bool failed = false, bool no_memory = false, bool playable = false) {
            AutoIn i; i.playing = playing; i.near_end = near; i.at_end = at_end; i.loaded = loaded; i.busy = busy; i.failed = failed;
            i.no_memory = no_memory; i.playable = playable; return i;
        };
        AutoNext a; a.enabled = true;
        assert(autonext_tick(a, in(true, false, false, true)) == AutoAct::None);          // not near the end yet
        assert(autonext_tick(a, in(true, true, false, true)) == AutoAct::Preload);
        autonext_preloading(a);
        assert(autonext_tick(a, in(true, true, false, false, true)) == AutoAct::None);    // one at a time
        assert(autonext_tick(a, in(true, true, false, false, false)) == AutoAct::None);   // it is ready and waits
        // the track plays out: the waiting one comes forward and plays, with no load in between
        assert(autonext_tick(a, in(false, true, true, false, false)) == AutoAct::Swap);
        assert(autonext_tick(a, in(false, false, false, true, false, false, false, true)) == AutoAct::Play);
        assert(autonext_tick(a, in(true, false, false, true)) == AutoAct::None);
        // ... and near ITS end the one after it is loaded early in turn
        assert(autonext_tick(a, in(true, true, false, true)) == AutoAct::Preload);

        // the swap does not wait for the early load to finish: it may still be decoding
        a = AutoNext(); a.enabled = true;
        autonext_tick(a, in(true, true, false, true)); autonext_preloading(a);
        assert(autonext_tick(a, in(false, true, true, false, true)) == AutoAct::Swap);
        assert(autonext_tick(a, in(false, false, false, false, true, false, false, false)) == AutoAct::None);   // not playable yet
        assert(autonext_tick(a, in(false, false, false, false, true, false, false, true)) == AutoAct::Play);

        // not while the current track is still loading or being analysed, not paused, not when switched off
        a = AutoNext(); a.enabled = true;
        assert(autonext_tick(a, in(true, true, false, false, true)) == AutoAct::None);
        assert(autonext_tick(a, in(false, true, false, true)) == AutoAct::None);
        a.enabled = false;
        assert(autonext_tick(a, in(true, true, false, true)) == AutoAct::None);

        // no room for both tracks: give the early load up, and load the ordinary way when the track has ended
        a = AutoNext(); a.enabled = true;
        autonext_tick(a, in(true, true, false, true)); autonext_preloading(a);
        assert(autonext_tick(a, in(true, true, false, false, false, true, true)) == AutoAct::DropPreload);
        assert(autonext_tick(a, in(true, true, false, false)) == AutoAct::None);         // the caller cancelled it: loader idle, nothing kept
        assert(autonext_tick(a, in(true, true, false, false)) == AutoAct::None);         // and it is not tried again for this track
        assert(autonext_tick(a, in(false, true, true, false)) == AutoAct::LoadNext);
        autonext_loading(a);
        assert(autonext_tick(a, in(false, false, false, false, true, false, false, true)) == AutoAct::Play);
        assert(autonext_tick(a, in(true, true, false, true)) == AutoAct::Preload);       // the next track gets its own try

        // an early load that fails for any other reason: skip that file and load the one after it early
        a = AutoNext(); a.enabled = true;
        autonext_tick(a, in(true, true, false, true)); autonext_preloading(a);
        assert(autonext_tick(a, in(true, true, false, false, false, true, false)) == AutoAct::Preload);
        autonext_preloading(a);
        assert(autonext_tick(a, in(true, true, false, false, true)) == AutoAct::None);
        // ... or, if nothing follows it, stop trying
        assert(autonext_tick(a, in(true, true, false, false, false, true, false)) == AutoAct::Preload);
        autonext_no_preload(a);
        assert(autonext_tick(a, in(true, true, false, false)) == AutoAct::None);
        assert(autonext_tick(a, in(true, true, false, true)) == AutoAct::None);

        // the last track of the folder: one look for a next file, not one every loop
        a = AutoNext(); a.enabled = true;
        assert(autonext_tick(a, in(true, true, false, true)) == AutoAct::Preload);
        autonext_no_preload(a);
        assert(autonext_tick(a, in(true, true, false, true)) == AutoAct::None);
        assert(autonext_tick(a, in(false, true, true, true)) == AutoAct::LoadNext);       // the ordinary path then reports the end
        autonext_none_left(a);
        assert(autonext_tick(a, in(false, true, true, true)) == AutoAct::None);

        // the user picks a file while one is waiting: everything automatic is forgotten
        a = AutoNext(); a.enabled = true;
        autonext_tick(a, in(true, true, false, true)); autonext_preloading(a);
        autonext_manual(a);
        assert(!a.bg && autonext_tick(a, in(false, false, false, false, true)) == AutoAct::None);
        // switched off with one waiting: the track just ends; switched on again, the waiting one is still used
        a = AutoNext(); a.enabled = true;
        autonext_tick(a, in(true, true, false, true)); autonext_preloading(a);
        a.enabled = false;
        assert(autonext_tick(a, in(false, true, true, false)) == AutoAct::None);
        a.enabled = true;
        assert(autonext_tick(a, in(true, true, false, false)) == AutoAct::None);
        assert(autonext_tick(a, in(false, true, true, false)) == AutoAct::Swap);
        // switched off while the early load fails: nothing is skipped or retried behind the user's back
        a = AutoNext(); a.enabled = true;
        autonext_tick(a, in(true, true, false, true)); autonext_preloading(a);
        a.enabled = false;
        assert(autonext_tick(a, in(true, true, false, false, false, true, false)) == AutoAct::None);
        assert(autonext_tick(a, in(true, true, false, false, false, true, true)) == AutoAct::None);
        // a scratch to the end with one waiting is not the end
        a = AutoNext(); a.enabled = true;
        autonext_tick(a, in(true, true, false, true)); autonext_preloading(a);
        { AutoIn i = in(false, true, true, false); i.scratching = true; assert(autonext_tick(a, i) == AutoAct::None); }
        assert(autonext_tick(a, in(false, true, true, false)) == AutoAct::None);
    }

    std::printf("deck_math_test: all assertions passed\n");
    // --- auto-next: which file follows the current one in the browser's order ---
    {
        const char *folder[] = {"b.wav", "A.wav", "notes.txt", "c.mp3", ".hidden.wav", "B2.flac", "a.wav.apv", "d.aiff"};
        auto next_after = [&](const char *cur) {
            static char best[64];
            best[0] = 0;
            for (const char *n : folder) if (deck_next_better(cur, n, best)) snprintf(best, sizeof best, "%s", n);
            return (const char *)best;
        };
        assert(!strcmp(next_after("A.wav"), "b.wav"));       // case does not matter, as in the list
        assert(!strcmp(next_after("b.wav"), "B2.flac"));
        assert(!strcmp(next_after("B2.flac"), "c.mp3"));     // notes.txt and the sidecar are not audio
        assert(!strcmp(next_after("c.mp3"), "d.aiff"));
        assert(!strcmp(next_after("d.aiff"), ""));           // the last file: nothing follows, no wrap
        assert(!strcmp(next_after("bb.wav"), "c.mp3"));      // the current file need not exist any more
        assert(!strcmp(next_after(""), "A.wav"));
        assert(!deck_next_better("a.wav", "a.wav", ""));     // never the same file again
        assert(!deck_next_better("a.wav", nullptr, "") && !deck_next_better("a.wav", "", ""));
    }

    // --- auto-next: when to load the next track ---
    {
        // AutoIn: playing, scratching, at_end, loader busy, loader failed, track playable
        AutoNext a;
        auto in = [](bool playing, bool at_end, bool busy = false, bool failed = false, bool playable = true, bool scratching = false) {
            AutoIn i; i.playing = playing; i.at_end = at_end; i.busy = busy; i.failed = failed; i.playable = playable; i.scratching = scratching; return i;
        };
        // off: a track that plays to its end just stops
        assert(autonext_tick(a, in(true, false)) == AutoAct::None);
        assert(autonext_tick(a, in(false, true)) == AutoAct::None);
        // on: the same thing asks for the next track, once
        a.enabled = true;
        assert(autonext_tick(a, in(true, false)) == AutoAct::None);
        assert(autonext_tick(a, in(false, true)) == AutoAct::LoadNext);
        assert(autonext_tick(a, in(false, true)) == AutoAct::None);
        // the caller started the load: play as soon as it can be played
        autonext_loading(a);
        assert(autonext_tick(a, in(false, false, true, false, false)) == AutoAct::None);
        assert(autonext_tick(a, in(false, false, true, false, true)) == AutoAct::Play);
        assert(autonext_tick(a, in(true, false, true)) == AutoAct::None);
        // a pause is not an end, and neither is a pause near the end
        assert(autonext_tick(a, in(false, false)) == AutoAct::None);
        // an automatic load that fails moves on to the file after it
        a = AutoNext(); a.enabled = true;
        autonext_tick(a, in(true, false));
        assert(autonext_tick(a, in(false, true)) == AutoAct::LoadNext);
        autonext_loading(a);
        assert(autonext_tick(a, in(false, false, false, true, false)) == AutoAct::LoadNext);
        autonext_loading(a);
        assert(autonext_tick(a, in(false, false, false, true, false)) == AutoAct::LoadNext);     // and again
        autonext_none_left(a);                                                                  // the folder has no more
        assert(autonext_tick(a, in(false, false, false, true, false)) == AutoAct::None);
        // a file the user picked that fails stays failed
        a = AutoNext(); a.enabled = true;
        autonext_manual(a);
        assert(autonext_tick(a, in(false, false, false, true, false)) == AutoAct::None);
        // a manual load cancels a pending automatic play
        a = AutoNext(); a.enabled = true;
        autonext_tick(a, in(true, false)); autonext_tick(a, in(false, true)); autonext_loading(a);
        autonext_manual(a);
        assert(autonext_tick(a, in(false, false, true, false, true)) == AutoAct::None);
        // the end arrives while the analysis is still being saved: wait for the loader
        a = AutoNext(); a.enabled = true;
        autonext_tick(a, in(true, false, true));
        assert(autonext_tick(a, in(false, true, true)) == AutoAct::None);
        assert(autonext_tick(a, in(false, true, true)) == AutoAct::None);
        assert(autonext_tick(a, in(false, true, false)) == AutoAct::LoadNext);
        // ... unless the user moved away from the end or started playing again meanwhile
        a = AutoNext(); a.enabled = true;
        autonext_tick(a, in(true, false, true)); autonext_tick(a, in(false, true, true));
        assert(autonext_tick(a, in(false, false, true)) == AutoAct::None);
        assert(autonext_tick(a, in(false, false, false)) == AutoAct::None);
        // a scratch that is dragged to the end is not the track finishing
        a = AutoNext(); a.enabled = true;
        autonext_tick(a, in(true, false, false, false, true, true));
        assert(autonext_tick(a, in(false, true, false, false, true, true)) == AutoAct::None);
        assert(autonext_tick(a, in(false, true)) == AutoAct::None);
        // switched off after the end was seen but before the loader was free: nothing happens
        a = AutoNext(); a.enabled = true;
        autonext_tick(a, in(true, false, true)); autonext_tick(a, in(false, true, true));
        a.enabled = false;
        assert(autonext_tick(a, in(false, true, false)) == AutoAct::None);
        // switched off while an automatic load is in flight: it is not played and a failure is not skipped
        a = AutoNext(); a.enabled = true;
        autonext_tick(a, in(true, false)); autonext_tick(a, in(false, true)); autonext_loading(a);
        a.enabled = false;
        assert(autonext_tick(a, in(false, false, false, true, false)) == AutoAct::None);
    }

    return 0;
}
