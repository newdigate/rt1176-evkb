/* cm4_wire_test CM4 firmware (Phase 3.2): the CM4 SELF-CONFIGURES LPI2C5 (the
 * on-board WM8962 codec bus) and runs three polled I2C master transactions,
 * streaming each observation to the CM7 over the MU (channel 0, in order):
 *   1. reset-write R15<-0x6243 to the WM8962 @0x1A  -> ack  (expect 0)
 *   2. zero-byte probe of absent address 0x2A       -> nack (expect 2)
 *   3. device-ID read-back of R15 (repeated START)  -> rdn=2, rdv
 *
 * Phase 3.3: the register/clock sequences now ARE the shared C core
 * lpi2c1176.c from newdigate/Wire (MIT, N. Newdigate) — the same code the CM7
 * TwoWire master path runs, no more keep-in-sync mirror. This file keeps only
 * the MU scaffolding, the LPI2C5 per-instance address table (imxrt1176.h
 * values as literals; the bare-metal image has no core headers), and the
 * WM8962 token protocol (from newdigate/Audio control_wm8962.cpp, MIT). The
 * R15 readback default 0x6243 (= device ID) is a hardware FACT taken from the
 * Linux wm8962.c reg_default table (2026-07-18); no code from that GPL source.
 *
 * SILICON TRUTH: the qemu2 LPI2C model + wm8962-stub respond on MCR.MEN alone
 * (clock gate / clock root / LPSR pin mux ignored), so a green QEMU run proves
 * only the register/transfer sequence; the wiring-free EVKB run (real codec
 * ACK + ID) is what proves the CM4 brought up the clock and LPSR pins itself.
 * ACK/NACK is judged at STOP completion (SDF wait watching NDF), NEVER at TDF
 * — TDF leads the ACK bit by a byte-time on silicon (see lpi2c1176.c; the
 * qemu model's deferred-NDF mirrors exactly this).
 * Public-domain scaffolding (N. Newdigate); shared-core register logic MIT. */
#include <stdint.h>
#include "lpi2c1176.h"

/* LPI2C5 + its CCM/LPSR-IOMUXC instance addresses (imxrt1176.h values; the
 * CM7 library binds the same registers via the header macros). */
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

#define WM8962_ADDR  0x1Au
#define ABSENT_ADDR  0x2Au   /* clear of WM8962 0x1A + FXLS8974 accel 0x18 */

/* ---- MU B side (the CM4's) ---- */
#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define SR_TE(n)   (1u << (23 - (n)))

/* The shared vector table (startup_cm4.S) references these C symbols. Polled
 * I2C needs neither, but the table entries must resolve. */
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}

static void mu_send(unsigned ch, uint32_t v)
{
    while (!(MUB_SR & SR_TE(ch))) {
    }
    MUB_TR(ch) = v;
}

int main(void)
{
    /* --- self-config LPI2C5 via the shared core (~100 kHz) --- */
    lpi2c1176_begin(LPI2C5, &lpi2c5_hw, 100000u);

    /* --- config readbacks --- */
    uint32_t mcr   = LPI2C5->MCR & LPI2C1176_MCR_MEN;  /* -> 1 */
    uint32_t lpcg  = *lpi2c5_hw.lpcg;                  /* informative */
    uint32_t croot = *lpi2c5_hw.clock_root;            /* informative */

    /* --- 1. reset-write R15<-0x6243 (WM8962_Init's own first write) --- */
    static const uint8_t reset_wr[4] = { 0x00u, 0x0Fu, 0x62u, 0x43u };
    uint32_t ack = lpi2c1176_master_write(LPI2C5, WM8962_ADDR, reset_wr, 4, 1);

    /* --- 2. zero-byte probe of an absent address -> address NACK --- */
    uint32_t nack = lpi2c1176_master_write(LPI2C5, ABSENT_ADDR, 0, 0, 1);

    /* --- 3. device-ID read-back of R15 (write reg addr, repeated START) --- */
    static const uint8_t reg_addr[2] = { 0x00u, 0x0Fu };
    uint8_t rd[2] = { 0, 0 };
    uint32_t rdn = 0, rdv = 0;
    if (lpi2c1176_master_write(LPI2C5, WM8962_ADDR, reg_addr, 2, 0) == 0u) {  /* no STOP */
        rdn = lpi2c1176_master_read(LPI2C5, WM8962_ADDR, rd, 2, 1);
        rdv = ((uint32_t)rd[0] << 8) | rd[1];
    }

    /* --- stream the 8 observations to the CM7 (MU TR0, fixed order) --- */
    mu_send(0, mcr);
    mu_send(0, lpcg);
    mu_send(0, croot);
    mu_send(0, ack);
    mu_send(0, nack);
    mu_send(0, rdn);
    mu_send(0, rdv);
    mu_send(0, 1u);                          /* done */

    for (;;) {
    }
}
