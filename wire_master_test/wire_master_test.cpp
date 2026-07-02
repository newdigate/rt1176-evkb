#include "Arduino.h"
#include "HardwareSerial.h"
#include "Wire.h"

static void print_hex(uint8_t v) {
	const char *h = "0123456789ABCDEF";
	Serial1.print(h[v >> 4]); Serial1.print(h[v & 0xF]);
}

void setup() {
	Serial1.begin(115200);
	while (!Serial1) { }
	Wire.begin();
	Wire.setClock(400000);

	// 1. Bus scan: find the EEPROM.
	int found = -1;
	for (uint8_t a = 1; a < 0x7F; a++) {
		Wire.beginTransmission(a);
		if (Wire.endTransmission() == 0) { found = a; break; }
	}
	Serial1.print("scan_found=0x"); if (found >= 0) print_hex((uint8_t)found); else Serial1.print("NONE");
	Serial1.println();

	// 2. Write 4 bytes to EEPROM offset 0x00.
	Wire.beginTransmission(0x50);
	Wire.write((uint8_t)0x00);          // internal address pointer
	Wire.write((uint8_t)0xDE); Wire.write((uint8_t)0xAD);
	Wire.write((uint8_t)0xBE); Wire.write((uint8_t)0xEF);
	uint8_t wr = Wire.endTransmission();
	Serial1.print("wr_status="); Serial1.println(wr);

	// 3. Read them back: set pointer to 0x00 (no stop), then read 4.
	Wire.beginTransmission(0x50);
	Wire.write((uint8_t)0x00);
	Wire.endTransmission(false);
	Wire.requestFrom((uint8_t)0x50, (uint8_t)4, true);
	Serial1.print("readback=");
	while (Wire.available()) { print_hex((uint8_t)Wire.read()); Serial1.print(' '); }
	Serial1.println();

	// 4. Absent address must NACK -> status 2.
	Wire.beginTransmission(0x33);
	Serial1.print("absent_status="); Serial1.println(Wire.endTransmission());
}

void loop() { }
