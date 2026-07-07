#include "Arduino.h"
#include "HardwareSerial.h"
#include <EEPROM.h>

struct Settings { uint32_t magic; int16_t a; int16_t b; char tag[6]; };

void setup() {
	Serial1.begin(115200);
	while (!Serial1) {}

	// --- Stage RW: byte-level write/read across the address space + a struct ---
	bool rw_ok = true;
	for (int i = 0; i < 256; i++) {
		uint8_t v = (uint8_t)(i * 7 + 3);
		EEPROM.write(i, v);
	}
	for (int i = 0; i < 256; i++) {
		if (EEPROM.read(i) != (uint8_t)(i * 7 + 3)) { rw_ok = false; break; }
	}
	Settings s = { 0xEE9702AA, -1234, 4321, {'R','T','1','1','7','\0'} };
	EEPROM.put(1000, s);
	Settings r;
	EEPROM.get(1000, r);
	if (r.magic != s.magic || r.a != s.a || r.b != s.b || strcmp(r.tag, s.tag) != 0) rw_ok = false;
	Serial1.print("EEPROM_RW="); Serial1.println(rw_ok ? "PASS" : "FAIL");

	// --- Stage WEAR: hammer one address past a full sector to force compaction+erase ---
	// A sector holds 2048 two-byte entries; >2048 changing writes to the same
	// address forces eepromemu_flash_erase_sector + rewrite. Verify the value +
	// a neighbor (same sector) survive the compaction.
	const int A = 42, B = 43;
	EEPROM.write(B, 0x5A);
	bool wear_ok = true;
	for (int n = 0; n < 2100; n++) {
		EEPROM.write(A, (uint8_t)n);       // changes each time -> new entry each time
		if (EEPROM.read(A) != (uint8_t)n) { wear_ok = false; break; }
	}
	if (EEPROM.read(B) != 0x5A) wear_ok = false;   // neighbor survived the erase/compaction
	Serial1.print("EEPROM_WEAR="); Serial1.println(wear_ok ? "PASS" : "FAIL");

	Serial1.print("EEPROM_LENGTH="); Serial1.println(EEPROM.length());   // expect 4284
	Serial1.print("EEPROM_ALL="); Serial1.println((rw_ok && wear_ok) ? "PASS" : "FAIL");
}
void loop() {}
