#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"
#include "input_i2s.h"
#include "analyze_peak.h"
// The console is LPUART1 on BOTH boards. The two cores just name it
// differently: cores/imxrt1176 calls LPUART1 `Serial1`, while cores/teensy4
// follows the Teensy pin-0/1 convention and calls LPUART6 `Serial1` and LPUART1
// `Serial6`. Naming it once here is what keeps QEMU and silicon reading the same
// wire -- on the MIMXRT1060-EVKB, LPUART1 (GPIO_AD_B0_12/13) is the DAPLink VCOM,
// whereas LPUART6 only reaches Arduino header pins D0/D1.
//
// The CODEC is a genuinely different chip, not a naming difference: the
// MIMXRT1170-EVKB has a WM8962, the MIMXRT1060-EVKB a WM8960. Different register
// maps, different drivers. The I2C bus differs too (LPI2C5 vs LPI2C1, both at
// 0x1A) but needs no guard here -- control_wm8962.cpp uses Wire2 and
// control_wm8960.cpp uses Wire, so swapping the class swaps the bus.
#if defined(ARDUINO_MIMXRT1060_EVKB)
#include "control_wm8960.h"
#define CONSOLE       Serial6
#define BOARD_CODEC_T AudioControlWM8960
#else
#include "control_wm8962.h"
#define CONSOLE       Serial1
#define BOARD_CODEC_T AudioControlWM8962
#endif

// Task 4 integration gate: SAI injector -> AudioInputI2S (SAI1-RX DMA capture)
// -> AudioAnalyzePeak. Proves the capture->graph->peak plumbing; QEMU is
// injector/timer-paced so the 44.1 kHz clock rate itself is a HW item (Task 5).
AudioInputI2S      in;
AudioAnalyzePeak   peak;
// RIGHT channel (channel 1): AudioInputI2S::isr deinterleaves even samples ->
// left (ch 0), odd samples -> right (ch 1), on both boards. The SAI RX work's
// injector convention puts the primary signal on the right channel, which also
// matches the MIMXRT1170-EVKB's on-board mic (WM8962 Input3). On the
// MIMXRT1060-EVKB the analogue front end is the WM8960's and is wired
// differently -- so on that board treat the channel choice as the injector
// convention only, and confirm against the codec's own routing before reading
// anything into a silicon mic measurement.
AudioConnection    patchCord(in, 1, peak, 0);
BOARD_CODEC_T      wm;

void setup() {
    CONSOLE.begin(115200);
    while (!CONSOLE) {}
    AudioMemory(24);
    wm.enable();
    // AudioInputI2S ctor auto-calls begin()
    float pk = 0.0f;
    uint32_t t0 = millis();
    while (millis() - t0 < 500) {
        if (peak.available()) { float v = peak.read(); if (v > pk) pk = v; }
        yield();
    }
    bool ok = pk > 0.02f;
    CONSOLE.print("info peak="); CONSOLE.println(pk, 4);
    CONSOLE.println(ok ? "STAGE_PEAK=PASS" : "STAGE_PEAK=FAIL");
    CONSOLE.println(ok ? "AUDIOINPUT_ALL=PASS" : "AUDIOINPUT_ALL=FAIL");
}
// Periodic peak report for the HW mic test: every ~500 ms print the peak over
// that window (analyze_peak accumulates until read()). Gated on available() so
// a dataless read() (which would return ~1.0 from the reset min/max) is skipped.
// In QEMU the injector keeps this at ~0.7500 on either board; on silicon it
// tracks whatever the codec's ADC is routed to -- on the MIMXRT1170-EVKB that
// is the on-board mic (WM8962 Input3), low when quiet and rising on sound near
// it. The MIMXRT1060-EVKB's WM8960 front end differs, so check its routing
// before expecting the same behaviour there.
void loop() {
    static uint32_t last = 0;
    if (millis() - last > 500) {
        last = millis();
        if (peak.available()) {
            CONSOLE.print("MIC peak="); CONSOLE.println(peak.read(), 4);
        } else {
            CONSOLE.println("MIC peak=(no blocks)");
        }
    }
}
