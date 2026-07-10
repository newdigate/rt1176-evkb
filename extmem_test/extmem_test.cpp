#include "Arduino.h"
#include "HardwareSerial.h"
#include "wiring.h"    // extmem_malloc / _free / _calloc / _realloc

#define IS_EXTMEM(p) (((uint32_t)(p) >> 28) == 8)   // SDRAM 0x8000_0000..0x83FF_FFFF

void setup()
{
	Serial1.begin(115200);
	while (!Serial1) {}
	Serial1.println("EXTMEM_INIT");

	// 1. malloc: an SDRAM pointer, write + read-back
	uint32_t *a = (uint32_t *)extmem_malloc(4096);
	bool alloc_ok = (a != NULL) && IS_EXTMEM(a);
	if (alloc_ok) {
		for (int i = 0; i < 1024; i++) a[i] = 0xC0DE0000u + (uint32_t)i;
		for (int i = 0; i < 1024; i++) if (a[i] != 0xC0DE0000u + (uint32_t)i) { alloc_ok = false; break; }
	}
	Serial1.print("EXTMEM_ALLOC="); Serial1.println(alloc_ok ? "PASS" : "FAIL");

	// 2. calloc: zeroed SDRAM
	uint32_t *c = (uint32_t *)extmem_calloc(256, sizeof(uint32_t));
	bool calloc_ok = (c != NULL) && IS_EXTMEM(c);
	if (calloc_ok) for (int i = 0; i < 256; i++) if (c[i] != 0u) { calloc_ok = false; break; }
	Serial1.print("EXTMEM_CALLOC="); Serial1.println(calloc_ok ? "PASS" : "FAIL");

	// 3. realloc: contents preserved across a grow (which may move)
	uint32_t *r = (uint32_t *)extmem_realloc(a, 16384);   // a is consumed
	bool realloc_ok = (r != NULL) && IS_EXTMEM(r);
	if (realloc_ok) for (int i = 0; i < 1024; i++) if (r[i] != 0xC0DE0000u + (uint32_t)i) { realloc_ok = false; break; }
	Serial1.print("EXTMEM_REALLOC="); Serial1.println(realloc_ok ? "PASS" : "FAIL");

	// 4. free + re-malloc still lands in SDRAM
	extmem_free(r);
	extmem_free(c);
	uint32_t *d = (uint32_t *)extmem_malloc(4096);
	bool free_ok = (d != NULL) && IS_EXTMEM(d);
	Serial1.print("EXTMEM_FREE="); Serial1.println(free_ok ? "PASS" : "FAIL");
	extmem_free(d);

	// 5. fallback: a request larger than the whole 64 MB pool degrades to NULL
	void *huge = extmem_malloc((size_t)100 * 1024 * 1024);
	bool fallback_ok = (huge == NULL);
	if (huge) extmem_free(huge);
	Serial1.print("EXTMEM_FALLBACK="); Serial1.println(fallback_ok ? "PASS" : "FAIL");

	bool all = alloc_ok && calloc_ok && realloc_ok && free_ok && fallback_ok;
	Serial1.print("EXTMEM_TEST="); Serial1.println(all ? "PASS" : "FAIL");
}

void loop() {}
