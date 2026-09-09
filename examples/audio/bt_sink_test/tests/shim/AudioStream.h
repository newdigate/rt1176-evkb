// HOST SHIM (NEW-41): the smallest AudioStream that AudioInputBluetooth can be a real subclass of, plus the
// instrumentation the test reads.  The Audio library's own AudioStream drags in the whole graph (update
// responder, the DMA-driven ISR walk, the Teensy memory pool); none of that is what node_test asks about.
// What the node actually uses is four things -- allocate(), release(), transmit() and the audio_block_t
// layout -- so those are what this provides, over a small static pool, with every transmit() COPIED into a log
// the test can assert on.  ShimAudio::outstanding() is the leak witness: the node must release every block it
// allocates, including on the early-return path where only one of the two was available.
#pragma once
#include <stdint.h>
#include <string.h>

#define AUDIO_BLOCK_SAMPLES 128

typedef struct audio_block_struct {
    uint8_t ref_count;
    int16_t data[AUDIO_BLOCK_SAMPLES];
} audio_block_t;

struct ShimAudio {
    static const int POOL = 8;            // the node takes 2 per update(); 8 leaves room to starve it deliberately
    static const int LOG  = 8192;
    static void  reset();                 // free the pool and clear the log
    static void  setPool(int n);          // how many blocks allocate() will hand out before returning nullptr
    static int   outstanding();           // allocated and not yet released
    static int   logCount();
    static uint8_t   logChan(int i);
    static const int16_t *logData(int i); // the COPY taken at transmit() time
    static int32_t   meanAbs(const int16_t *d);   // mean |sample| over one block -- the node's own rms measure
};

class AudioStream {
public:
    AudioStream(unsigned char ninput, audio_block_t **iqueue) { (void)ninput; (void)iqueue; }
    virtual ~AudioStream() {}
    virtual void update(void) = 0;
protected:
    static audio_block_t *allocate(void);
    static void release(audio_block_t *block);
    void transmit(audio_block_t *block, unsigned char index = 0);
};
