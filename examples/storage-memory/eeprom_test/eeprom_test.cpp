#include "Arduino.h"
#include "HardwareSerial.h"
#include <EEPROM.h>

struct Settings { uint32_t magic; int16_t a; int16_t b; char tag[6]; };

// Persistence marker, at a fixed address so it can be found again after a
// reboot. 4200..4211 (12 bytes); 4211 is inside E2END (4283). Stage RW writes
// 0..255 and put() writes 1000..1015, so neither overlaps the marker itself --
// though RW DOES share the marker's flash sectors, which matters when reading
// the raw array (see transcript_hw_evkb.txt).
//
// `boots` is what makes the hardware persistence test discriminating, and it is
// the whole reason this struct is not just magic+payload. A marker that was
// LOST to power loss gets recreated by the next boot at boots=1; one that
// SURVIVED carries its count forward. Without a value the sketch cannot
// reproduce from a blank slate, "survived" and "deterministically recreated"
// are indistinguishable -- in the console output AND in a raw flash dump, since
// recreation rewrites the same bytes at the same offsets in the same order.
struct BootMark { uint32_t magic; uint32_t payload; uint32_t boots; };
static const int  BOOT_ADDR    = 4200;
static const uint32_t BOOT_MAGIC   = 0xB007EE99;
static const uint32_t BOOT_PAYLOAD = 0xCAFEF00D;

void setup() {
	Serial1.begin(115200);
	while (!Serial1) {}

	// --- Stage BOOT: persistence marker, read BEFORE anything else writes ---
	// QEMU boots from -kernel with no backing store behind the FlexSPI window,
	// so nothing written survives the process: this is deterministically FIRST
	// there, and the gate asserts exactly that. On the EVKB it reads RETURN
	// after a power-cycle with no reflash.
	//
	// RETURN alone is NOT proof that the data persisted. The board runs this
	// sketch on power-up before any console can attach, so a marker lost to
	// power loss would be silently recreated by that unobserved boot and every
	// later reset would still print RETURN.
	//
	// `boots` is what settles it: it increments on every boot that finds the
	// marker, so it is cumulative state no fresh boot can fabricate. Read it
	// straight out of the NOR array over SWD before and after removing power --
	// carried forward means the data survived, back to 1 means it did not. That
	// works without catching any particular boot on the console, which is the
	// difficulty this test kept running into. See transcript_hw_evkb.txt.
	BootMark bm;
	EEPROM.get(BOOT_ADDR, bm);
	uint32_t boots;
	if (bm.magic == BOOT_MAGIC) {
		boots = bm.boots + 1;
		bm.boots = boots;
		// Rewrites all 12 bytes, but only the counter word's bytes actually
		// change: eeprom_write_byte elides a write whose value is unchanged
		// (eeprom.c:137), so magic and payload cost nothing here.
		EEPROM.put(BOOT_ADDR, bm);
		Serial1.print("EEPROM_BOOT=RETURN payload=0x"); Serial1.print(bm.payload, HEX);
		Serial1.print(" boots="); Serial1.println(boots);
		Serial1.print("EEPROM_PERSIST_HW=");
		// boots > 1 is the load-bearing half: a recreated marker reports 1.
		Serial1.println((bm.payload == BOOT_PAYLOAD && boots > 1) ? "PASS" : "FAIL");
	} else {
		BootMark w = { BOOT_MAGIC, BOOT_PAYLOAD, 1 };
		EEPROM.put(BOOT_ADDR, w);
		boots = 1;
		Serial1.println("EEPROM_BOOT=FIRST boots=1");
	}

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

	// --- Stage API: the surface beyond read/write/get/put/length ---
	// update(), operator[] in both directions, proxy-to-proxy assignment, and
	// range-for iteration. Without this stage the wider API is compiled but
	// never executed, which is how an API regression reaches hardware.
	// Addresses 300..302 are outside every other stage's range.
	bool api_ok = true;
	EEPROM.update(300, 0x11);
	if (EEPROM.read(300) != 0x11) api_ok = false;
	EEPROM.update(300, 0x11);                       // the unchanged-value path
	if (EEPROM.read(300) != 0x11) api_ok = false;
	EEPROM[301] = 0x22;                             // proxy write
	if (EEPROM.read(301) != 0x22) api_ok = false;
	uint8_t via_proxy = EEPROM[301];                // proxy read
	if (via_proxy != 0x22) api_ok = false;
	EEPROM[302] = EEPROM[301];                      // must copy the VALUE, not rebind
	if (EEPROM.read(302) != 0x22) api_ok = false;
	uint32_t seen = 0;
	for (uint8_t b : EEPROM) { (void)b; seen++; }   // begin()/end()/EEPtr/EERef
	if (seen != EEPROM.length()) api_ok = false;
	Serial1.print("EEPROM_API="); Serial1.println(api_ok ? "PASS" : "FAIL");

	// --- Stage PERSIST: the cold-start path, without a reboot ---
	// eeprom_initialize() re-scans all 63 sectors and rebuilds sector_index[]
	// (cores/imxrt1176/eeprom.c:77) -- exactly what runs on a fresh boot against
	// flash that already holds data. Calling it again proves the rescan
	// reconstructs the same view of everything written above, including across
	// the sector the wear stage erased and compacted.
	// It does NOT prove data survives a power cycle; only the hardware run can,
	// via EEPROM_BOOT above. Do not let this stage's name suggest otherwise.
	bool persist_ok = true;
	eeprom_initialize();
	Settings r2;
	EEPROM.get(1000, r2);
	if (r2.magic != s.magic || r2.a != s.a || r2.b != s.b || strcmp(r2.tag, s.tag) != 0) persist_ok = false;
	if (EEPROM.read(A) != (uint8_t)2099) persist_ok = false;   // last wear value
	if (EEPROM.read(B) != 0x5A) persist_ok = false;            // its neighbour
	for (int i = 0; i < 256; i++) {
		if (i == A || i == B) continue;                        // rewritten by stage WEAR
		if (EEPROM.read(i) != (uint8_t)(i * 7 + 3)) { persist_ok = false; break; }
	}
	BootMark bm2;
	EEPROM.get(BOOT_ADDR, bm2);
	if (bm2.magic != BOOT_MAGIC || bm2.payload != BOOT_PAYLOAD) persist_ok = false;
	// The counter must read back as what this boot just stored -- proves the
	// increment reached flash and survived the rescan, not just the RAM copy.
	if (bm2.boots != boots) persist_ok = false;
	Serial1.print("EEPROM_PERSIST="); Serial1.println(persist_ok ? "PASS" : "FAIL");

	Serial1.print("EEPROM_LENGTH="); Serial1.println(EEPROM.length());   // expect 4284
	Serial1.print("EEPROM_ALL=");
	Serial1.println((rw_ok && wear_ok && api_ok && persist_ok) ? "PASS" : "FAIL");
}
void loop() {}
