#include "Arduino.h"
#include "HardwareSerial.h"
// FlexPWM analogWrite demo/verification: sweep duty on D9 (FLEXPWM1_PWM0_B) at
// 1 kHz so a Saleae on D9 can measure duty% + frequency. The green LED (D3)
// brightness tracks the sweep via analogWrite -> exercises the new pin table.
void setup() {
	Serial1.begin(115200); while (!Serial1) {}
	analogWriteFrequency(9, 1000.0f);
	Serial1.println("pwm: D9 duty sweep @1kHz (0,64,128,192,255)");
}
void loop() {
	static const int duty[] = {0, 64, 128, 192, 255};
	static int i = 0;
	analogWrite(9, duty[i]);
	analogWrite(LED_BUILTIN, duty[i]);
	Serial1.print("duty="); Serial1.println(duty[i]);
	i = (i + 1) % 5;
	delay(1500);
}
