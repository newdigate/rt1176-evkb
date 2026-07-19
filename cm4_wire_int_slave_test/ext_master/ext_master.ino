/* Phase 4.2 external I2C master (HW-side oracle for the read-data path).
 *
 * Writes the protocol constants {A5 5A C3} to the CM4 slave @0x42, then reads
 * the 1-byte response, printing both. This printout is the ONLY place the
 * master-observed response byte is asserted (expect rd=3C) — QEMU cannot gate
 * it (the model serves the master's read on the CM7 vCPU with a 0xFF fallback
 * and does not model the TXDSTALL clock-stretch across vCPUs). On silicon,
 * TXDSTALL holds SCL until the CM4 slave ISR loads STDR, so the master reads
 * the real 0x3C.
 *
 * IMPORTANT — two SEPARATE transactions (STOP between write and read). The CM4
 * slave's exchange_complete() requires stops>=2; do NOT use a repeated START
 * (endTransmission(false)) or the slave never sees the second STOP and hangs.
 *
 * 3.3V LOGIC ONLY (e.g. Arduino MKR Zero — the wire_slave_test precedent).
 * NEVER a 5V master without a level shifter: the EVKB pads are not 5V-tolerant.
 *
 * Wiring to the EVKB Arduino header:
 *   master SDA -> EVKB A4 (GPIO_AD_09)
 *   master SCL -> EVKB A5 (GPIO_AD_08)
 *   master GND -> EVKB GND        (ESSENTIAL — a floating ground was the
 *                                  Phase-3.2 I2C flakiness; not optional)
 * The EVKB pads carry internal pull-ups (pad_ctl 0x1E); add external
 * 2.2-4.7 kOhm pull-ups to 3V3 if the bus is marginal.
 */
#include <Wire.h>

void setup() {
  Serial.begin(115200);
  while (!Serial) {}
  Wire.begin();
  Wire.setClock(100000);
}

void loop() {
  // Transaction 1: write the 3 protocol bytes, STOP.
  Wire.beginTransmission(0x42);
  Wire.write(0xA5);
  Wire.write(0x5A);
  Wire.write(0xC3);
  uint8_t w = Wire.endTransmission();     // true => issues STOP

  delay(5);

  // Transaction 2: separate START, read 1 byte, STOP.
  uint8_t n = Wire.requestFrom((uint8_t)0x42, (uint8_t)1);
  int rb = n ? Wire.read() : -1;

  Serial.print("wr=");
  Serial.print(w);                        // expect 0 (all bytes ACKed)
  Serial.print(" rd=");
  Serial.println(rb, HEX);                // expect 3C

  delay(2000);                            // repeats until unplugged
}
