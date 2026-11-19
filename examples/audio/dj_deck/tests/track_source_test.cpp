// Host test for the TrackSource adapters: WAV, AIFF, FLAC and MP3 read from a
// file in pieces must give exactly the PCM that decoding the whole file from
// memory gives (audio_codecs_probe/probe_decode.h, the reviewed reference).
#undef NDEBUG
#include "track_source.h"
#include "probe_decode.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using audio_codecs::preview::MemoryReader;
static std::string g_clips;

static std::vector<uint8_t> load(const char *name) {
    std::vector<uint8_t> d;
    std::FILE *fp = std::fopen((g_clips + "/" + name).c_str(), "rb");
    if (!fp) { std::printf("cannot open %s/%s\n", g_clips.c_str(), name); std::exit(2); }
    uint8_t b[65536]; size_t n;
    while ((n = std::fread(b, 1, sizeof b, fp)) > 0) d.insert(d.end(), b, b + n);
    std::fclose(fp);
    return d;
}
struct Ref { ProbeResult r; std::vector<int16_t> pcm; };
static Ref reference(const char *ext, const std::vector<uint8_t> &d) {
    Ref ref; ref.pcm.resize(44100 * 2 * 10);
    if (!strcmp(ext, "wav")) { static audio_codecs::wav::WavDecoder x; ref.r = probe_decode_container(x, d.data(), d.size(), ref.pcm.data(), ref.pcm.size(), 16384); }
    else if (!strcmp(ext, "aiff")) { static audio_codecs::aiff::AiffDecoder x; ref.r = probe_decode_container(x, d.data(), d.size(), ref.pcm.data(), ref.pcm.size(), 16384); }
    else if (!strcmp(ext, "flac")) { static audio_codecs::flac::FlacDecoder x; ref.r = probe_decode_container(x, d.data(), d.size(), ref.pcm.data(), ref.pcm.size(), d.size()); }
    else { static audio_codecs::mp3::Mp3Decoder x; ref.r = probe_decode_mp3(x, d.data(), d.size(), ref.pcm.data(), ref.pcm.size()); }
    ref.pcm.resize((size_t)ref.r.frames * ref.r.channels);
    return ref;
}
struct Got { SrcStatus status; TrackInfo info; std::vector<int16_t> pcm; int probes; int32_t last; };
// Open, probe to Ready, decode in calls of `chunk` frames.
static Got run(const char *name, const std::vector<uint8_t> &d, uint32_t chunk) {
    Got g; g.probes = 0; g.last = 0; g.info = TrackInfo();
    MemoryReader file(d.data(), d.size());
    TrackSource *src = track_source_for_name(name);
    assert(src);
    g.status = src->open(file);
    while (g.status == SrcStatus::Working) { g.status = src->probe(); assert(++g.probes < 100000); }
    if (g.status != SrcStatus::Ready) return g;
    g.info = src->info();
    if (chunk < src->minChunkFrames()) chunk = src->minChunkFrames();
    std::vector<int16_t> buf((size_t)chunk * g.info.channels);
    int guard = 0;
    for (;;) {
        g.last = src->decode(buf.data(), chunk);
        if (g.last <= 0) break;
        assert((uint32_t)g.last <= chunk);
        g.pcm.insert(g.pcm.end(), buf.begin(), buf.begin() + (size_t)g.last * g.info.channels);
        assert(++guard < 10000000);
    }
    src->close();
    return g;
}
static void put32le(std::vector<uint8_t> &v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }
static void put32be(std::vector<uint8_t> &v, uint32_t x) { for (int i = 3; i >= 0; i--) v.push_back((uint8_t)(x >> (8 * i))); }

