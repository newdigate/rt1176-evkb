#include <Arduino.h>
#include "HardwareSerial.h"
#include "DMAChannel.h"

// ---- Stage A: software-triggered memory-to-memory (buffers in OCRAM) ----
static DMAMEM uint32_t a_src[16];
static DMAMEM uint32_t a_dst[16];
static DMAChannel a_dma(false);          // (false) = don't allocate at static init
static volatile uint32_t a_done;
static void a_isr() { a_dma.clearInterrupt(); a_done = 1; }

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
}

void loop() { }
