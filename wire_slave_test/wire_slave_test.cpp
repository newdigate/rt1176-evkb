#include "Arduino.h"
#include "HardwareSerial.h"
#include "Wire.h"

volatile int rx_count = 0;
volatile uint8_t b0 = 0, b1 = 0, b2 = 0;

void onRecv(int n) { rx_count = n; b0 = Wire1.read(); b1 = Wire1.read(); b2 = Wire1.read(); }
void onReq(void)   { Wire1.write((uint8_t)0x11); }   // single-byte response

static void hex2(uint8_t v) { const char* h="0123456789ABCDEF"; Serial1.print(h[v>>4]); Serial1.print(h[v&0xF]); }

void setup() {
	Serial1.begin(115200); while (!Serial1) {}
	Wire1.begin((uint8_t)0x42);              // slave @ 0x42 on the loopback bus
	Wire1.onReceive(onRecv);
	Wire1.onRequest(onReq);
	Wire.begin(); Wire.setClock(100000);     // master

	// Master writes 3 bytes to the slave -> onReceive should fire with 3 bytes.
	Wire.beginTransmission(0x42);
	Wire.write((uint8_t)0xAA); Wire.write((uint8_t)0xBB); Wire.write((uint8_t)0xCC);
	uint8_t w = Wire.endTransmission();
	delayMicroseconds(500);

	Serial1.print("rx_count="); Serial1.println(rx_count);
	Serial1.print("rx=0x"); hex2(b0); Serial1.print(" 0x"); hex2(b1); Serial1.print(" 0x"); hex2(b2); Serial1.println();
	Serial1.print("wr_status="); Serial1.println(w);

	// Master reads 1 byte from the slave -> onRequest should fire and return 0x11.
	uint8_t got = Wire.requestFrom((uint8_t)0x42, (uint8_t)1, true);
	uint8_t rb = Wire.available() ? (uint8_t)Wire.read() : 0;
	Serial1.print("rd("); Serial1.print(got); Serial1.print(")=0x"); hex2(rb); Serial1.println();
}
void loop() {}
