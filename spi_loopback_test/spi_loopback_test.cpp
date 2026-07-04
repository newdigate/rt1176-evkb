#include "Arduino.h"
#include "HardwareSerial.h"
#include "SPI.h"

static void hex2(uint8_t v) { const char* h="0123456789ABCDEF"; Serial1.print(h[v>>4]); Serial1.print(h[v&0xF]); }

void setup() {
	Serial1.begin(115200);
	while (!Serial1) {}
	SPI.begin();

	bool ok = true;
	// Loopback (MOSI tied to MISO): transfer returns what it sent.
	SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
	uint8_t a = SPI.transfer(0xA5);
	uint8_t b = SPI.transfer(0x3C);
	if (a != 0xA5 || b != 0x3C) ok = false;

	uint8_t buf[4] = {0xDE, 0xAD, 0xBE, 0xEF};
	SPI.transfer(buf, 4);
	if (buf[0]!=0xDE || buf[1]!=0xAD || buf[2]!=0xBE || buf[3]!=0xEF) ok = false;

	uint16_t w = SPI.transfer16(0xBEEF);
	if (w != 0xBEEF) ok = false;
	SPI.endTransaction();

	// All four modes + LSB first still echo correctly through loopback.
	const uint8_t modes[4] = {SPI_MODE0, SPI_MODE1, SPI_MODE2, SPI_MODE3};
	for (int m = 0; m < 4; m++) {
		SPI.beginTransaction(SPISettings(1000000, LSBFIRST, modes[m]));
		if (SPI.transfer(0x5A) != 0x5A) ok = false;
		SPI.endTransaction();
	}

	Serial1.print("spi a=0x"); hex2(a); Serial1.print(" b=0x"); hex2(b);
	Serial1.print(" w=0x"); hex2(w>>8); hex2(w&0xFF);
	Serial1.println();
	Serial1.println(ok ? "SPI_LOOPBACK=PASS" : "SPI_LOOPBACK=FAIL");
}
void loop() {}
