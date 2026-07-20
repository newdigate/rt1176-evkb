#include "Arduino.h"
#include "HardwareSerial.h"
#include "Wire.h"

// Minimal SSD1306 128x64 driver over LPI2C1 (Arduino header). The EVKB is the
// I2C master; the OLED is a slave at 0x3C (some boards 0x3D). We scan the bus,
// report what we find over Serial1 (VCOM @115200), init the panel, draw a
// framebuffer, then blink the display invert so it is visibly "alive".

static uint8_t OLED_ADDR = 0x3C;
static int     g_found    = -1;
static int     g_count    = 0;     // how many addresses ACKed (all-ACK => stuck bus)
static bool    g_has3c    = false; // did 0x3C specifically ACK?
#define OLED_W 128
#define OLED_H 64
static uint8_t fb[OLED_W * OLED_H / 8];   // 1024-byte framebuffer, 8 pages

// ---- classic 5x7 font, only the glyphs we render (column bitmaps, LSB=top) ----
struct Glyph { char c; uint8_t col[5]; };
static const Glyph FONT[] = {
	{' ', {0x00,0x00,0x00,0x00,0x00}},
	{'1', {0x00,0x42,0x7F,0x40,0x00}},
	{'2', {0x42,0x61,0x51,0x49,0x46}},
	{'6', {0x3C,0x4A,0x49,0x49,0x30}},
	{'7', {0x01,0x71,0x09,0x05,0x03}},
	{'C', {0x3E,0x41,0x41,0x41,0x22}},
	{'I', {0x00,0x41,0x7F,0x41,0x00}},
	{'K', {0x7F,0x08,0x14,0x22,0x41}},
	{'O', {0x3E,0x41,0x41,0x41,0x3E}},
	{'R', {0x7F,0x09,0x19,0x29,0x46}},
	{'T', {0x01,0x01,0x7F,0x01,0x01}},
	{'!', {0x00,0x00,0x5F,0x00,0x00}},
	{':', {0x00,0x36,0x36,0x00,0x00}},
	{')', {0x00,0x41,0x22,0x1C,0x00}},
};

static void print_hex(uint8_t v) {
	const char *h = "0123456789ABCDEF";
	Serial1.print(h[v >> 4]); Serial1.print(h[v & 0xF]);
}

// Reliable address probe: issue START+addr(W)+STOP at the register level, wait
// for the STOP to actually complete (SDF), THEN read NDF. This avoids the
// TDF-races-NDF bug in Wire.endTransmission (TDF asserts a byte-time before the
// ACK is sampled, so every address falsely looks ACKed on real silicon).
// Returns true only if the address was ACKed.
static bool i2c_probe(uint8_t a) {
	LPI2C1_MSR = LPI2C1_MSR;                              // W1C stale flags
	LPI2C1_MTDR = ((uint32_t)4u << 8) | ((uint32_t)(a << 1) | 0u);  // START + addr(W)
	LPI2C1_MTDR = ((uint32_t)2u << 8);                   // STOP
	uint32_t g = 0;
	while (!(LPI2C1_MSR & (1u << 9)) && ++g < 400000u) {} // wait SDF (stop detect)
	bool timeout = (g >= 400000u);
	bool nack    = (LPI2C1_MSR & (1u << 10)) != 0;        // NDF
	LPI2C1_MSR = LPI2C1_MSR;                              // clear
	return !nack && !timeout;
}

static int g_initfail = 0;   // count of init/cmd writes that NACKed

// Register-level checked command write: START+addr + 0x00(control) + cmd + STOP,
// wait for STOP, verify no NACK. Reliable (doesn't rely on Wire's TDF race).
static void oled_cmd(uint8_t c) {
	LPI2C1_MSR  = LPI2C1_MSR;                                   // clear
	LPI2C1_MTDR = ((uint32_t)4u << 8) | ((uint32_t)(OLED_ADDR << 1) | 0u); // START+addr(W)
	LPI2C1_MTDR = ((uint32_t)0u << 8) | 0x00u;                 // control byte (commands)
	LPI2C1_MTDR = ((uint32_t)0u << 8) | c;                    // the command
	LPI2C1_MTDR = ((uint32_t)2u << 8);                        // STOP
	uint32_t g = 0;
	while (!(LPI2C1_MSR & (1u << 9)) && ++g < 400000u) {}      // wait SDF
	if ((LPI2C1_MSR & (1u << 10)) || g >= 400000u) g_initfail++; // NDF or timeout
	LPI2C1_MSR = LPI2C1_MSR;
}

static void oled_init() {
	static const uint8_t seq[] = {
		0xAE,             // display off
		0xD5, 0x80,       // clock divide
		0xA8, 0x3F,       // multiplex = 64
		0xD3, 0x00,       // display offset 0
		0x40,             // start line 0
		0x8D, 0x14,       // charge pump on
		0x20, 0x00,       // memory addressing mode = horizontal
		0xA1,             // segment remap (col 127->SEG0)
		0xC8,             // COM scan direction remapped
		0xDA, 0x12,       // COM pins config
		0x81, 0xCF,       // contrast
		0xD9, 0xF1,       // pre-charge
		0xDB, 0x40,       // VCOMH deselect
		0xA4,             // resume to RAM content
		0xA6,             // normal (non-inverted)
		0xAF,             // display on
	};
	for (unsigned i = 0; i < sizeof(seq); i++) oled_cmd(seq[i]);
}

