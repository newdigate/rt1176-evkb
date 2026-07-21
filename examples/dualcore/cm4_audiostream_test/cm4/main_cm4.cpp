/* cm4_audiostream_test CM4 firmware: the AudioStream graph engine running on
 * the M4. The image compiles the REAL core engine — cores/imxrt1176/
 * AudioStream.cpp, unmodified — against the cm4_shim Arduino-lite header
 * (include-path ordering puts cores/imxrt1176/cm4_shim before the core dir),
 * plus the audiostream_test TestSource/TestSink pair ported to MU reporting.
 *
 * Sequence: run static ctors (TestSource's ctor calls update_setup(), which
 * NVIC-enables IRQ_SOFTWARE=44; attachInterruptVector is a shim no-op — the
 * handler is static, vector index 60 in startup_cm4.S), enable DWT CYCCNT
 * (software_isr reads ARM_DWT_CYCCNT; on the CM4 it must be switched on),
 * hand-expand AudioMemory(20) (the macro's DMAMEM is a CM7-world placement;
 * the CM4 pool lives in plain DTCM .bss), unmask PRIMASK (reset left it set),
 * pend IRQ_SOFTWARE 8x — each pend runs one software_isr graph pass — then
 * assert flow (8 blocks, values 0..7 in order) and noleak (pool drained to 0).
 *
 * Reported over MU channel 0, in order:
 *   flow    00000001  blocks flowed source->sink in order
 *   noleak  00000001  AudioMemoryUsage() back to 0
 *   recv    received-block count (expect 00000008)
 *   memmax  AudioMemoryUsageMax() (pool high-water mark)
 *   done    D0DE0003
 * Public domain (N. Newdigate). */
#include <stdint.h>
#include "Arduino.h"        // cores/imxrt1176/cm4_shim/Arduino.h
#include "AudioStream.h"    // the real core engine header
extern "C" {
#include "mu_report.h"
void cm4_run_ctors(void);
}

// AudioStream.cpp's software_isr is a plain C++-linkage free function
// (AudioStream.cpp:54/316 — no extern "C"), so its symbol is mangled
// (_Z12software_isrv) and the static vector table can't name it directly.
// This thin extern "C" wrapper is what startup_cm4.S places at index 60.
void software_isr(void);
extern "C" void Software_IRQHandler(void) { software_isr(); }

// --- TestSource/TestSink: ported from examples/audio/audiostream_test ------

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

// AudioMemory(20), hand-expanded (AudioStream.h:121-124). The macro places
// the pool in DMAMEM — a CM7-world section; here the pool is plain DTCM .bss.
static audio_block_t audio_pool[20];

// DWT cycle counter: software_isr reads ARM_DWT_CYCCNT for its CPU-usage
// bookkeeping; on the CM4 the counter must be enabled first.
#define DEMCR      (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL   (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)

extern "C" int main(void) {
    cm4_run_ctors();                 // TestSource ctor -> update_setup()

    DEMCR |= (1u << 24);             // TRCENA: unlock the DWT
    DWT_CYCCNT = 0u;
    DWT_CTRL |= 1u;                  // CYCCNTENA

    AudioStream::initialize_memory(audio_pool, 20);

    __asm volatile ("cpsie i");      // reset left PRIMASK set; NVIC is armed

    const int N = 8;
    for (int k = 0; k < N; k++) {
        NVIC_SET_PENDING(IRQ_SOFTWARE);   // stand in for the audio clock's trigger
        // One graph pass per pend: wait (bounded) for the ISR to deliver it.
        for (volatile uint32_t i = 0; i < 200000u && sink.received <= (uint16_t)k; i++) {}
    }

    // flow: sink saw N blocks, values 0..N-1 in order
    uint32_t flow = (sink.received >= (uint16_t)N) ? 1u : 0u;
    for (int k = 0; k < N && flow; k++) if (sink.values[k] != (int16_t)k) flow = 0u;
    // noleak: every allocated block was released -> memory_used back to 0
    uint32_t noleak = (AudioMemoryUsage() == 0) ? 1u : 0u;

    mu_send(flow);
    mu_send(noleak);
    mu_send(sink.received);
    mu_send(AudioMemoryUsageMax());
    mu_send(0xD0DE0003u);
    for (;;) { __asm volatile ("wfi"); }
}
