#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"

void setup() {
    Serial1.begin(115200);
#if defined(__IMXRT1176__)
    Serial1.println("RT1176 Serial1 up");
#elif defined(__IMXRT1062__)
    Serial1.println("RT1062 Serial1 up");
#else
#error "unknown target: expected __IMXRT1176__ or __IMXRT1062__"
#endif
}

void loop() {
    static uint32_t n = 0;
    Serial1.print("count=");
    Serial1.println(n++);
    delay(200);
}
