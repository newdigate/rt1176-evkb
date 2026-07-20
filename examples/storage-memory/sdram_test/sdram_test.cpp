#include "Arduino.h"
#include "HardwareSerial.h"

/* ============================================================================
 * sdram_test.cpp — SEMC 64 MB SDRAM memory-test gate (MIMXRT1170-EVKB).
 *
 * The core brings up the SEMC SDRAM at 0x80000000 automatically in startup
 * (semc_sdram_init() runs before __libc_init_array), so by the time setup()
 * runs the 64 MB window is already live and the EXTMEM section is usable.
 * This gate proves the window with two classic memory tests:
 *
 *   - data-line test : walking 1s / walking 0s in a single word, exercising
 *                      all 32 data lines (the board is a 32-bit port = two
 *                      16-bit chips), run over a few EXTMEM words.
 *   - address-line test : power-of-two-strided writes across the FULL 64 MB via
 *                      a raw pointer, then read-back-all — catches address-line
 *                      shorts and aliasing (the highest stride, word 2^23 /
 *                      byte 0x2000000, toggles the MSB address line of the 64 MB
 *                      device, so the strides span the whole window's lines).
 *
 * Markers are printed over Serial1 (LPUART1) @115200 from setup() context only.
 * ========================================================================== */

#define SDRAM_BASE       0x80000000u
#define SDRAM_WORDS      (64u * 1024u * 1024u / 4u)   /* 16M words = 2^24 = full 64 MB */
#define SDRAM_TEST_WORDS 256                          /* modest EXTMEM data-line area (1 KB) */

/* Lands at 0x80000000 (ERAM / .externalram) via the core's EXTMEM macro. */
EXTMEM uint32_t buf[SDRAM_TEST_WORDS];

/* Walking-1s and walking-0s in a single word: drives every one of the 32 data
 * lines both high and low, on a handful of EXTMEM words. */
static bool data_line_test(void)
{
	static const int words[] = { 0, 1, SDRAM_TEST_WORDS / 2, SDRAM_TEST_WORDS - 1 };
	for (unsigned w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
		volatile uint32_t *p = &buf[words[w]];
		for (int i = 0; i < 32; i++) {
			uint32_t pat = (1u << i);            /* walking 1 */
			*p = pat;
			if (*p != pat) return false;
			uint32_t inv = ~pat;                 /* walking 0 */
			*p = inv;
			if (*p != inv) return false;
		}
	}
	return true;
}

/* Power-of-two-strided address test over the full 64 MB window: offset 0 plus
 * 1,2,4,...,2^23 words (< 16M words), each written a distinct value derived from
 * its index, then all read back. A shorted or aliased address line collapses two
 * offsets onto one cell, so the later write clobbers the earlier and the
 * read-back mismatches. */
static bool addr_line_test(void)
{
	volatile uint32_t *ram = (volatile uint32_t *)SDRAM_BASE;
	uint32_t offs[25];
	int n = 0;

	offs[n++] = 0u;
	for (uint32_t o = 1u; o < SDRAM_WORDS; o <<= 1) {   /* 2^0 .. 2^23 -> 24 offsets */
		offs[n++] = o;
	}

	/* Write a distinct value per address first (so aliasing survives to read-back). */
	for (int i = 0; i < n; i++) {
		ram[offs[i]] = 0xA5A50000u + (uint32_t)i;
	}
	/* Read every address back and verify it kept its own distinct value. */
	for (int i = 0; i < n; i++) {
		if (ram[offs[i]] != (0xA5A50000u + (uint32_t)i)) return false;
	}
	return true;
}

/* Diagnostic dump of the SEMC clock chain — read-only, so the HW arbiter run is
 * self-documenting. On silicon these read the real boot-ROM state; the question it
 * answers is whether SYS_PLL2_PFD1 (the 594 MHz SEMC source) is already up from the
 * boot ROM (like SYS_PLL2_PFD2, which the HW-verified SD card rides) or needs an
 * explicit init. A fully-up chain is: PFD1_FRAC=16 (594 MHz), GATE=0, STABLE=1, and
 * CLK_ROOT4=0x602 (MUX=6 SYS_PLL2_PFD1, DIV=2 -> /3 = 198 MHz). Values are raw
 * registers (ANADIG_PLL @0x40C84000, CCM root @0x40CC0200). */
static void dump_semc_clock(void)
{
	uint32_t pll2  = *(volatile uint32_t *)0x40C84240u; /* ANADIG SYS_PLL2_CTRL */
	uint32_t pfd   = *(volatile uint32_t *)0x40C84270u; /* ANADIG SYS_PLL2_PFD  */
	uint32_t root4 = *(volatile uint32_t *)0x40CC0200u; /* CCM CLOCK_ROOT4 (SEMC) */
	uint32_t f = (pfd >> 8) & 0x3Fu, g = (pfd >> 15) & 0x1u, s = (pfd >> 14) & 0x1u;
	Serial1.printf("CLK_PLL2_CTRL=%08lX\n", (unsigned long)pll2);
	Serial1.printf("CLK_PLL2_PFD=%08lX PFD1_FRAC=%lu GATE=%lu STABLE=%lu\n",
	               (unsigned long)pfd, (unsigned long)f, (unsigned long)g, (unsigned long)s);
	Serial1.printf("CLK_ROOT4=%08lX (want 0x602)\n", (unsigned long)root4);
}

/* Refresh-retention test: write a value to one address per MB across the full
 * 64 MB, wait well past the 64 ms auto-refresh window, then verify every value
 * survived. A broken/disabled refresh (bad SDRAMCR2/3 timing) lets un-accessed
 * rows decay in that window, so this is what actually proves refresh is cycling.
 * On QEMU (perfect RAM) it always holds; on silicon it's the real test. */
static bool retention_test(void)
{
	volatile uint32_t *ram = (volatile uint32_t *)SDRAM_BASE;
	const uint32_t stride = 0x100000u / 4u;          /* 1 MB in words */
	for (uint32_t k = 0; k < 64u; k++) {
		ram[k * stride] = 0x5A5A0000u ^ (k * 0x9E3779B1u);
	}
	delay(200);                                       /* >> 64 ms refresh window */
	for (uint32_t k = 0; k < 64u; k++) {
		if (ram[k * stride] != (0x5A5A0000u ^ (k * 0x9E3779B1u))) return false;
	}
	return true;
}

void setup()
{
	Serial1.begin(115200);
	while (!Serial1) {}

	Serial1.println("SDRAM_INIT");
	dump_semc_clock();

	bool data_ok = data_line_test();
	Serial1.print("SDRAM_DATA="); Serial1.println(data_ok ? "PASS" : "FAIL");

	bool addr_ok = addr_line_test();
	Serial1.print("SDRAM_ADDR="); Serial1.println(addr_ok ? "PASS" : "FAIL");

	bool hold_ok = retention_test();
	Serial1.print("SDRAM_HOLD="); Serial1.println(hold_ok ? "PASS" : "FAIL");

	Serial1.print("SDRAM_TEST=");
	Serial1.println((data_ok && addr_ok && hold_ok) ? "PASS" : "FAIL");
}

void loop() {}
