#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"

// QEMU LPADC returns channel*0x111 (12-bit). At the default 10-bit resolution
// that is (channel*0x111) >> 2.  Assert both LPADC1 and LPADC2, a couple of
// channels, the 12-bit path, and the A0 pin path (channel 0 -> 0).
void setup() {
    Serial1.begin(115200);
    Serial1.println("RT1176 LPADC test");
    Serial1.print("adc1_ch5=");     Serial1.println(analogReadChannel(0, 5));    // 0x555>>2 = 341
    Serial1.print("adc2_ch3=");     Serial1.println(analogReadChannel(1, 3));    // 0x333>>2 = 204
    analogReadResolution(12);
    Serial1.print("adc1_ch15_12b="); Serial1.println(analogReadChannel(0, 15));  // 0xFFF = 4095
    analogReadResolution(10);
    Serial1.print("A0=");           Serial1.println(analogRead(A0));             // ch0 -> 0
    Serial1.println("[adc] done");
}
void loop() {
    // Also print A0 continuously so the hardware test (Task 5) can watch it change.
    Serial1.print("A0="); Serial1.println(analogRead(A0));
    delay(1000);
}
