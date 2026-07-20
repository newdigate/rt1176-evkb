#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"

// Synthetic source: each update() emits a block filled with an incrementing
// per-block value (block k -> all samples = k), so the sink can verify order.
class TestSource : public AudioStream {
public:
    TestSource() : AudioStream(0, NULL) { update_setup(); }  // take the software-IRQ dispatch
    volatile uint16_t produced = 0;
    void update(void) override {
        audio_block_t *b = allocate();
        if (!b) return;                       // pool exhausted -> skip (sink sees a gap)
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) b->data[i] = (int16_t)produced;
        transmit(b, 0);
        release(b);                           // we transmitted; drop our ownership
        produced++;
    }
};

// Synthetic sink: 1 input; records the first sample of each received block.
class TestSink : public AudioStream {
public:
    TestSink() : AudioStream(1, inputQueueArray) {}
    volatile uint16_t received = 0;
    volatile int16_t last = -1;
    volatile int16_t values[64];
    void update(void) override {
        audio_block_t *b = receiveReadOnly(0);
        if (!b) return;
        last = b->data[0];
        if (received < 64) values[received] = b->data[0];
        received++;
        release(b);
    }
private:
    audio_block_t *inputQueueArray[1];
};

static TestSource src;
static TestSink   sink;
static AudioConnection conn(src, 0, sink, 0);

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    AudioMemory(20);                          // size the pool

    const int N = 8;
    for (int k = 0; k < N; k++) {
        NVIC_SET_PENDING(IRQ_SOFTWARE);       // stand in for the audio clock's trigger
        delayMicroseconds(200);               // let software_isr run one graph pass
    }

    // STAGE_FLOW: sink saw N blocks, values 0..N-1 in order
    bool flow = (sink.received >= (uint16_t)N);
    for (int k = 0; k < N && flow; k++) if (sink.values[k] != (int16_t)k) flow = false;
    Serial1.println(flow ? "STAGE_FLOW=PASS" : "STAGE_FLOW=FAIL");

    // STAGE_NOLEAK: every allocated block was released -> memory_used back to 0
    bool noleak = (AudioMemoryUsage() == 0);
    Serial1.println(noleak ? "STAGE_NOLEAK=PASS" : "STAGE_NOLEAK=FAIL");

    Serial1.print("info received="); Serial1.print(sink.received);
    Serial1.print(" mem_used="); Serial1.print(AudioMemoryUsage());
    Serial1.print(" mem_max="); Serial1.println(AudioMemoryUsageMax());
    Serial1.println((flow && noleak) ? "AUDIOSTREAM_ALL=PASS" : "AUDIOSTREAM_ALL=FAIL");
}
void loop() {}
