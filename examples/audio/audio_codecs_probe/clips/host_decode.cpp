// host_decode FILE NAME [OUT.raw] -- decode with audio-codecs on the host and print the clips.h row.
#include "probe_decode.h"
#include <cstdio>
#include <string>
#include <vector>
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    std::FILE *fp = std::fopen(argv[1], "rb"); if (!fp) return 2;
    std::vector<uint8_t> d; uint8_t b[65536]; size_t n;
    while ((n = std::fread(b, 1, sizeof b, fp)) > 0) d.insert(d.end(), b, b + n);
    std::fclose(fp);
    std::string f = argv[1]; std::string ext = f.substr(f.rfind('.') + 1);
    std::vector<int16_t> out(44100 * 2 * 12);
    ProbeResult r;
    if (ext == "wav") { static audio_codecs::wav::WavDecoder x; r = probe_decode_container(x, d.data(), d.size(), out.data(), out.size(), 16384); }
    else if (ext == "aiff") { static audio_codecs::aiff::AiffDecoder x; r = probe_decode_container(x, d.data(), d.size(), out.data(), out.size(), 16384); }
    else if (ext == "flac") { static audio_codecs::flac::FlacDecoder x; r = probe_decode_container(x, d.data(), d.size(), out.data(), out.size(), d.size()); }
    else { static audio_codecs::mp3::Mp3Decoder x; r = probe_decode_mp3(x, d.data(), d.size(), out.data(), out.size()); }
    if (argc > 3) { std::FILE *o = std::fopen(argv[3], "wb"); std::fwrite(out.data(), 2, (size_t)r.frames * r.channels, o); std::fclose(o); }
    std::printf("    { \"%s\", \"%s\", clip_%s, clip_%s_end, %u, %u, %u, 0x%08Xu },   /* bits=%u error=%d */\n",
                argv[1], ext.c_str(), argv[2], argv[2], r.rate, (unsigned)r.channels, r.frames,
                probe_fnv(out.data(), (size_t)r.frames * r.channels), (unsigned)r.bits, r.error);
    if (argc > 4) {   // MP3: a float decoder is not bit-identical across CPUs, so also emit a signature to compare within a tolerance
        std::FILE *o = std::fopen(argv[4], "w");
        std::fprintf(o, "#define CLIP_MP3_WIN 4096\nstatic const uint32_t clip_mp3_sig[] = {");
        for (uint32_t w = 0; w + 4096 <= r.frames; w += 4096) {
            uint32_t sum = 0;
            for (uint32_t i = w * 2; i < (w + 4096) * 2; i++) sum += (uint32_t)(out[i] < 0 ? -out[i] : out[i]);
            std::fprintf(o, "%u,", sum);
        }
        std::fprintf(o, "};\n#define CLIP_MP3_EXCERPT_AT 44100\nstatic const int16_t clip_mp3_excerpt[] = {");
        for (uint32_t i = 44100 * 2; i < (44100 + 1024) * 2; i++) std::fprintf(o, "%d,", out[i]);
        std::fprintf(o, "};\n");
        std::fclose(o);
    }
    return 0;
}
