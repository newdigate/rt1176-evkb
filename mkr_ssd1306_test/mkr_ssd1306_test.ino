// MKR Zero SSD1306 sanity test — no libraries, raw Wire.
// Inits a 128x64 SSD1306 at 0x3C, then flashes the WHOLE panel on/off using the
// controller's "entire display ON" command (0xA5) vs "resume to RAM" (0xA4).
// If the screen does NOT flash fully white, the panel/power/module is the fault
// (not the host), since this is the standard, known-good Arduino I2C path.
//
// Wiring (MKR Zero): SDA = pin 11, SCL = pin 12, VCC = VCC(3.3V), GND = GND.
// Open Serial Monitor @115200 to see status.

#include <Wire.h>

#define OLED_ADDR 0x3C

int initNacks = 0;

// One command byte: [0x00 control][cmd]
void oledCmd(uint8_t c) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write((uint8_t)0x00);
  Wire.write(c);
  if (Wire.endTransmission() != 0) initNacks++;
}

void oledInit() {
  static const uint8_t seq[] = {
    0xAE,             // display off
    0xD5, 0x80,       // clock divide
    0xA8, 0x3F,       // multiplex = 64
    0xD3, 0x00,       // display offset 0
    0x40,             // start line 0
    0x8D, 0x14,       // charge pump ON (internal Vcc)
    0x20, 0x00,       // horizontal addressing
    0xA1,             // segment remap
    0xC8,             // COM scan dec
    0xDA, 0x12,       // COM pins
    0x81, 0xCF,       // contrast
    0xD9, 0xF1,       // pre-charge
    0xDB, 0x40,       // VCOMH
    0xA4,             // resume to RAM
    0xA6,             // normal (non-inverted)
    0xAF,             // display ON
  };
  for (unsigned i = 0; i < sizeof(seq); i++) oledCmd(seq[i]);
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) { }   // wait up to 3s for USB serial

  Wire.begin();
  Wire.setClock(100000);
  delay(150);                                    // let the panel's RC reset settle

  // Bus scan
  Serial.print("scan:");
  bool has3c = false;
  for (uint8_t a = 1; a < 0x7F; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print(" 0x"); Serial.print(a, HEX);
      if (a == 0x3C) has3c = true;
    }
  }
  Serial.println();
  Serial.print("has_0x3C="); Serial.println(has3c ? "YES" : "no");

  oledInit();
  Serial.print("init done, init_nacks="); Serial.println(initNacks);
  Serial.println("flashing ALL PIXELS on/off (0xA5/0xA4) every 600ms...");
}

void loop() {
  static bool on = false;
  on = !on;
  oledCmd(on ? 0xA5 : 0xA4);      // whole panel ON  vs  resume-to-RAM
  Serial.print(on ? "ALL-ON " : "resume ");
  Serial.print("init_nacks="); Serial.println(initNacks);
  delay(600);
}
