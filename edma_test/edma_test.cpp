#include <Arduino.h>
#include "HardwareSerial.h"
#include "DMAChannel.h"
#include "I2S.h"

// ---- Stage A: software-triggered memory-to-memory (buffers in OCRAM) ----
static DMAMEM uint32_t a_src[16];
static DMAMEM uint32_t a_dst[16];
static DMAChannel a_dma(false);          // (false) = don't allocate at static init
static volatile uint32_t a_done;
static void a_isr() { a_dma.clearInterrupt(); a_done = 1; }

// ---- Stage B: DMA-fed I2S sine ring (OCRAM). 96 frames = 2 full 1 kHz cycles. ----
static DMAMEM int16_t g_sine[96 * 2];
static void build_sine() {
    for (int i = 0; i < 96; i++) {
        double ph = 2.0 * 3.14159265358979 * (i % 48) / 48.0;   // match check_tap PI_C
        int16_t v = (int16_t)(0x6000 * __builtin_sin(ph));
        g_sine[2*i + 0] = v;                 // L = full sine
        g_sine[2*i + 1] = (int16_t)(v / 2);  // R = half amplitude
    }
}

void setup() {
    Serial1.begin(115200);
    delay(50);

    for (int i = 0; i < 16; i++) { a_src[i] = 0xA5A50000u + i; a_dst[i] = 0; }
    a_dma.begin();                        // allocate a channel, init TCD
    a_dma.sourceBuffer(a_src, sizeof(a_src));
    a_dma.destinationBuffer(a_dst, sizeof(a_dst));
    a_dma.interruptAtCompletion();
    a_dma.attachInterrupt(a_isr);
    a_dma.triggerManual();                // SSRT -> START: runs the whole major loop
    uint32_t guard = 2000000;
    while (!a_done && guard--) { }
    bool ok = a_done;
    for (int i = 0; i < 16; i++) if (a_dst[i] != a_src[i]) ok = false;
    Serial1.println(ok ? "STAGE_A_PASS" : "STAGE_A_FAIL");

    build_sine();
    I2S.beginDMA(g_sine, 96);                 // static tone: no refill callback
    Serial1.println("STAGE_B_DONE");
}

void loop() {
    // DMA plays continuously with zero CPU involvement. Report once the half +
    // complete interrupts have both fired (block count advanced past 2).
    static bool reported = false;
    if (!reported && I2S.dmaBlockCount() >= 2) {
        Serial1.println("STAGE_B_PASS");
        reported = true;
    }
}