int main(int argc, char **argv) {
    g_clips = argc > 1 ? argv[1] : "../audio_codecs_probe/clips";
    struct { const char *file, *ext; } clips[] = {{"short.wav", "wav"}, {"short.aiff", "aiff"}, {"click.flac", "flac"}, {"click.mp3", "mp3"}, {"short24.wav", "wav"}};

    // --- every clip, several call sizes: the same PCM as the whole-file decode ---
    for (auto &c : clips) {
        const auto d = load(c.file);
        const Ref ref = reference(c.ext, d);
        assert(ref.r.error == 0 && ref.r.frames > 0);
        for (uint32_t chunk : {1000u, 8192u, 30000u}) {
            const Got g = run(c.file, d, chunk);
            bool ok = g.status == SrcStatus::Ready && g.last == 0 && g.info.sample_rate == ref.r.rate && g.info.channels == ref.r.channels &&
                      g.pcm.size() == ref.pcm.size() && memcmp(g.pcm.data(), ref.pcm.data(), ref.pcm.size() * 2) == 0;
            // the announced length: exact for the containers, an upper bound within two MPEG frames for MP3
            if (!strcmp(c.ext, "mp3")) ok = ok && g.info.total_frames >= ref.r.frames && g.info.total_frames <= ref.r.frames + 2304 && g.info.bit_depth == 16;
            else ok = ok && g.info.total_frames == ref.r.frames && g.info.bit_depth == ref.r.bits;
            if (!ok) { std::printf("FAIL %s chunk %u: status %d last %d frames %zu (want %u) total %llu rate %u ch %u bits %u\n", c.file, chunk, (int)g.status, g.last,
                                   g.pcm.size() / (g.info.channels ? g.info.channels : 1), ref.r.frames, (unsigned long long)g.info.total_frames, g.info.sample_rate, g.info.channels, g.info.bit_depth); return 1; }
        }
    }
    assert(run("short24.wav", load("short24.wav"), 4096).info.bit_depth == 24);

    // --- names ---
    assert(track_source_for_name("x.ogg") == nullptr && track_source_for_name("x.m4a") == nullptr && track_source_for_name("x") == nullptr);
    assert(track_source_for_name("A.WAV") && track_source_for_name("b.Aif") && track_source_for_name("c.AIFF") && track_source_for_name("d.Flac") && track_source_for_name("e.MP3"));

    // --- big tags where real files have them: far more than one read window before and after the audio ---
    {   // WAV: a 300 KB LIST chunk before "data", another after it
        const auto d = load("short.wav"); const Ref ref = reference("wav", d);
        std::vector<uint8_t> v(d.begin(), d.begin() + 36);                    // RIFF header + fmt chunk
        const std::vector<uint8_t> junk(300001, 0x7F);                        // odd length: a pad byte follows
        v.insert(v.end(), {'L', 'I', 'S', 'T'}); put32le(v, (uint32_t)junk.size()); v.insert(v.end(), junk.begin(), junk.end()); v.push_back(0);
        v.insert(v.end(), d.begin() + 36, d.end());
        v.insert(v.end(), {'L', 'I', 'S', 'T'}); put32le(v, 200000); v.insert(v.end(), 200000, 0x55);
        const Got g = run("t.wav", v, 8192);
        assert(g.status == SrcStatus::Ready && g.pcm == ref.pcm && g.info.total_frames == ref.r.frames);
    }
    {   // AIFF: an 'ID3 ' chunk before COMM/SSND and artwork-sized one after
        const auto d = load("short.aiff"); const Ref ref = reference("aiff", d);
        std::vector<uint8_t> v(d.begin(), d.begin() + 12);
        v.insert(v.end(), {'I', 'D', '3', ' '}); put32be(v, 300000); v.insert(v.end(), 300000, 0x7F);
        v.insert(v.end(), d.begin() + 12, d.end());
        v.insert(v.end(), {'I', 'D', '3', ' '}); put32be(v, 200000); v.insert(v.end(), 200000, 0x55);
        const Got g = run("t.aiff", v, 8192);
        assert(g.status == SrcStatus::Ready && g.pcm == ref.pcm && g.info.total_frames == ref.r.frames);
    }
    {   // FLAC: a 400 KB PADDING block (embedded artwork is the same shape) between STREAMINFO and the audio
        const auto d = load("click.flac"); const Ref ref = reference("flac", d);
        // find the end of the metadata: walk the blocks
        size_t p = 4; bool last = false;
        while (!last) { last = d[p] & 0x80; p += 4 + ((size_t)d[p + 1] << 16 | (size_t)d[p + 2] << 8 | d[p + 3]); }
        std::vector<uint8_t> v(d.begin(), d.begin() + 4 + 4 + 34);            // "fLaC" + STREAMINFO
        v[4] &= 0x7F;                                                         // STREAMINFO is no longer the last block
        v.insert(v.end(), {0x81, 0x06, 0x1A, 0x80}); v.insert(v.end(), 400000, 0);     // PADDING, last, 400000 bytes
        v.insert(v.end(), d.begin() + p, d.end());
        const Got g = run("t.flac", v, 8192);
        assert(g.status == SrcStatus::Ready && g.pcm == ref.pcm);
    }
    {   // MP3: a 300 KB ID3v2 tag (artwork) in front
        const auto d = load("click.mp3"); const Ref ref = reference("mp3", d);
        size_t skip = 0;
        if (!memcmp(d.data(), "ID3", 3)) skip = 10 + ((d[6] & 0x7F) << 21 | (d[7] & 0x7F) << 14 | (d[8] & 0x7F) << 7 | (d[9] & 0x7F));
        std::vector<uint8_t> v = {'I', 'D', '3', 3, 0, 0, 0, 0x12, 0x27, 0x60};     // syncsafe 300000
        v.insert(v.end(), 300000, 0xFF);                                            // full of false syncs
        v.insert(v.end(), d.begin() + skip, d.end());
        const Got g = run("t.mp3", v, 8192);
        if (!(g.status == SrcStatus::Ready && g.pcm == ref.pcm && g.info.sample_rate == 44100 && g.info.channels == 2)) {
            std::printf("FAIL mp3 big tag: status %d frames %zu rate %u\n", (int)g.status, g.pcm.size() / 2, g.info.sample_rate); return 1; }
    }
    {   // a streamed WAV: data size 0 means "to the end of the file"
        auto d = load("short.wav"); const Ref ref = reference("wav", d);
        memset(d.data() + 40, 0, 4);
        const Got g = run("t.wav", d, 8192);
        assert(g.status == SrcStatus::Ready && g.info.total_frames == ref.r.frames && g.pcm == ref.pcm);
    }

    // --- broken files: an error or a clean early end, in bounded work ---
    for (auto &c : clips) {
        const auto d = load(c.file);
        for (size_t cut : {(size_t)0, (size_t)3, (size_t)100, d.size() / 2}) {
            std::vector<uint8_t> v(d.begin(), d.begin() + cut);
            const Got g = run(c.file, v, 8192);
            assert(g.probes <= (int)(v.size() / 65536) + 8);
            if (g.status == SrcStatus::Ready) {                               // header was whole: a shorter track, never more than announced
                assert(g.pcm.size() / g.info.channels <= g.info.total_frames);
                assert(cut >= 100);
            } else {
                assert(g.status == SrcStatus::Error && g.pcm.empty());
            }
        }
    }
    {   // text named .mp3; 0xFF throughout; MPEG-2 Layer III
        const char text[] = "This is not an MP3 file, it is a paragraph of text that someone renamed.";
        std::vector<uint8_t> v(text, text + sizeof text);
        assert(run("t.mp3", v, 8192).status == SrcStatus::Error);
        v.assign(400000, 0xFF);
        const Got g = run("t.mp3", v, 8192);
        assert(g.status == SrcStatus::Error && g.probes <= 16);
        v.assign(8192, 0);
        for (size_t at = 0; at + 4 < v.size(); at += 208) { v[at] = 0xFF; v[at + 1] = 0xF3; v[at + 2] = 0x80; v[at + 3] = 0xC4; }
        assert(run("t.mp3", v, 8192).status == SrcStatus::Error);
    }
    {   // the wrong format under a right name
        assert(run("t.flac", load("short.wav"), 8192).status == SrcStatus::Error);
        assert(run("t.wav", load("click.flac"), 8192).status == SrcStatus::Error);
        assert(run("t.aiff", load("click.mp3"), 8192).status == SrcStatus::Error);
    }
    std::printf("track_source_test: all assertions passed\n");
    return 0;
}
