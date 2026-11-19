// Host test for the Loader: progressive decode, sidecar reuse / rebuild / save,
// failures, cancellation.  Storage is a temp directory; memory is malloc with
// a live-allocation count; time is a fake clock the test advances.
#undef NDEBUG
#include "audio_codecs/tempo/tempo_types.h"
#include <cstring>
#include "loader.h"
#include "probe_decode.h"
#include "audio_codecs/preview/preview_generator.h"
#include "audio_codecs/spectrum/spectrum_generator.h"
#include "audio_codecs/spectrum/desktop_fft.h"
#include "audio_codecs/tempo/tempo_generator.h"
#include "audio_codecs/tempo/tempo_reader.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

using namespace audio_codecs;
using preview::SeekableReader; using preview::SeekableWriter;

// ---- storage over a directory ----
struct CountingReader : preview::FileStreamReader {
    std::FILE *fp; uint64_t *budget;
    CountingReader(std::FILE *f, uint64_t *b) : preview::FileStreamReader(f), fp(f), budget(b) {}
    size_t read(uint8_t *d, size_t n) override {
        if (budget) { if (*budget == 0) return 0; if (n > *budget) n = (size_t)*budget; *budget -= n; }
        return preview::FileStreamReader::read(d, n);
    }
};
struct DirStorage : Storage {
    std::string dir; int writes = 0; bool read_only = false; uint64_t read_budget = UINT64_MAX; bool limit_reads = false;
    std::string full(const char *p) { return dir + p; }
    SeekableReader *openRead(const char *path) override {
        std::FILE *fp = std::fopen(full(path).c_str(), "rb");
        return fp ? new CountingReader(fp, limit_reads ? &read_budget : nullptr) : nullptr;
    }
    SeekableWriter *openWrite(const char *path) override {
        if (read_only) return nullptr;
        std::FILE *fp = std::fopen(full(path).c_str(), "w+b");
        if (!fp) return nullptr;
        writes++;
        return new preview::FileStreamWriter(fp);
    }
    void close(SeekableReader *r) override { if (!r) return; CountingReader *c = static_cast<CountingReader *>(r); std::fclose(c->fp); delete c; }
    void close(SeekableWriter *w) override { if (!w) return; fps_close(w); }
    static void fps_close(SeekableWriter *w) { w->flush(); delete static_cast<preview::FileStreamWriter *>(w); }
};
static int g_live = 0;
static bool g_no_mem = false;
static void *t_alloc(size_t n) { if (g_no_mem) return nullptr; g_live++; return std::malloc(n); }
static void t_release(void *p) { if (p) { g_live--; std::free(p); } }
static uint32_t g_clock = 0, g_units = 0, g_tick = 1500;     // each look at the clock costs 1.5 ms: a few units per step()
static uint32_t t_now() { g_units++; g_clock += g_tick; return g_clock; }

static std::vector<uint8_t> slurp(const std::string &p) {
    std::vector<uint8_t> d; std::FILE *fp = std::fopen(p.c_str(), "rb"); if (!fp) return d;
    uint8_t b[65536]; size_t n; while ((n = std::fread(b, 1, sizeof b, fp)) > 0) d.insert(d.end(), b, b + n);
    std::fclose(fp); return d;
}
static void spit(const std::string &p, const std::vector<uint8_t> &d) { std::FILE *fp = std::fopen(p.c_str(), "wb"); assert(fp); std::fwrite(d.data(), 1, d.size(), fp); std::fclose(fp); }
static bool exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }
static uint32_t fnv(const void *p, size_t n) { uint32_t h = 2166136261u; const uint8_t *b = (const uint8_t *)p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u; return h; }

// Run a load to its end.  Returns the number of step() calls.
static int finish(Loader &L, uint32_t budget = 5000, int max_steps = 4000000) {
    int steps = 0;
    while (L.stage() != LoadStage::Done && L.stage() != LoadStage::Failed && L.stage() != LoadStage::Idle) { L.step(budget, t_now); assert(++steps < max_steps); }
    return steps;
}
// What the tempo engine gives for this PCM when run directly.
static void direct_tempo(const int16_t *pcm, uint64_t frames, uint8_t ch, uint32_t rate, uint32_t &bpm_q16, uint32_t &beats) {
    std::vector<uint8_t> asv((size_t)(frames / 512 + 64) * 70 + 4096), att(1u << 20);
    preview::MemoryReader in((const uint8_t *)pcm, (size_t)frames * ch * 2);
    preview::MemoryWriter aw(asv.data(), asv.size());
    spectrum::DesktopRealFftBackend fft; fft.init();
    spectrum::SpectrumGenerator sg; assert(sg.init(in, aw, fft, rate, ch, true));
    while (sg.step(64) != spectrum::GeneratorStatus::Complete) {}
    preview::MemoryReader ar(asv.data(), (size_t)aw.size());
    preview::MemoryWriter tw(att.data(), att.size());
    tempo::TempoGenerator tg; assert(tg.init(&ar, &tw));
    while (!tg.is_complete() && !tg.has_error()) tg.step(128);
    preview::MemoryReader ti(att.data(), (size_t)tw.size());
    tempo::TempoReader tr; assert(tr.init(&ti));
    bpm_q16 = tr.header().global_bpm_q16; beats = tr.header().total_beats;
}

