// nosh_bringup -- the first firmware for a Nosh board (RT1176, Nosh-only build).
//
// One image that walks every subsystem the Nosh port changed, prints a verdict
// line per subsystem on Serial1 (LPUART1, the J11 debug header) and keeps the
// user LED blinking so a board with no console still shows life.  Each verdict
// is a greppable token; the QEMU gate asserts them, and on the bench the same
// transcript is what tells you which part of a fresh board to look at.
//
//   NOSH_BRINGUP ...      banner: board macro, clocks, reset reason
//   SDRAM_DATA / _ADDR / _FILL / SDRAM=PASS|FAIL
//                         the 128 MB SEMC window (two AS4C32M16SC-7TIN at
//                         132 MHz): data lines, address lines, full fill
//   SD=PASS|FAIL(...)     microSD on USDHC2 (GPIO_SD_B2_00..05): card-detect
//                         pin, SdFat mount, write + read-back of one file
//   I2C5_SCAN=DONE n=K    LPI2C5 = Wire2: SGTL5000 (0x0A) and GT911 (0x5D/0x14)
//                         on hardware; the WM8962 stub (0x1A) in QEMU
//   M2_SDIO=ok|...        uSDHC1 SDIO enumeration of the M.2 module after the
//                         reset preamble (no VSELECT write on Nosh)
//   BRINGUP=PASS|FAIL     SDRAM and SD both passed (I2C and M.2 are reported,
//                         not required: an empty M.2 socket is a valid board)
//   alive=N               once a second from loop(), LED toggling at 2 Hz
//
// QEMU: the gate boots the EVKB machine with sdram-size=128M, the card image on
// -drive index 1 (USDHC2) and m2-wifi=on so the IW416 model answers CMD5.
#include <Arduino.h>
#include <Wire.h>
#include <SD.h>
#include <SdioHost.h>
#include <string.h>

#if !defined(ARDUINO_NOSH)
#error "nosh_bringup is Nosh-only: configure with -DEVKB_BOARD=nosh and toolchain/nosh.toolchain.cmake"
#endif

#define SDRAM_BASE   0x80000000u
#define SDRAM_BYTES  (SEMC_SDRAM_MB * 1024u * 1024u)
#define SDRAM_WORDS  (SDRAM_BYTES / 4u)

extern uint32_t _extram_start, _extram_end;   // .bss.extram (imxrt1176_nosh.ld)
extern "C" uint8_t external_psram_size;       // startup.c: SEMC_SDRAM_MB, set once the window is live

static SdioHost sdio;
static bool g_sdramPass = false, g_sdPass = false;
static int  g_i2cFound = -1;
static SdioHost::Status g_m2 = SdioHost::CMD_TIMEOUT;

static const char *sdioStatusName(SdioHost::Status s) {
	switch (s) {
	case SdioHost::OK:               return "ok";
	case SdioHost::NO_IO_FUNCTION:   return "no-io-function";
	case SdioHost::CMD_TIMEOUT:      return "cmd-timeout";
	case SdioHost::CMD_CRC:          return "cmd-crc";
	case SdioHost::CLOCK_UNSTABLE:   return "clock-unstable";
	case SdioHost::BAD_CIS:          return "bad-cis";
	case SdioHost::CMD5_NO_RESPONSE: return "cmd5-no-response";
	case SdioHost::INIT_CLK_STUCK:   return "init-clk-stuck";
	}
	return "?";
}

// ---------------------------------------------------------------------------
// SDRAM.  startup.c has already run semc_sdram_init() and zeroed .bss.extram,
// so the window is live.  Three tests, cheapest first:
//   data lines   walking 1 / walking 0 in a few words (all 32 DQ lines, both
//                chips, both directions)
//   address lines power-of-two strides over the whole 128 MB: word offsets
//                0, 1, 2, ..., 2^24 each get a distinct value, then all are read
//                back.  A shorted or unconnected address line, or a SEMC COL/MS
//                setting that aliases the top half onto the bottom (the 64 MB
//                EVKB configuration on a 128 MB part does exactly that), makes
//                two offsets land on one cell and the read-back disagree.
//   full fill    every free word written with a function of its own address,
//                then read back: the refresh and timing check, and the one
//                that costs real time (about 3 s on silicon).
// ---------------------------------------------------------------------------
EXTMEM static uint32_t g_dataWords[64];

