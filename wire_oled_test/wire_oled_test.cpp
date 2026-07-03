#include "Arduino.h"
#include "HardwareSerial.h"
#include "Wire.h"

// RT1170 EVKB = I2C MASTER, talking to an Arduino MKR Zero configured as an I2C
// SLAVE at 0x42. Scans the bus, writes 2 bytes, then reads back the slave's
// fixed 4-byte response pattern (0x11 0x22 0x33 0x44).
#define SLAVE 0x42

static void print_hex(uint8_t v) {
	const char *h = "0123456789ABCDEF";
	Serial1.print(h[v >> 4]); Serial1.print(h[v & 0xF]);
}

void setup() {
	Serial1.begin(115200);
	while (!Serial1) { }
	Wire.begin();
	Wire.setClock(100000);
	Serial1.println("RT1170 I2C master <-> MKR Zero slave @0x42");
}

void loop() {
	// 1. Bus scan.
	Serial1.print("scan:");
	int n = 0;
	for (uint8_t a = 1; a < 0x7F; a++) {
		Wire.beginTransmission(a);
		if (Wire.endTransmission() == 0) { Serial1.print(" 0x"); print_hex(a); n++; }
	}
	if (n == 0) Serial1.print(" NONE");
	Serial1.println();

	// 2. Write 2 bytes to the slave.
	Wire.beginTransmission(SLAVE);
	Wire.write((uint8_t)0xA5); Wire.write((uint8_t)0x5A);
	Serial1.print("wr_status="); Serial1.println(Wire.endTransmission());

	// 3. Read back the slave's 4-byte response (expect 11 22 33 44).
	uint8_t got = Wire.requestFrom((uint8_t)SLAVE, (uint8_t)4, true);
	Serial1.print("rd("); Serial1.print(got); Serial1.print(")=");
	while (Wire.available()) { print_hex((uint8_t)Wire.read()); Serial1.print(' '); }
	Serial1.println();

	delay(1000);
}
