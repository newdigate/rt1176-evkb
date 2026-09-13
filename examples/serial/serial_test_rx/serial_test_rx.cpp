#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"

// Not volatile: written only in serialEvent1() (called from yield(), same thread
// as loop()), never from an ISR — unlike the core's serial1_rx_isr_count.
static uint32_t echoed = 0;

// serialEvent1() overrides the weak default in HardwareSerial1.cpp.  It is the
// ONLY echo path: yield() calls it (between loop() iterations) when RX data is
// available.  Echoes each byte verbatim; counts bytes echoed.
void serialEvent1() {
    while (Serial1.available()) {
        int c = Serial1.read();
        if (c < 0) break;
        Serial1.write((uint8_t)c);
        echoed++;
    }
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1 && millis() < 2000) {}
    Serial1.println("=== BOOT ===");
    Serial1.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n", "serial_test_rx", 1, __DATE__, __TIME__);
    Serial1.println("RT1176 RX echo ready");
}

void loop() {
    // loop() does NOT read/echo (avoids a read race with serialEvent1); it only
    // reports.  Returning to the core main() calls yield(), which drives
    // serialEvent1() when bytes are available.
    Serial1.print("[status] rx_isr=");
    Serial1.print(serial1_rx_isr_count);
    Serial1.print(" echoed=");
    Serial1.println(echoed);
    delay(1000);
}
