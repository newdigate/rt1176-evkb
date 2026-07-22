/* codec_wm8962.c - WM8962 codec bring-up from the CM4, over the shared LPI2C
 * core (Wire/lpi2c1176.c). The register program is a faithful transcription of
 * this fork's HW-verified WM8962Codec::begin() + AudioControlWM8962::enable()
 * (newdigate/Audio control_wm8962.cpp, MIT) -- the exact sequence that drove
 * an audible 1 kHz tone on J101 and MIC=PASS in examples/audio/i2s_int_test on
 * the CM7. Here it runs entirely on the CM4 with no Wire/C++ runtime: the four
 * primitives (writeReg / readReg / modifyReg / pollSeqDone) are expressed over
 * raw lpi2c1176_master_write / lpi2c1176_master_read calls, the same shared C
 * core the CM7 TwoWire master path runs (cm4_wire_test pattern).
 *
 * LPI2C5 + its CCM/LPSR-IOMUXC instance addresses are the imxrt1176.h values as
 * literals (the bare-metal image has no core headers), lifted verbatim from
 * examples/dualcore/cm4_wire_test/cm4/main_cm4.c (HW-verified: the CM4 itself
 * ungates LPCG102, roots CLOCK_ROOT41 mux 1, and muxes the LPSR pads).
 *
 * SAMPLE RATE: the codec is a slave synced to the SAI's 44.1 kHz MCLK/BCLK.
 * begin() writes ADDCTL3 (0x1B) = 0x0010 (the SDK's 48 kHz case, as the source
 * WM8962_Init does); enable() then overrides it with 0x0000 (the 44.1 kHz
 * case) -- the sysclk/Fs ratio (512) is identical, only ADDCTL3's absolute-rate
 * field changes. This mirrors control_wm8962.cpp exactly. THE CODEC NEEDS MCLK
 * PRESENT for its internal write-sequencer (reg 0x57/0x5A, poll 0x5D bit0), so
 * the caller starts the SAI TX clock BEFORE calling codec_wm8962_init()
 * (see main_cm4.cpp -- the HW-verified i2s_int_test ordering).
 *
 * QEMU: the wm8962-stub ACKs writes on MCR.MEN alone and reads 0x0000 for every
 * register (so pollSeqDone sees bit0==0 immediately); a green QEMU run proves
 * the transfer sequence, the wiring-free EVKB run (real codec) proves the CM4
 * brought up the LPSR clock + pins. codec_wm8962_init() returns 1 iff every
 * transaction ACKed.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#include <stdint.h>
#include "lpi2c1176.h"
#include "codec_wm8962.h"

/* LPI2C5 register block + its CCM/LPSR-IOMUXC instance table (cm4_wire_test
 * literals; the CM7 Wire library binds the same registers via header macros). */
#define LPI2C5 ((lpi2c1176_regs_t *)0x40C34000u)
static const lpi2c1176_hw_t lpi2c5_hw = {
    .lpcg = (volatile uint32_t *)0x40CC6CC0u,          /* CCM_LPCG102_DIRECT */
    .clock_root = (volatile uint32_t *)0x40CC1480u,    /* CCM_CLOCK_ROOT41_CONTROL */
    .clock_root_val = (1u << 8),                       /* mux 1 -> 24 MHz */
    .scl_mux = (volatile uint32_t *)0x40C08014u, .scl_mux_val = 0x10u, /* GPIO_LPSR_05 ALT0|SION */
    .scl_pad = (volatile uint32_t *)0x40C08054u,
    .sda_mux = (volatile uint32_t *)0x40C08010u, .sda_mux_val = 0x10u, /* GPIO_LPSR_04 ALT0|SION */
    .sda_pad = (volatile uint32_t *)0x40C08050u,
    .scl_select = (volatile uint32_t *)0x40C08084u, .scl_select_val = 0u,
    .sda_select = (volatile uint32_t *)0x40C08088u, .sda_select_val = 0u,
    .pad_ctl_val = 0x0Au,                              /* LPSR open-drain */
};

#define WM8962_ADDR    0x1Au
#define WM8962_ADDCTL3 0x1Bu
#define WM8962_ADDCTL3_44100HZ 0x0000u

/* --- the four codec primitives over the shared LPI2C core --- */

/* WriteReg: {0x00, reg, val>>8, val&0xFF} to 0x1A, STOP. 1 = ACK. */
static uint32_t writeReg(uint16_t reg, uint16_t val)
{
    uint8_t buf[4] = { 0x00u, (uint8_t)reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFFu) };
    return lpi2c1176_master_write(LPI2C5, WM8962_ADDR, buf, 4, 1) == 0u;
}

