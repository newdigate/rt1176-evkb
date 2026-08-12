#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"
#include "synth_sine.h"
#include "analyze_peak.h"
#include "output_i2s.h"
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

// Task 3: AudioSynthWaveformSine -> AudioOutputI2S (SAI1 TX DMA) + a synth peak
// sanity. STAGE_SYNTH proves the source; STAGE_TONE is asserted host-side from
// the SAI1 TX tap file. QEMU is timer/tap-paced -> proves the graph->SAI-TX
// plumbing; the real 44.1 kHz rate + audibility are the HW item (Task 4).
AudioSynthWaveformSine sine;
AudioAnalyzePeak       peak;
AudioOutputI2S         out;
AudioConnection        pcPeak(sine, 0, peak, 0);   // sanity tap of the source
AudioConnection        pcL(sine, 0, out, 0);       // left  = sine
AudioConnection        pcR(sine, 0, out, 1);       // right = sine
BOARD_CODEC_T          wm;

void setup() {
    CONSOLE.begin(115200);
    while (!CONSOLE) {}
    AudioMemory(12);
    wm.enable();
    sine.frequency(1000.0f);
    sine.amplitude(0.5f);
    // out ctor auto-called begin(): SAI1 TX DMA is running; its isr pends
    // update_all() as the FIFO drains, so the graph self-clocks. Give it time.
    float pk = 0.0f;
    uint32_t t0 = millis();
    while (millis() - t0 < 500) {
        if (peak.available()) { float v = peak.read(); if (v > pk) pk = v; }
        yield();
    }
    bool synth_ok = pk > 0.40f && pk < 0.60f;
    CONSOLE.print("info synth_peak="); CONSOLE.println(pk, 4);
    CONSOLE.println(synth_ok ? "STAGE_SYNTH=PASS" : "STAGE_SYNTH=FAIL");
    // STAGE_TONE is decided host-side from the tap; emit a marker so the run
    // script knows the firmware reached steady state.
    CONSOLE.println("TONE_PLAYING");
}
void loop() {
    static uint32_t last = 0;
    if (millis() - last > 500) {
        last = millis();
        // Periodic health readout for the HW test: the sine also feeds `peak`,
        // and `peak` only advances when the graph runs (the TX DMA isr pends
        // update_all). ~0.5 here => the graph self-clocks on silicon => the same
        // sine is going out SAI1 TX to the codec DAC (WM8962/J101 on the
        // 1170-EVKB, WM8960 on the 1060-EVKB). "(no update)" => the
        // TX DMA isn't driving the graph (a silicon-vs-QEMU divergence to chase).
        if (peak.available()) {
            CONSOLE.print("TONE_PLAYING synth_peak="); CONSOLE.println(peak.read(), 4);
        } else {
            CONSOLE.println("TONE_PLAYING synth_peak=(no update)");
        }
    }
}
