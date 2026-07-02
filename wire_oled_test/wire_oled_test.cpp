#include "Arduino.h"
#include "HardwareSerial.h"
#include "Wire.h"
#define OLED 0x3C

static void print_hex(uint8_t v) {
	const char *h = "0123456789ABCDEF";
	Serial1.print(h[v >> 4]); Serial1.print(h[v & 0xF]);
}
static void cmd(uint8_t c) {
	Wire.beginTransmission(OLED); Wire.write((uint8_t)0x00); Wire.write(c); Wire.endTransmission();
}
static const uint8_t init_seq[] = {
	0xAE,0xD5,0x80,0xA8,0x3F,0xD3,0x00,0x40,0x8D,0x14,0x20,0x00,
	0xA1,0xC8,0xDA,0x12,0x81,0xCF,0xD9,0xF1,0xDB,0x40,0xA4,0xA6,0xAF
};

void setup() {
	Serial1.begin(115200);
	while (!Serial1) { }
	Wire.begin();
	Wire.setClock(400000);

	// Full bus scan (diagnostic: shows what ACKs on LPI2C1).
	Serial1.print("scan:");
	int n = 0;
	for (uint8_t a = 1; a < 0x7F; a++) {
		Wire.beginTransmission(a);
		if (Wire.endTransmission() == 0) { Serial1.print(" 0x"); print_hex(a); n++; }
	}
	if (n == 0) Serial1.print(" NONE");
	Serial1.println();

	// Explicit OLED ACK check.
	Wire.beginTransmission(OLED);
	Serial1.print("oled_ack="); Serial1.println(Wire.endTransmission());   // expect 0

	// Init + draw a checkerboard.
	for (unsigned i = 0; i < sizeof(init_seq); i++) cmd(init_seq[i]);
	cmd(0x21); cmd(0); cmd(127);        // column range 0..127
	cmd(0x22); cmd(0); cmd(7);          // page range 0..7
	for (int page = 0; page < 8; page++) {
		for (int col = 0; col < 128; ) {
			Wire.beginTransmission(OLED);
			Wire.write((uint8_t)0x40);  // data stream
			int cnt = 0;
			while (cnt < 16 && col < 128) { Wire.write((uint8_t)((col & 1) ? 0x55 : 0xAA)); col++; cnt++; }
			Wire.endTransmission();
		}
	}
	Serial1.println("oled_done");
}
void loop() { }
