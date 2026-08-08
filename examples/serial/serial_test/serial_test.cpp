#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"

// The console is LPUART1 on BOTH boards. The two cores just name it
// differently: cores/imxrt1176 calls LPUART1 `Serial1`, while cores/teensy4
// follows the Teensy pin-0/1 convention and calls LPUART6 `Serial1` and LPUART1
// `Serial6`. Naming it once here is what keeps QEMU and silicon reading the same
// wire -- on the MIMXRT1060-EVKB, LPUART1 (GPIO_AD_B0_12/13) is the DAPLink VCOM,
// whereas LPUART6 only reaches Arduino header pins D0/D1.
#if defined(ARDUINO_MIMXRT1060_EVKB)
#define CONSOLE Serial6
#else
#define CONSOLE Serial1
#endif

void setup() {
    CONSOLE.begin(115200);
#if defined(__IMXRT1176__)
    CONSOLE.println("RT1176 Serial1 up");
#elif defined(__IMXRT1062__)
    CONSOLE.println("RT1062 Serial6 up");
#else
#error "unknown target: expected __IMXRT1176__ or __IMXRT1062__"
#endif
}

void loop() {
    static uint32_t n = 0;
    CONSOLE.print("count=");
    CONSOLE.println(n++);
    delay(200);
}
