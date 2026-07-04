#include "Arduino.h"
#include "HardwareSerial.h"

volatile int g_count = 0;
static void onIrq() { g_count++; }

static void pulse(int n) {                 // n high/low toggles on D13 (wired to D9)
	for (int i = 0; i < n; i++) {
		digitalWrite(13, HIGH); delayMicroseconds(50);
		digitalWrite(13, LOW);  delayMicroseconds(50);
	}
}

void setup() {
	Serial1.begin(115200); while (!Serial1) {}
	pinMode(13, OUTPUT); pinMode(9, INPUT); digitalWrite(13, LOW);
	bool ok = true;

	attachInterrupt(9, onIrq, RISING);     // 5 rising edges
	g_count = 0; pulse(5); int rising = g_count; if (rising != 5) ok = false;

	attachInterrupt(9, onIrq, CHANGE);     // 5 toggles = 10 edges
	g_count = 0; pulse(5); int change = g_count; if (change != 10) ok = false;

	detachInterrupt(9);                    // no more interrupts
	g_count = 0; pulse(5); int detached = g_count; if (detached != 0) ok = false;

	Serial1.print("rising="); Serial1.print(rising);
	Serial1.print(" change="); Serial1.print(change);
	Serial1.print(" detached="); Serial1.println(detached);
	Serial1.println(ok ? "IRQ=PASS" : "IRQ=FAIL");
}
void loop() {}