static bool sdramDataLines() {
	static const unsigned idx[] = { 0, 1, 31, 63 };
	for (unsigned k = 0; k < sizeof(idx) / sizeof(idx[0]); k++) {
		volatile uint32_t *p = &g_dataWords[idx[k]];
		for (int bit = 0; bit < 32; bit++) {
			uint32_t one = 1u << bit;
			*p = one;  arm_dcache_flush_delete((void *)p, 4);
			if (*p != one) { Serial1.printf("SDRAM_DATA=FAIL word=%u bit=%d read=0x%08lx\n", idx[k], bit, (unsigned long)*p); return false; }
			*p = ~one; arm_dcache_flush_delete((void *)p, 4);
			if (*p != ~one) { Serial1.printf("SDRAM_DATA=FAIL word=%u ~bit=%d read=0x%08lx\n", idx[k], bit, (unsigned long)*p); return false; }
		}
	}
	Serial1.println("SDRAM_DATA=PASS");
	return true;
}

static bool sdramAddressLines() {
	volatile uint32_t *ram = (volatile uint32_t *)SDRAM_BASE;
	uint32_t offs[32];
	int n = 0;
	offs[n++] = 0;
	for (uint32_t s = 1; s < SDRAM_WORDS; s <<= 1) offs[n++] = s;
	// The strides land inside .bss.extram and the free area alike; save what
	// they overwrite so the statics above survive.
	uint32_t saved[32];
	for (int i = 0; i < n; i++) saved[i] = ram[offs[i]];
	for (int i = 0; i < n; i++) { ram[offs[i]] = 0xC0DE0000u | (uint32_t)i; arm_dcache_flush_delete((void *)&ram[offs[i]], 4); }
	bool ok = true;
	for (int i = 0; i < n; i++) {
		uint32_t v = ram[offs[i]];
		if (v != (0xC0DE0000u | (uint32_t)i)) {
			Serial1.printf("SDRAM_ADDR=FAIL stride_word=0x%08lx read=0x%08lx want=0x%08lx\n",
			               (unsigned long)offs[i], (unsigned long)v, (unsigned long)(0xC0DE0000u | i));
			ok = false;
			break;
		}
	}
	for (int i = 0; i < n; i++) ram[offs[i]] = saved[i];
	if (ok) Serial1.printf("SDRAM_ADDR=PASS strides=%d top_word=0x%08lx\n", n, (unsigned long)offs[n - 1]);
	return ok;
}

static bool sdramFill() {
	// Free SDRAM: from the end of .bss.extram (rounded up) to the top.
	uint32_t start = ((uint32_t)&_extram_end + 31u) & ~31u;
	uint32_t end   = SDRAM_BASE + SDRAM_BYTES;
	volatile uint32_t *p = (volatile uint32_t *)start;
	uint32_t words = (end - start) / 4u;
	uint32_t t0 = millis();
	for (uint32_t i = 0; i < words; i++) p[i] = (start + 4u * i) ^ 0x5A5A5A5Au;
	uint32_t t1 = millis();
	uint32_t errors = 0, firstBad = 0;
	for (uint32_t i = 0; i < words; i++) {
		uint32_t want = (start + 4u * i) ^ 0x5A5A5A5Au;
		if (p[i] != want) { if (!errors) firstBad = start + 4u * i; errors++; }
	}
	uint32_t t2 = millis();
	Serial1.printf("SDRAM_FILL=%s mb=%lu errors=%lu write_ms=%lu read_ms=%lu",
	               errors ? "FAIL" : "PASS", (unsigned long)((end - start) >> 20),
	               (unsigned long)errors, (unsigned long)(t1 - t0), (unsigned long)(t2 - t1));
	if (errors) Serial1.printf(" first_bad=0x%08lx", (unsigned long)firstBad);
	Serial1.println();
	return errors == 0;
}

