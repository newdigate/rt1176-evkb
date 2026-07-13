#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"

// QEMU LPADC returns channel*0x111 (12-bit). At the default 10-bit resolution
// that is (channel*0x111) >> 2.  Assert both LPADC1 and LPADC2, a couple of
// channels, the 12-bit path, and the pin paths: A0 = GPIO_AD_10 = ADC1 CH2A
// (2*0x111 >> 2 = 136) and A4 = GPIO_AD_09 = ADC1 CH1B (0x111 >> 2 = 68;
// QEMU's synthetic value ignores ABSEL, so A/B sides read the same channel).
static volatile uint16_t async_val = 0xFFFF;
static volatile uint8_t  async_fired = 0;
static void adc_cb(uint16_t v) { async_val = v; async_fired = 1; }

void setup() {
    Serial1.begin(115200);
    Serial1.println("RT1176 LPADC test");
    // Hardware: mux A0 (GPIO_AD_10) to a non-driving ALT so LPADC1 CH2A can tap
    // the pad (SDK lpadc example: ALT5 GPIO_MUX, pad 0x02). No-op under QEMU.
    IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_10 = 5u;
    IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_10 = 0x02u;
    Serial1.print("adc1_ch5=");     Serial1.println(analogReadChannel(0, 5));    // 0x555>>2 = 341
    Serial1.print("adc2_ch3=");     Serial1.println(analogReadChannel(1, 3));    // 0x333>>2 = 204
    analogReadResolution(12);
    Serial1.print("adc1_ch15_12b="); Serial1.println(analogReadChannel(0, 15));  // 0xFFF = 4095
    analogReadResolution(10);
    Serial1.print("A0=");           Serial1.println(analogRead(A0));             // CH2A -> 136
    Serial1.print("A4=");           Serial1.println(analogRead(A4));             // CH1B -> 68
    // async on LPADC1 channel 5 -> callback should deliver 0x555>>2 = 341 (10-bit)
    analogReadResolution(10);
    analogReadAsyncChannel(0, 5, adc_cb);
    for (uint32_t t = 0; t < 1000000 && !async_fired; t++) { __asm__ volatile("nop"); }  /* bounded wait for ISR */
    Serial1.print("async_fired="); Serial1.println(async_fired);
    Serial1.print("async_val=");   Serial1.println(async_val);
    Serial1.println("[adc] done");
}
void loop() {
    // Also print A0 continuously so the hardware test (Task 5) can watch it change.
    Serial1.print("A0="); Serial1.println(analogRead(A0));
    delay(1000);
}
