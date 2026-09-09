#include "AudioStream.h"
namespace {
audio_block_t s_pool[ShimAudio::POOL];
bool     s_used[ShimAudio::POOL];
int      s_limit = ShimAudio::POOL;
int      s_out = 0;
uint8_t  s_logCh[ShimAudio::LOG];
int16_t  s_logData[ShimAudio::LOG][AUDIO_BLOCK_SAMPLES];
int      s_logN = 0;
}
void ShimAudio::reset() { memset(s_used, 0, sizeof s_used); s_out = 0; s_logN = 0; s_limit = POOL; }
void ShimAudio::setPool(int n) { s_limit = n < 0 ? 0 : (n > POOL ? POOL : n); }
int  ShimAudio::outstanding() { return s_out; }
int  ShimAudio::logCount() { return s_logN; }
uint8_t ShimAudio::logChan(int i) { return s_logCh[i]; }
const int16_t *ShimAudio::logData(int i) { return s_logData[i]; }
int32_t ShimAudio::meanAbs(const int16_t *d) {
    uint32_t sum = 0;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { int32_t v = d[i]; sum += (uint32_t)(v < 0 ? -v : v); }
    return (int32_t)(sum / AUDIO_BLOCK_SAMPLES);
}
audio_block_t *AudioStream::allocate(void) {
    if (s_out >= s_limit) return nullptr;
    for (int i = 0; i < ShimAudio::POOL; i++) if (!s_used[i]) {
        s_used[i] = true; s_out++; s_pool[i].ref_count = 1;
        memset(s_pool[i].data, 0x5A, sizeof s_pool[i].data);   // poison: a block the node forgot to fill is visible
        return &s_pool[i];
    }
    return nullptr;
}
void AudioStream::release(audio_block_t *b) {
    if (!b) return;
    for (int i = 0; i < ShimAudio::POOL; i++) if (&s_pool[i] == b) { if (s_used[i]) { s_used[i] = false; s_out--; } return; }
}
void AudioStream::transmit(audio_block_t *b, unsigned char index) {
    if (!b || s_logN >= ShimAudio::LOG) return;
    s_logCh[s_logN] = index; memcpy(s_logData[s_logN], b->data, sizeof b->data); s_logN++;
}
