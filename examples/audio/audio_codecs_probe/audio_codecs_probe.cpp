#include <Arduino.h>
#include "audio_codecs/wav/wav_decoder.h"
#include "audio_codecs/aiff/aiff_decoder.h"
#include "audio_codecs/flac/flac_decoder.h"
#include "audio_codecs/mp3/mp3_decoder.h"
#include "audio_codecs/preview/preview_reader.h"
#include "audio_codecs/spectrum/spectrum_generator.h"
#include "audio_codecs/spectrum/desktop_fft.h"
#include "audio_codecs/tempo/tempo_generator.h"
#include "audio_codecs/tempo/tempo_reader.h"

void setup() {
    Serial1.begin(115200);
    Serial1.println("=== BOOT ===");
    Serial1.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n", "audio_codecs_probe", 1, __DATE__, __TIME__);
}
void loop() {}