// Push the whole framebuffer to GDDRAM (horizontal addressing, full window).
static void oled_flush() {
	oled_cmd(0x21); oled_cmd(0x00); oled_cmd(0x7F);   // column 0..127
	oled_cmd(0x22); oled_cmd(0x00); oled_cmd(0x07);   // page 0..7
	// Wire buffer is 32 bytes: send 0x40 + 16 data per transaction.
	for (unsigned i = 0; i < sizeof(fb); i += 16) {
		Wire.beginTransmission(OLED_ADDR);
		Wire.write((uint8_t)0x40);   // Co=0, D/C#=1 -> data
		for (unsigned j = 0; j < 16; j++) Wire.write(fb[i + j]);
		Wire.endTransmission();
	}
}

static void fb_clear() { for (unsigned i = 0; i < sizeof(fb); i++) fb[i] = 0; }

static void fb_pixel(int x, int y, bool on) {
	if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
	uint8_t *p = &fb[x + (y >> 3) * OLED_W];
	uint8_t m = (uint8_t)(1u << (y & 7));
	if (on) *p |= m; else *p &= (uint8_t)~m;
}

static void fb_rect(int x0, int y0, int x1, int y1) {   // outline
	for (int x = x0; x <= x1; x++) { fb_pixel(x, y0, true); fb_pixel(x, y1, true); }
	for (int y = y0; y <= y1; y++) { fb_pixel(x0, y, true); fb_pixel(x1, y, true); }
}

static const Glyph *glyph(char c) {
	for (unsigned i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++)
		if (FONT[i].c == c) return &FONT[i];
	return &FONT[0];   // space fallback
}

// Draw a string at pixel (x,y); scale doubles pixel size for larger text.
static int fb_text(int x, int y, const char *s, int scale) {
	for (; *s; s++) {
		const Glyph *g = glyph(*s);
		for (int col = 0; col < 5; col++) {
			uint8_t bits = g->col[col];
			for (int row = 0; row < 7; row++) {
				if (bits & (1u << row)) {
					for (int sx = 0; sx < scale; sx++)
						for (int sy = 0; sy < scale; sy++)
							fb_pixel(x + col * scale + sx, y + row * scale + sy, true);
				}
			}
		}
		x += (5 + 1) * scale;   // glyph + 1px space
	}
	return x;
}

void setup() {
	Serial1.begin(115200);
	while (!Serial1) { }
	Serial1.println("SSD1306 display test (EVKB master on LPI2C1)");

	Wire.begin();
	Wire.setClock(100000);           // slow bus: marginal pull-ups corrupt bytes at 400k
	delay(150);                       // let the SSD1306 power-on reset (RC) settle

	// Bus scan -> report every device that ACKs.
	Serial1.print("scan:");
	int found = -1;
	int probe_count = 0;
	for (uint8_t a = 1; a < 0x7F; a++) {
		Wire.beginTransmission(a);           // FIXED Wire.endTransmission
		bool wire_ack = (Wire.endTransmission() == 0);
		bool probe_ack = i2c_probe(a);       // raw register probe (reference), same bus
		if (wire_ack) {
			Serial1.print(" 0x"); print_hex(a);
			if (found < 0) found = a;
			g_count++;
			if (a == 0x3C) g_has3c = true;
		}
		if (probe_ack) probe_count++;
	}
	Serial1.print("probe_count="); Serial1.println(probe_count);
	if (found < 0) Serial1.print(" NONE");
	Serial1.println();
	g_found = found;
	if (found == 0x3C || found == 0x3D) OLED_ADDR = (uint8_t)found;
	Serial1.print("using OLED_ADDR=0x"); print_hex(OLED_ADDR); Serial1.println();

	oled_init();
	fb_clear();
	fb_rect(0, 0, 127, 63);
	fb_text(20, 6,  "RT1176", 2);      // big header
	fb_text(28, 34, "I2C OK", 1);
	fb_text(52, 48, ":)", 1);
	oled_flush();
	Serial1.println("drew frame; blinking invert in loop()");
}

void loop() {
	static bool inv = false;
	static uint32_t beat = 0;
	inv = !inv;
	// 0xA5 = force ALL pixels on (ignores RAM) -> definitive "is the panel alive"
	// test; 0xA4 = resume to RAM. If the screen flashes fully white, panel+charge
	// pump work and only our data path is suspect.
	oled_cmd(inv ? 0xA5 : 0xA4);
	// Periodic console echo so the scan result is readable without catching boot.
	Serial1.print("alive beat="); Serial1.print(beat++);
	Serial1.print(" ack_count="); Serial1.print(g_count);
	Serial1.print(" has_0x3C="); Serial1.print(g_has3c ? "YES" : "no");
	Serial1.print(" init_nacks="); Serial1.print(g_initfail);
	Serial1.print(" @0x"); print_hex(OLED_ADDR); Serial1.println();
	delay(600);
}
