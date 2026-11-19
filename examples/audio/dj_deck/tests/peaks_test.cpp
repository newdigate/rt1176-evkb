// Host test for PeakBuilder: APV1 peak tables built while a track decodes.
// The contract is byte identity with audio-codecs' PreviewGenerator.
#undef NDEBUG
#include "peaks.h"
#include "audio_codecs/preview/preview_generator.h"
#include "audio_codecs/preview/preview_reader.h"
#include "audio_codecs/preview/preview_stream.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace audio_codecs::preview;
static uint32_t g = 7;
static int16_t rnd16() { g = g * 1664525u + 1013904223u; return (int16_t)(g >> 16); }

static std::vector<uint8_t> reference(const std::vector<int16_t> &pcm, uint8_t ch, uint32_t rate) {
    std::vector<uint8_t> out(pcm.size() / 16 + 4096);
    MemoryReader src((const uint8_t *)pcm.data(), pcm.size() * 2);
    MemoryWriter dst(out.data(), out.size());
    PreviewGenerator gen;
    assert(gen.init(src, dst, nullptr, rate, ch, true));
    assert(gen.generate_all());
    out.resize((size_t)dst.size());
    return out;
}
// The two ownership fields are the loader's to fill in; everything else must match.
static bool same_file(const uint8_t *a, size_t an, const std::vector<uint8_t> &b) {
    if (an != b.size()) { std::printf("size %zu vs %zu\n", an, b.size()); return false; }
    for (size_t i = 0; i < an; i++) {
        if (i >= 28 && i < 36) continue;                    // source_file_size, source_header_crc32
        if (a[i] != b[i]) { std::printf("byte %zu: %02x vs %02x\n", i, a[i], b[i]); return false; }
    }
    return true;
}

int main() {
    for (uint8_t ch : {(uint8_t)2, (uint8_t)1}) {
        for (uint32_t frames : {1000000u, 128u * 16u * 16u * 3u, 127u, 129u, 2049u, 1u}) {
            std::vector<int16_t> pcm((size_t)frames * ch);
            for (auto &s : pcm) s = rnd16();
            const auto ref = reference(pcm, ch, 44100);
            for (uint32_t chunk : {1u, 127u, 128u, 4096u, 100000u}) {
                if (chunk == 1 && frames > 5000) continue;   // slow and adds nothing
                std::vector<uint8_t> store(PeakBuilder::storageBytes(frames, ch));
                PeakBuilder pb;
                assert(pb.begin(store.data(), store.size(), ch, 44100, frames));
                uint32_t done = 0;
                while (done < frames) {
                    const uint32_t n = frames - done < chunk ? frames - done : chunk;
                    pb.push(pcm.data() + (size_t)done * ch, n);
                    done += n;
                    // (c) every table fills in behind the decode, complete points only
                    assert(pb.points(0) == done / 128u);
                    assert(pb.points(1) == done / 2048u);
                    assert(pb.points(2) == done / 32768u);
                }
                pb.finish(frames);
                if (!same_file(pb.image(), pb.imageBytes(), ref)) {
                    std::printf("FAIL identity ch=%u frames=%u chunk=%u\n", ch, frames, chunk); return 1;
                }
                assert(pb.framesPerPoint(0) == 128 && pb.framesPerPoint(1) == 2048 && pb.framesPerPoint(2) == 32768);
                assert(pb.channels() == ch);
                // (b) the library's reader accepts it
                MemoryReader r(pb.image(), pb.imageBytes());
                PreviewReader pr;
                assert(pr.init(r));
                assert(pr.total_frames() == frames && pr.sample_rate() == 44100 && pr.channels() == ch);
            }
        }
    }
    {   // (d) the track turned out shorter than announced (MP3): still a valid file for the real length
        const uint32_t announced = 500000, actual = 431234;
        std::vector<int16_t> pcm((size_t)actual * 2);
        for (auto &s : pcm) s = rnd16();
        std::vector<uint8_t> store(PeakBuilder::storageBytes(announced, 2));
        PeakBuilder pb;
        assert(pb.begin(store.data(), store.size(), 2, 48000, announced));
        pb.push(pcm.data(), actual);
        pb.finish(actual);
        assert(same_file(pb.image(), pb.imageBytes(), reference(pcm, 2, 48000)));
        // pushes beyond what was announced are dropped, never written past the storage
        PeakBuilder over;
        std::vector<uint8_t> small(PeakBuilder::storageBytes(1000, 2) + 64, 0xEE);
        assert(over.begin(small.data(), small.size() - 64, 2, 44100, 1000));
        over.push(pcm.data(), 50000);
        over.finish(50000);
        for (size_t i = small.size() - 64; i < small.size(); i++) assert(small[i] == 0xEE);
        assert(over.points(0) <= 8);
    }
    {   // (e) storage too small
        uint8_t tiny[200];
        PeakBuilder pb;
        assert(!pb.begin(tiny, sizeof tiny, 2, 44100, 1000000));
        assert(!pb.begin(nullptr, 1 << 20, 2, 44100, 1000));
        assert(!pb.begin(tiny, sizeof tiny, 3, 44100, 100));
        assert(PeakBuilder::storageBytes(0, 2) == 128);
    }
    {   // (f) adopt: tables read from a saved file
        const uint32_t frames = 300000;
        std::vector<int16_t> pcm((size_t)frames * 2);
        for (auto &s : pcm) s = rnd16();
        std::vector<uint8_t> store(PeakBuilder::storageBytes(frames, 2));
        PeakBuilder pb;
        assert(pb.begin(store.data(), store.size(), 2, 44100, frames));
        pb.push(pcm.data(), frames); pb.finish(frames);
        pb.stamp(0x12345678u, 0xCAFEF00Du);
        std::vector<uint8_t> file(pb.image(), pb.image() + pb.imageBytes());
        PeakBuilder ad;
        assert(ad.adopt(file.data(), file.size()));
        for (int l = 0; l < 3; l++) {
            assert(ad.points(l) == pb.points(l) && ad.framesPerPoint(l) == pb.framesPerPoint(l));
            assert(memcmp(ad.table(l), pb.table(l), (size_t)pb.points(l) * 4) == 0);
        }
        assert(ad.channels() == 2 && ad.totalFrames() == frames && ad.sampleRate() == 44100);
        assert(ad.sourceFileSize() == 0x12345678u && ad.sourceHeaderCrc() == 0xCAFEF00Du);
        // damaged files are refused
        PeakBuilder bad;
        assert(!bad.adopt(file.data(), 60));                                  // cut inside the header
        assert(!bad.adopt(file.data(), file.size() - 100));                   // tables cut short
        std::vector<uint8_t> f2 = file; f2[0] ^= 0xFF; assert(!bad.adopt(f2.data(), f2.size()));              // magic
        f2 = file; f2[4] = 9; assert(!bad.adopt(f2.data(), f2.size()));                                       // version
        f2 = file; memset(f2.data() + 80, 0xFF, 8); assert(!bad.adopt(f2.data(), f2.size()));                 // a table offset far outside the file
        f2 = file; f2[36] = 2; assert(!bad.adopt(f2.data(), f2.size()));                                      // lod_count
        assert(!bad.adopt(nullptr, 1000));
    }
    std::printf("peaks_test: all assertions passed\n");
    return 0;
}
