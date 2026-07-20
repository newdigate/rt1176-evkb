#include <Arduino.h>
#include "HardwareSerial.h"
#include "I2S.h"
#include "Wire.h"

namespace {
// Self-contained 48 kHz WM8962 init, copied from the (about-to-move) core
// WM8962Codec so this raw-I2SClass test stays independent of the Audio library.
// MUST be the FULL HW-verified sequence -- a reduced subset is silent on HW
// (see the rt1176-i2s-sai note). Anonymous namespace => no clash with the core
// WM8962Codec still compiled via the core glob during this task.
class LocalWM8962 {
public:
    bool begin(TwoWire &b, uint8_t a = 0x1A);
private:
    TwoWire *bus; uint8_t addr;
    bool writeReg(uint16_t reg, uint16_t val);
    bool readReg(uint16_t reg, uint16_t *val);
    bool modifyReg(uint16_t reg, uint16_t mask, uint16_t val);
    bool pollSeqDone();
};
bool LocalWM8962::writeReg(uint16_t reg, uint16_t val) {
    bus->beginTransmission(addr);
    bus->write((uint8_t)0x00); bus->write((uint8_t)reg);
    bus->write((uint8_t)(val >> 8)); bus->write((uint8_t)(val & 0xFF));
    return bus->endTransmission() == 0;
}
bool LocalWM8962::readReg(uint16_t reg, uint16_t *val) {
    bus->beginTransmission(addr);
    bus->write((uint8_t)0x00); bus->write((uint8_t)reg);
    if (bus->endTransmission(false) != 0) return false;
    if (bus->requestFrom(addr, (uint8_t)2) != 2) return false;
    uint8_t hi = bus->read(), lo = bus->read();
    *val = ((uint16_t)hi << 8) | lo; return true;
}
bool LocalWM8962::modifyReg(uint16_t reg, uint16_t mask, uint16_t val) {
    uint16_t regVal; if (!readReg(reg, &regVal)) return false;
    regVal &= (uint16_t)~mask; regVal |= val; return writeReg(reg, regVal);
}
bool LocalWM8962::pollSeqDone() {
    for (int i = 0; i < 100000; i++) {
        uint16_t s; if (!readReg(0x5D, &s)) return false;
        if ((s & 0x1) == 0) return true;
    }
    return false;
}
const uint16_t INIT_PRE[][2]  = { {0x0F,0x6243}, {0x81,0x0000} };
const uint16_t INIT_POST[][2] = { {0x08,0x09E4}, {0x19,0x01FE}, {0x1A,0x01E0} };
const uint16_t SEQ[] = {0x80, 0x92, 0xE8};
const uint16_t VOLUME[][2] = {
    {0x15,0x01C0},{0x16,0x01C0},{0x0A,0x01C0},{0x0B,0x01C0},{0x28,0x01FF},
    {0x29,0x01FF},{0x00,0x013F},{0x01,0x013F},{0x02,0x016B},{0x03,0x016B} };
const uint16_t ROUTE[][2] = {
    {0x25,0x0018},{0x26,0x0012},{0x22,0x0009},{0x69,0x0000},{0x6A,0x0000},
    {0x64,0x0000},{0x65,0x0000},{0x1F,0x0003} };
bool LocalWM8962::begin(TwoWire &b, uint8_t a) {
    bus = &b; addr = a;
    for (auto &e : INIT_PRE) if (!writeReg(e[0], e[1])) return false;
    if (!modifyReg(0x9B, 0x0001, 0x0000)) return false;
    for (auto &e : INIT_POST) if (!writeReg(e[0], e[1])) return false;
    for (uint16_t id : SEQ) {
        if (!writeReg(0x57, 0x0020)) return false;
        if (!writeReg(0x5A, id))     return false;
        if (!pollSeqDone())          return false;
    }
    if (!modifyReg(0x04, 0x0600, 0x0000)) return false;
    if (!modifyReg(0x08, 0x0020, 0x0020)) return false;
    for (auto &e : ROUTE) if (!writeReg(e[0], e[1])) return false;
    if (!modifyReg(0x07, 0x0013, 0x0002)) return false;
    for (auto &e : VOLUME) if (!writeReg(e[0], e[1])) return false;
    if (!modifyReg(0x07, 0x000C, 0x0000)) return false;
    if (!writeReg(0x1B, 0x0010)) return false;   // ADDCTL3: 48 kHz
    if (!writeReg(0x38, 0x000A)) return false;   // CLK4: ratio 512
    return true;
}
} // anonymous namespace

// 1 kHz @ 48 kHz = 48 samples/cycle. L = full sine, R = half amplitude
// (distinct so a channel swap is detectable). 2 cycles = 96 frames.
static int16_t g_sine[96 * 2];
static void build_sine() {
    for (int i = 0; i < 96; i++) {
        double ph = 2.0 * 3.14159265358979 * (i % 48) / 48.0;
        int16_t v = (int16_t)(0x6000 * __builtin_sin(ph));
        g_sine[2*i + 0] = v;                 // L
        g_sine[2*i + 1] = (int16_t)(v / 2);  // R
    }
}

void setup() {
    Serial1.begin(115200);
    delay(50);
    I2S.begin(48000);
    // Stage A: verify SAI1 configured as 48k/16-bit I2S master.
    uint32_t tcsr = SAI1_TCSR, tcr2 = SAI1_TCR2, tcr4 = SAI1_TCR4, tcr5 = SAI1_TCR5;
    bool te   = (tcsr & SAI_TCSR_TE) != 0;
    bool div7 = (tcr2 & 0xFFu) == 7;
    bool mclk1= ((tcr2 >> 26) & 0x3u) == 1;
    bool i2s4 = (tcr4 & SAI_TCR4_MF) && (tcr4 & SAI_TCR4_FSD) &&
                (tcr4 & SAI_TCR4_FSE) && (((tcr4 >> 16) & 0x1Fu) == 1);
    bool w16  = (((tcr5 >> 16) & 0x1Fu) == 15) && (((tcr5 >> 24) & 0x1Fu) == 15);
    Serial1.print("STAGE_A tcsr="); Serial1.print(tcsr, HEX);
    Serial1.print(" tcr2="); Serial1.print(tcr2, HEX);
    Serial1.print(" tcr4="); Serial1.print(tcr4, HEX);
    Serial1.print(" tcr5="); Serial1.println(tcr5, HEX);
    if (te && div7 && mclk1 && i2s4 && w16) Serial1.println("STAGE_A_PASS");
    else Serial1.println("STAGE_A_FAIL");

    build_sine();
    I2S.write(g_sine, 96);
    Serial1.println("STAGE_B_DONE");   // tap capture is checked host-side

    Wire2.begin();
    static LocalWM8962 localCodec;
    bool ok = localCodec.begin(Wire2);
    Serial1.println(ok ? "STAGE_C_PASS" : "STAGE_C_FAIL");
}
void loop() {
    // Continuous transmit so BCLK/FS/DATA stay steady for a Saleae capture on
    // hardware. 96 frames = exactly 2 full 1 kHz cycles, so this loops seamlessly.
    I2S.write(g_sine, 96);
}