// ---------------------------------------------------------------------------
// microSD on USDHC2.  Card-detect is GPIO_AD_34 (pin 22, low = card); the slot
// supply enable GPIO_AD_35 (pin 23) is pulled low = on at reset, so the card is
// powered without firmware doing anything.  SD.begin(BUILTIN_SDCARD) on Nosh
// reads the card-detect pin first, then brings USDHC2 up through SdFat.
// ---------------------------------------------------------------------------
static bool sdTest() {
	pinMode(NOSH_PIN_SD_CD, INPUT);
	int cd = digitalRead(NOSH_PIN_SD_CD);
	int pwr = digitalRead(NOSH_PIN_SD_PWREN);
	Serial1.printf("sd: cd=%d (%s) pwren_b=%d (%s)\n", cd, cd ? "no card" : "card present",
	               pwr, pwr ? "OFF" : "on");
	if (!SD.begin(BUILTIN_SDCARD)) { Serial1.println("SD=FAIL(begin)"); return false; }
	uint32_t sectors = SD.sdfs.card() ? SD.sdfs.card()->sectorCount() : 0;
	Serial1.printf("sd: usdhc2 up, sectors=%lu (%lu MB), fat_type=%d\n",
	               (unsigned long)sectors, (unsigned long)(sectors / 2048u), (int)SD.sdfs.fatType());

	const char *fn = "NOSHBRNG.TXT";
	const char *payload = "Nosh bring-up: SDRAM 128 MB, microSD on USDHC2";
	if (SD.exists(fn)) SD.remove(fn);
	File w = SD.open(fn, FILE_WRITE);
	if (!w) { Serial1.println("SD=FAIL(open-write)"); return false; }
	w.print(payload);
	w.close();
	File r = SD.open(fn);
	if (!r) { Serial1.println("SD=FAIL(open-read)"); return false; }
	char buf[80];
	int n = 0;
	while (r.available() && n < (int)sizeof(buf) - 1) buf[n++] = (char)r.read();
	buf[n] = 0;
	r.close();
	if (strcmp(buf, payload) != 0) { Serial1.printf("SD=FAIL(mismatch) read='%s'\n", buf); return false; }
	Serial1.println("SD=PASS");
	return true;
}

// ---------------------------------------------------------------------------
// LPI2C5 = Wire2: the only I2C bus on Nosh (GPIO_LPSR_04/05).  Codec SGTL5000
// at 0x0A, GT911 touch at 0x5D (or 0x14 depending on its address strap at
// reset).  The panel power enable is pulled up, so the GT911 is powered.
// Reported, not asserted: a board built without the display is still a board.
// ---------------------------------------------------------------------------
static const char *i2cName(uint8_t a) {
	switch (a) {
	case 0x0A: return "SGTL5000";
	case 0x5D: case 0x14: return "GT911";
	case 0x1A: return "WM8962 (QEMU EVKB stub)";
	case 0x45: return "ATtiny88 panel MCU (QEMU EVKB stub)";
	case 0x50: return "AT24C02 (QEMU fixture)";
	}
	return "?";
}

static int i2cScan() {
	Wire2.begin();
	Wire2.setClock(100000);
	int found = 0;
	for (uint8_t a = 0x08; a < 0x78; a++) {
		Wire2.beginTransmission(a);
		if (Wire2.endTransmission() == 0) {
			Serial1.printf("i2c5: ack 0x%02X %s\n", a, i2cName(a));
			found++;
		}
	}
	Serial1.printf("I2C5_SCAN=DONE n=%d\n", found);
	return found;
}

// ---------------------------------------------------------------------------
// M.2 Key E socket on uSDHC1.  Release the three reset lines (SDIO_RESET#,
// W_DISABLE1# = WL_RST, W_DISABLE2# = BT_RST) the way M2Radio's preamble does,
// then enumerate.  NVCC_SD1 is fixed at 1.8 V on Nosh, so useIoVoltage1V8()
// is a request the board already honours and begin() writes no VSELECT.
// ---------------------------------------------------------------------------
static SdioHost::Status m2Probe() {
	pinMode(NOSH_PIN_SDIO_RST, OUTPUT); digitalWrite(NOSH_PIN_SDIO_RST, LOW);
	pinMode(NOSH_PIN_WL_RST, OUTPUT);   digitalWrite(NOSH_PIN_WL_RST, LOW);
	pinMode(NOSH_PIN_BT_RST, OUTPUT);   digitalWrite(NOSH_PIN_BT_RST, LOW);
	delay(20);
	digitalWrite(NOSH_PIN_SDIO_RST, HIGH);
	delay(10);
	digitalWrite(NOSH_PIN_WL_RST, HIGH);
	digitalWrite(NOSH_PIN_BT_RST, HIGH);
	delay(100);   // module power-on reset before it will answer CMD5
	pinMode(NOSH_PIN_WIFI_WAKE, INPUT);
	pinMode(NOSH_PIN_BT_WAKE, INPUT);

	sdio.useIoVoltage1V8(true);
	SdioHost::Status st = sdio.begin();
	Serial1.printf("M2_SDIO=%s funcs=%u rca=0x%04x int_status=0x%08lx r4=0x%08lx wifi_wake=%d bt_wake=%d\n",
	               sdioStatusName(st), sdio.ioFunctionCount(), sdio.rca(),
	               (unsigned long)sdio.lastIntStatus(), (unsigned long)sdio.lastR4(),
	               digitalRead(NOSH_PIN_WIFI_WAKE), digitalRead(NOSH_PIN_BT_WAKE));
	return st;
}

