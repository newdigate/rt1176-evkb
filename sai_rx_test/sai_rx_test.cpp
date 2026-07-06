#include <Arduino.h>
#include "HardwareSerial.h"
#include "I2S.h"
#include "core_pins.h"

static int16_t expect_sine[96 * 2];
static int16_t got[96 * 2];
static DMAMEM int16_t rx_ring[96 * 2];
static DMAMEM int16_t tx_ring[96 * 2];
static void build_sine() {
    for (int i = 0; i < 96; i++) {
        double ph = 2.0 * 3.14159265358979 * (i % 48) / 48.0;
        int16_t v = (int16_t)(0x6000 * __builtin_sin(ph));
        expect_sine[2*i + 0] = v;
        expect_sine[2*i + 1] = (int16_t)(v / 2);
    }
}

void setup() {
    Serial1.begin(115200);
    delay(50);
    build_sine();
    I2S.begin(48000);
    I2S.read(got, 96);
    bool ok = true;
    for (int i = 0; i < 96 * 2; i++) if (got[i] != expect_sine[i]) ok = false;
    Serial1.println(ok ? "STAGE_A_PASS" : "STAGE_A_FAIL");
    I2S.beginReceiveDMA(rx_ring, 96);
    Serial1.println("STAGE_B_DONE");
    for (int i = 0; i < 96 * 2; i++) tx_ring[i] = expect_sine[i];   // same sine out the TX
    // Stage A's I2S.begin() already set TCSR[TE] (polled TX/RX check), with
    // FRDE (FIFO-request-DMA-enable) still clear at that point. QEMU's SAI
    // model (imxrt_sai_write) only (re-)arms the TX frame-clock timer that
    // paces the TX DMA request on a TE 0->1 edge; since beginDMA() below only
    // ORs FRDE onto an already-1 TE, TE never re-edges and the model's TX
    // timer -- which self-terminated on its very first tick back in Stage A
    // (FRDE was 0 then, so imxrt_sai_tx_tick's early-return skips its own
    // re-arming timer_mod) -- stays dead forever, and no bytes ever reach the
    // tap. Force a genuine edge here: drop TE (a bare register poke via the
    // same SAI1_TCSR/SAI_TCSR_TE macros I2S.cpp itself uses -- test-only,
    // does not touch cores/ or QEMU) and let beginDMA() below raise
    // TE+BCE+FRDE together in one write, so the model sees FRDE already set
    // at the 0->1 edge and keeps the timer re-arming. This only tears down
    // TX-side model state (imxrt_sai_write's SAI_TCSR case); RX has its own
    // register block (RCSR) and timer, untouched by a TCSR write, so Stage
    // B's already-running RX DMA capture is unaffected.
    SAI1_TCSR &= ~SAI_TCSR_TE;
    I2S.beginDMA(tx_ring, 96);       // full-duplex: TX + RX now both DMA-driven
    Serial1.println("STAGE_C_DONE");
}
void loop() {
    static bool bdone = false, fdone = false;
    if (!bdone && I2S.rxBlockCount() >= 2) {
        bool ok = true;
        for (int i = 0; i < 96 * 2; i++) if (rx_ring[i] != expect_sine[i]) ok = false;
        Serial1.println(ok ? "STAGE_B_PASS" : "STAGE_B_FAIL");
        bdone = true;
    }
    if (bdone && !fdone && I2S.dmaBlockCount() >= 2 && I2S.rxBlockCount() >= 4) {
        Serial1.println("STAGE_FD_PASS");     // TX DMA + RX DMA both advancing
        fdone = true;
    }
}
