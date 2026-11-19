// host_analyse FILE.flac [SECONDS] -- spectrum + tempo with audio-codecs on the host; prints BPM x100 and beats.
#include "probe_decode.h"
#include "audio_codecs/preview/preview_stream.h"
#include "audio_codecs/spectrum/spectrum_generator.h"
#include "audio_codecs/spectrum/desktop_fft.h"
#include "audio_codecs/tempo/tempo_generator.h"
#include "audio_codecs/tempo/tempo_reader.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace audio_codecs;
int main(int argc, char **argv) {
    std::FILE *fp = std::fopen(argv[1], "rb"); if (!fp) return 2;
    std::vector<uint8_t> d; uint8_t b[65536]; size_t n;
    while ((n = std::fread(b, 1, sizeof b, fp)) > 0) d.insert(d.end(), b, b + n);
    std::fclose(fp);
    std::vector<int16_t> out(44100 * 2 * 700);
    static flac::FlacDecoder x;
    ProbeResult r = probe_decode_container(x, d.data(), d.size(), out.data(), out.size(), d.size());
    uint32_t frames = r.frames;
    if (argc > 2 && atof(argv[2]) > 0) { uint32_t want = (uint32_t)(atof(argv[2]) * r.rate); if (want < frames) frames = want; }
    std::vector<uint8_t> asv(64u << 20), att(4u << 20);
    preview::MemoryReader in((const uint8_t *)out.data(), (size_t)frames * 4);
    preview::MemoryWriter aw(asv.data(), asv.size());
    spectrum::DesktopRealFftBackend fft; fft.init();
    spectrum::SpectrumGenerator sg; sg.init(in, aw, fft, r.rate, 2, true);
    while (true) { auto st = sg.step(32); if (st == spectrum::GeneratorStatus::Complete) break; if ((int)st > (int)spectrum::GeneratorStatus::Complete) { std::printf("spectrum error\n"); return 1; } }
    preview::MemoryReader ar(asv.data(), (size_t)aw.size());
    preview::MemoryWriter tw(att.data(), att.size());
    tempo::TempoGenerator tg; tg.init(&ar, &tw);
    while (!tg.is_complete() && !tg.has_error()) tg.step(128);
    preview::MemoryReader tr_in(att.data(), (size_t)tw.size());
    tempo::TempoReader tr; tr.init(&tr_in);
    if (argc > 3) {   // clips.h rows: what the board must reproduce
        auto fnv = [](const uint8_t *p, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u; return h; };
        std::printf("#define CLIP_HOST_ASV_BYTES %lluu\n#define CLIP_HOST_ASV_FNV 0x%08Xu\n#define CLIP_HOST_ATT_BYTES %lluu\n#define CLIP_HOST_ATT_FNV 0x%08Xu\n"
                    "#define CLIP_HOST_BPM_X100 %llu\n#define CLIP_HOST_BEATS %uu\n",
                    (unsigned long long)aw.size(), fnv(asv.data(), (size_t)aw.size()), (unsigned long long)tw.size(), fnv(att.data(), (size_t)tw.size()),
                    ((unsigned long long)tr.header().global_bpm_q16 * 100 + 32768) >> 16, tr.header().total_beats);
        return 0;
    }
    std::printf("frames=%u asv_bytes=%llu bpm_x100=%llu beats=%u first_beat_ms=%u confidence=%u\n", frames, (unsigned long long)aw.size(),
                ((unsigned long long)tr.header().global_bpm_q16 * 100 + 32768) >> 16, tr.header().total_beats, tr.header().first_beat_ms, tr.header().confidence);
    return 0;
}