int main(int argc, char **argv) {
    const std::string clips = argc > 1 ? argv[1] : "../audio_codecs_probe/clips";
    char tmpl[] = "/tmp/djdeck_loader_XXXXXX";
    assert(mkdtemp(tmpl));
    DirStorage st; st.dir = tmpl;
    const LoaderMem mem = {t_alloc, t_release};
    const uint64_t BUDGET = 96ull << 20;
    const char *names[] = {"short.wav", "short.aiff", "click.flac", "click.mp3"};

    for (const char *name : names) {
        const std::string path = std::string("/") + name;
        spit(st.dir + path, slurp(clips + "/" + name));
        // ---- (a) a fresh load: decodes, analyses, saves ----
        {
            Loader L(st, mem, BUDGET);
            st.writes = 0;
            assert(L.start(path.c_str()));
            const uint32_t gen = L.generation();
            // (j) while decoding, what has been decoded never changes afterwards
            uint64_t last = 0; uint32_t snap_sum = 0; uint64_t snap_frames = 0;
            while (L.stage() != LoadStage::Done && L.stage() != LoadStage::Failed) {
                L.step(5000, t_now);
                if (L.stage() == LoadStage::Decode || L.stage() == LoadStage::Spectrum) {
                    assert(L.decodedFrames() >= last); last = L.decodedFrames();
                    if (snap_frames == 0 && last > 20000) { snap_frames = 20000; snap_sum = fnv(L.pcm(), (size_t)snap_frames * L.info().channels * 2); }
                }
            }
            if (L.stage() != LoadStage::Done) { std::printf("FAIL fresh %s: %s\n", name, L.error()); return 1; }
            assert(snap_frames && fnv(L.pcm(), (size_t)snap_frames * L.info().channels * 2) == snap_sum);
            assert(L.generation() == gen && L.sidecars() == SidecarResult::Saved && st.writes == 3 && !L.peaksFromFile());
            assert(L.decodedFrames() == L.totalFrames() && L.totalFrames() > 0 && L.progressPct() == 100);
            // the PCM is the reference decode
            const auto file = slurp(st.dir + path);
            std::vector<int16_t> ref(44100 * 2 * 10); ProbeResult r;
            if (strstr(name, ".wav")) { static wav::WavDecoder x; r = probe_decode_container(x, file.data(), file.size(), ref.data(), ref.size(), 16384); }
            else if (strstr(name, ".aiff")) { static aiff::AiffDecoder x; r = probe_decode_container(x, file.data(), file.size(), ref.data(), ref.size(), 16384); }
            else if (strstr(name, ".flac")) { static flac::FlacDecoder x; r = probe_decode_container(x, file.data(), file.size(), ref.data(), ref.size(), file.size()); }
            else { static mp3::Mp3Decoder x; r = probe_decode_mp3(x, file.data(), file.size(), ref.data(), ref.size()); }
            assert(L.totalFrames() == r.frames && L.info().channels == r.channels && L.info().sample_rate == r.rate);
            assert(memcmp(L.pcm(), ref.data(), (size_t)r.frames * r.channels * 2) == 0);
            // the three sidecars exist; the .apv is PreviewGenerator's but for the two ownership fields
            const auto apv = slurp(st.dir + path + ".apv");
            assert(!apv.empty() && exists(st.dir + path + ".asv") && exists(st.dir + path + ".att"));
            {
                std::vector<uint8_t> want(apv.size() + 4096);
                preview::MemoryReader src((const uint8_t *)ref.data(), (size_t)r.frames * r.channels * 2);
                preview::MemoryWriter dst(want.data(), want.size());
                preview::PreviewGenerator gen2; assert(gen2.init(src, dst, nullptr, r.rate, r.channels, true)); assert(gen2.generate_all());
                assert((size_t)dst.size() == apv.size());
                for (size_t i = 0; i < apv.size(); i++) if (i < 28 || i >= 36) assert(apv[i] == want[i]);
                assert(memcmp(apv.data() + 28, want.data() + 28, 8) != 0);        // and it IS stamped
            }
            // tempo: what the engine gives for this PCM
            uint32_t bpm = 0, beats = 0;
            direct_tempo(L.pcm(), L.totalFrames(), L.info().channels, L.info().sample_rate, bpm, beats);
            // (a beat the engine puts in the last, zero-padded, analysis hop is past the audio and is dropped)
            assert(L.tempoReady() && L.bpmQ16() == bpm && (L.beatCount() == beats || L.beatCount() + 1 == beats));
            for (uint32_t i = 1; i < L.beatCount(); i++) assert(L.beats()[i].frame > L.beats()[i - 1].frame && L.beats()[i].frame < L.totalFrames());
            L.cancel();
            assert(g_live == 0 && L.stage() == LoadStage::Idle);
        }
        // ---- (b) again: everything comes from the sidecars, nothing is written ----
        {
            const auto apv0 = slurp(st.dir + path + ".apv"), att0 = slurp(st.dir + path + ".att");
            Loader L(st, mem, BUDGET);
            st.writes = 0;
            assert(L.start(path.c_str()));
            bool tempo_early = false, peaks_early = false;
            while (L.stage() != LoadStage::Done && L.stage() != LoadStage::Failed) {
                L.step(5000, t_now);
                if (L.stage() == LoadStage::Decode && L.decodedFrames() < L.totalFrames()) {
                    tempo_early = tempo_early || L.tempoReady();
                    peaks_early = peaks_early || (L.peaksFromFile() && L.peaks().points(2) == (L.peaks().totalFrames() + 32767) / 32768);
                }
            }
            assert(L.stage() == LoadStage::Done && L.sidecars() == SidecarResult::Reused && st.writes == 0 && L.peaksFromFile());
            assert(tempo_early && peaks_early);
            assert(slurp(st.dir + path + ".apv") == apv0 && slurp(st.dir + path + ".att") == att0);
            L.cancel(); assert(g_live == 0);
        }
    }
    const std::string P = "/click.flac";
    auto load_once = [&](Loader &L) { assert(L.start(P.c_str())); finish(L); return L.stage(); };
    // ---- (d) a truncated .apv: all three rebuilt ----
    {
        auto apv = slurp(st.dir + P + ".apv"); apv.resize(60); spit(st.dir + P + ".apv", apv);
        Loader L(st, mem, BUDGET); st.writes = 0;
        assert(load_once(L) == LoadStage::Done && L.sidecars() == SidecarResult::Saved && st.writes == 3 && !L.peaksFromFile());
        assert(slurp(st.dir + P + ".apv").size() > 1000);
    }
    // ---- (e) only the .att missing: tempo redone (.asv and .att written), the .apv reused ----
    {
        unlink((st.dir + P + ".att").c_str());
        Loader L(st, mem, BUDGET); st.writes = 0;
        assert(load_once(L) == LoadStage::Done && L.sidecars() == SidecarResult::Saved && st.writes == 2 && L.peaksFromFile() && L.tempoReady());
        assert(exists(st.dir + P + ".att"));
    }
    // ---- (g) a .att cut short (a write that ran out of card): not trusted, tempo redone ----
    {
        Loader L0(st, mem, BUDGET); assert(load_once(L0) == LoadStage::Done && L0.tempoReady());
        const uint32_t beats0 = L0.beatCount(); assert(beats0 > 4);
        L0.cancel();
        auto att = slurp(st.dir + P + ".att");
        tempo::AttHeader h; assert(att.size() >= sizeof(h)); std::memcpy(&h, att.data(), sizeof(h));
        assert(h.total_beats > 4 && h.beat_grid_offset + (uint64_t)h.total_beats * sizeof(tempo::AttBeatMarker) <= att.size());
        att.resize((size_t)h.beat_grid_offset + 3 * sizeof(tempo::AttBeatMarker));     // the header promises more beats than the file holds
        spit(st.dir + P + ".att", att);
        Loader L(st, mem, BUDGET); st.writes = 0;
        assert(load_once(L) == LoadStage::Done && L.tempoReady() && L.beatCount() == beats0);
        assert(L.sidecars() == SidecarResult::Saved && st.writes == 2);
    }
    // ---- (h) no memory for the analysis: the track is complete and stays, without tempo ----
    {
        unlink((st.dir + P + ".apv").c_str()); unlink((st.dir + P + ".asv").c_str()); unlink((st.dir + P + ".att").c_str());
        Loader L(st, mem, BUDGET); st.writes = 0;
        assert(L.start(P.c_str()));
        int steps = 0;
        while (L.stage() != LoadStage::Done && L.stage() != LoadStage::Failed) {
            if (L.stage() == LoadStage::Decode && L.decodedFrames() > 0) g_no_mem = true;
            L.step(5000, t_now); assert(++steps < 4000000);
        }
        g_no_mem = false;
        assert(L.stage() == LoadStage::Done && L.pcm() && L.decodedFrames() == L.totalFrames() && !L.tempoReady());
        assert(L.sidecars() == SidecarResult::SaveFailed && st.writes == 0);
        L.cancel(); assert(g_live == 0);
        Loader L2(st, mem, BUDGET); assert(load_once(L2) == LoadStage::Done && L2.tempoReady());   // sidecars back for the cases below
    }
    // ---- (c) the audio file changes (one more byte): the sidecars are someone else's now ----
    {
        auto d = slurp(st.dir + P); d.push_back(0); spit(st.dir + P, d);
        Loader L(st, mem, BUDGET); st.writes = 0;
        assert(load_once(L) == LoadStage::Done && L.sidecars() == SidecarResult::Saved && st.writes == 3 && !L.peaksFromFile());
    }
    // ---- a sidecar that belongs to another track of the same size is caught by the checksum ----
    {
        auto d = slurp(st.dir + P); d[60] ^= 0x01; spit(st.dir + P, d);       // inside STREAMINFO's MD5: same size, different first 4 KiB
        Loader L(st, mem, BUDGET); st.writes = 0;
        assert(load_once(L) == LoadStage::Done && st.writes == 3 && !L.peaksFromFile());
    }
    // ---- (f) a card that cannot be written: the track still loads, with tempo ----
    {
        unlink((st.dir + P + ".apv").c_str()); unlink((st.dir + P + ".asv").c_str()); unlink((st.dir + P + ".att").c_str());
        st.read_only = true;
        Loader L(st, mem, BUDGET);
        assert(load_once(L) == LoadStage::Done && L.sidecars() == SidecarResult::SaveFailed && L.tempoReady() && L.decodedFrames() == 352800);
        st.read_only = false;
        assert(!exists(st.dir + P + ".apv"));
    }
    // ---- (g) too long for the budget ----
    {
        Loader L(st, mem, 1u << 20);
        assert(L.start(P.c_str())); finish(L);
        assert(L.stage() == LoadStage::Failed && !strncmp(L.error(), "too long (max ", 14));
        L.cancel(); assert(g_live == 0);
    }
    // ---- (h) the card stops answering 200 KiB in ----
    {
        st.limit_reads = true; st.read_budget = 200 * 1024;
        Loader L(st, mem, BUDGET);
        assert(L.start(P.c_str())); finish(L);
        st.limit_reads = false; st.read_budget = UINT64_MAX;
        if (!(L.stage() == LoadStage::Failed && !strcmp(L.error(), "read error"))) { std::printf("FAIL (h): stage %d error '%s'\n", (int)L.stage(), L.error()); return 1; }
        L.cancel(); assert(g_live == 0);
    }
    // ---- (i) a second start() during the first decode ----
    {
        spit(st.dir + "/b.wav", slurp(clips + "/short.wav"));
        Loader L(st, mem, BUDGET);
        assert(L.start(P.c_str()));
        while (L.stage() != LoadStage::Decode || L.decodedFrames() < 50000) L.step(2000, t_now);
        const uint32_t gen = L.generation(); const int live_mid = g_live;
        assert(L.start("/b.wav"));
        assert(L.generation() == gen + 1 && g_live < live_mid);
        finish(L);
        assert(L.stage() == LoadStage::Done && L.totalFrames() == 88200);
        L.cancel(); assert(g_live == 0);
    }
    // ---- (k) step() honours its budget: with 500 us per unit and 5000 us, about ten units a call ----
    {
        Loader L(st, mem, BUDGET);
        assert(L.start(P.c_str()));
        g_tick = 500; uint32_t worst = 0;
        while (L.stage() != LoadStage::Done && L.stage() != LoadStage::Failed) { g_units = 0; L.step(5000, t_now); if (g_units > worst) worst = g_units; }
        g_tick = 1500;
        assert(L.stage() == LoadStage::Done && worst <= 13 && worst >= 5);
        L.cancel();
    }
    // ---- (l) not audio at all ----
    {
        spit(st.dir + "/not_audio.mp3", std::vector<uint8_t>(5000, 'x'));
        spit(st.dir + "/empty.wav", std::vector<uint8_t>());
        Loader L(st, mem, BUDGET);
        assert(L.start("/not_audio.mp3")); finish(L); assert(L.stage() == LoadStage::Failed && !strcmp(L.error(), "unsupported"));
        assert(L.start("/empty.wav")); finish(L);     assert(L.stage() == LoadStage::Failed && !strcmp(L.error(), "unsupported"));
        assert(!L.start("/song.ogg") && L.stage() == LoadStage::Failed && !strcmp(L.error(), "unsupported"));
        assert(!L.start("/missing.wav") && L.stage() == LoadStage::Failed && !strcmp(L.error(), "read error"));
        L.cancel(); assert(g_live == 0);
    }
    std::printf("loader_test: all assertions passed\n");
    return 0;
}