/* ReadReg: write {0x00, reg} (no STOP), repeated-START read 2 bytes big-endian. */
static uint32_t readReg(uint16_t reg, uint16_t *val)
{
    uint8_t ra[2] = { 0x00u, (uint8_t)reg };
    uint8_t rd[2] = { 0u, 0u };
    if (lpi2c1176_master_write(LPI2C5, WM8962_ADDR, ra, 2, 0) != 0u) {
        return 0u;
    }
    if (lpi2c1176_master_read(LPI2C5, WM8962_ADDR, rd, 2, 1) != 2u) {
        return 0u;
    }
    *val = ((uint16_t)rd[0] << 8) | rd[1];
    return 1u;
}

/* ModifyReg: read, clear mask bits, OR val, write back (fsl_wm8962 order). */
static uint32_t modifyReg(uint16_t reg, uint16_t mask, uint16_t val)
{
    uint16_t regVal;
    if (!readReg(reg, &regVal)) {
        return 0u;
    }
    regVal = (uint16_t)(regVal & (uint16_t)~mask);
    regVal = (uint16_t)(regVal | val);
    return writeReg(reg, regVal);
}

/* Poll the WM8962 write-sequencer done flag (reg 0x5D bit0 clears when idle). */
static uint32_t pollSeqDone(void)
{
    for (int i = 0; i < 100000; i++) {
        uint16_t s;
        if (!readReg(0x5Du, &s)) {
            return 0u;
        }
        if ((s & 0x1u) == 0u) {
            return 1u;
        }
    }
    return 0u;
}

/* --- the register program (control_wm8962.cpp WM8962Codec::begin, verbatim) --- */
static const uint16_t INIT_PRE[][2]  = { {0x0F, 0x6243}, {0x81, 0x0000} };
static const uint16_t INIT_POST[][2] = { {0x08, 0x09E4}, {0x19, 0x01FE}, {0x1A, 0x01E0} };
static const uint16_t SEQ[]          = { 0x80, 0x92, 0xE8 };
static const uint16_t ROUTE[][2] = {
    {0x25, 0x0018}, {0x26, 0x0012}, {0x22, 0x0009}, {0x69, 0x0000}, {0x6A, 0x0000},
    {0x64, 0x0000}, {0x65, 0x0000}, {0x1F, 0x0003},
};
static const uint16_t VOLUME[][2] = {
    {0x15, 0x01C0}, {0x16, 0x01C0}, {0x0A, 0x01C0}, {0x0B, 0x01C0}, {0x28, 0x01FF},
    {0x29, 0x01FF}, {0x00, 0x013F}, {0x01, 0x013F}, {0x02, 0x016B}, {0x03, 0x016B},
};

uint32_t codec_wm8962_init(void)
{
    unsigned i;

    /* self-config LPI2C5 via the shared core (~100 kHz) -- the CM4 ungates the
     * LPSR-domain clock and muxes the pads itself (cm4_wire_test, HW-verified). */
    lpi2c1176_begin(LPI2C5, &lpi2c5_hw, 100000u);

    for (i = 0; i < 2; i++) {
        if (!writeReg(INIT_PRE[i][0], INIT_PRE[i][1])) return 0u;
    }
    if (!modifyReg(0x9Bu, 0x0001u, 0x0000u)) return 0u;   /* disable osc/FLL */
    for (i = 0; i < 3; i++) {
        if (!writeReg(INIT_POST[i][0], INIT_POST[i][1])) return 0u;
    }
    for (i = 0; i < 3; i++) {                             /* StartSequence IDs */
        if (!writeReg(0x57u, 0x0020u)) return 0u;
        if (!writeReg(0x5Au, SEQ[i]))  return 0u;
        if (!pollSeqDone())            return 0u;
    }
    if (!modifyReg(0x04u, 0x0600u, 0x0000u)) return 0u;   /* DSP clock divider = 0 */
    if (!modifyReg(0x08u, 0x0020u, 0x0020u)) return 0u;   /* enable SYSCLK */
    for (i = 0; i < 8; i++) {
        if (!writeReg(ROUTE[i][0], ROUTE[i][1])) return 0u;
    }
    if (!modifyReg(0x07u, 0x0013u, 0x0002u)) return 0u;   /* IFACE0: I2S */
    for (i = 0; i < 10; i++) {
        if (!writeReg(VOLUME[i][0], VOLUME[i][1])) return 0u;
    }
    if (!modifyReg(0x07u, 0x000Cu, 0x0000u)) return 0u;   /* IFACE0: 16-bit */
    if (!writeReg(0x1Bu, 0x0010u)) return 0u;             /* ADDCTL3: 48 kHz case */
    if (!writeReg(0x38u, 0x000Au)) return 0u;             /* CLK4: ratio 512 */

    /* enable() override: rewrite ADDCTL3 with the 44.1 kHz code (this board's
     * SAI runs at 44.1 kHz; ratio 512 is unchanged, only the rate field). */
    if (!writeReg(WM8962_ADDCTL3, WM8962_ADDCTL3_44100HZ)) return 0u;

    return 1u;
}