void setup() {
	pinMode(LED_BUILTIN, OUTPUT);
	digitalWrite(LED_BUILTIN, HIGH);
	Serial1.begin(115200);
	while (!Serial1 && millis() < 2000) {}

	Serial1.println();
	Serial1.printf("NOSH_BRINGUP board=nosh core=imxrt1176 pins=%d build=%s %s\n",
	               (int)CORE_NUM_DIGITAL, __DATE__, __TIME__);
	Serial1.printf("clocks: cpu=%lu bus=%lu semc=%lu sdram_mb=%u\n",
	               (unsigned long)F_CPU_ACTUAL, (unsigned long)F_BUS_ACTUAL,
	               (unsigned long)SEMC_CLOCK_HZ, (unsigned)external_psram_size);
	uint32_t srsr = SRC_SRSR;
	Serial1.printf("reset: srsr=0x%08lx%s%s%s\n", (unsigned long)srsr,
	               (srsr & 0x00010001u) ? " POR" : "",
	               (srsr & (1u << 2))   ? " M7_LOCKUP" : "",
	               (srsr & (1u << 12))  ? " M4_LOCKUP" : "");
	Serial1.printf("extram: static=%lu bytes (0x%08lx..0x%08lx)\n",
	               (unsigned long)((uint32_t)&_extram_end - (uint32_t)&_extram_start),
	               (unsigned long)(uint32_t)&_extram_start, (unsigned long)(uint32_t)&_extram_end);

	// USB host port J2: ID low = an A-plug is in (VBUS switched on by hardware);
	// FAULT low = the TPS2553 tripped.  Both are inputs with pull-ups.
	pinMode(NOSH_PIN_USB2_ID, INPUT);
	pinMode(NOSH_PIN_USB2_OC, INPUT);
	Serial1.printf("usb2: id=%d (%s) fault=%d\n", digitalRead(NOSH_PIN_USB2_ID),
	               digitalRead(NOSH_PIN_USB2_ID) ? "no A-plug" : "A-plug, VBUS on",
	               digitalRead(NOSH_PIN_USB2_OC));

	g_sdramPass = sdramDataLines() && sdramAddressLines() && sdramFill();
	Serial1.printf("SDRAM=%s\n", g_sdramPass ? "PASS" : "FAIL");

	g_sdPass   = sdTest();
	g_i2cFound = i2cScan();
	g_m2       = m2Probe();

	Serial1.printf("BRINGUP_SUMMARY sdram=%s sd=%s i2c5=%d m2=%s\n",
	               g_sdramPass ? "PASS" : "FAIL", g_sdPass ? "PASS" : "FAIL",
	               g_i2cFound, sdioStatusName(g_m2));
	Serial1.printf("BRINGUP=%s\n", (g_sdramPass && g_sdPass) ? "PASS" : "FAIL");
	digitalWrite(LED_BUILTIN, LOW);
}

void loop() {
	static uint32_t alive = 0;
	static uint32_t lastAlive = 0, lastLed = 0;
	static bool led = false;
	uint32_t now = millis();
	if (now - lastLed >= 250) { lastLed = now; led = !led; digitalWrite(LED_BUILTIN, led); }
	if (now - lastAlive >= 1000) {
		lastAlive = now;
		Serial1.printf("alive=%lu\n", (unsigned long)++alive);
	}
}
