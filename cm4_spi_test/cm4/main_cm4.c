/* cm4_spi_test CM4 firmware (Phase 3.1): the CM4 SELF-CONFIGURES LPSPI1 and runs
 * a polled master self-loopback (external SDO->SDI jumper), streaming each
 * observation to the CM7 over the MU (channel 0, in order).
 *
 * Phase 3.3: the register/clock sequence now IS the shared C core
 * lpspi1176.c from newdigate/SPI (MIT, N. Newdigate) — the same code the CM7
 * SPIClass runs, no more keep-in-sync mirror. This file keeps only the MU
 * scaffolding, the LPSPI1 per-instance address table (imxrt1176.h values as
 * literals; the bare-metal image has no core headers), and the token flow.
 *
 * SILICON TRUTH: the qemu2 board attaches an ssi-loopback child to LPSPI1 that
 * echoes on CR.MEN ALONE — it ignores the clock gate, clock root, and pin mux.
 * So rx==tx in QEMU proves only the register/transfer sequence; the real
 * SDO->SDI jumper on hardware is what proves the CM4 ungated the clock + muxed
 * the pins + drove a real SCK.  Public-domain scaffolding (N. Newdigate);
 * shared-core register logic MIT as noted above. */
#include <stdint.h>
#include "lpspi1176.h"

/* LPSPI1 + its CCM/IOMUXC instance addresses (imxrt1176.h values; the CM7
 * library binds the same registers via the header macros). */
#define LPSPI1 ((lpspi1176_regs_t *)0x40114000u)
static const lpspi1176_hw_t lpspi1_hw = {
    .lpcg = (volatile uint32_t *)0x40CC6D00u,          /* CCM_LPCG104_DIRECT */
    .clock_root = (volatile uint32_t *)0x40CC1580u,    /* CCM_CLOCK_ROOT43_CONTROL */
    .clock_root_val = 0u,                              /* mux0 OSC24M div1 -> 24 MHz */
    .func_clock = 24000000u,
    .sck_mux = (volatile uint32_t *)0x400E817Cu, .sck_mux_val = 0u,   /* GPIO_AD_28 ALT0 */
    .sck_pad = (volatile uint32_t *)0x400E83C0u,
    .sck_select = (volatile uint32_t *)0x400E85D0u, .sck_select_val = 1u,
    .sdo_mux = (volatile uint32_t *)0x400E8184u, .sdo_mux_val = 0u,   /* GPIO_AD_30 ALT0 */
    .sdo_pad = (volatile uint32_t *)0x400E83C8u,
    .sdo_select = (volatile uint32_t *)0x400E85D8u, .sdo_select_val = 1u,
    .sdi_mux = (volatile uint32_t *)0x400E8188u, .sdi_mux_val = 0u,   /* GPIO_AD_31 ALT0 */
    .sdi_pad = (volatile uint32_t *)0x400E83CCu,
    .sdi_select = (volatile uint32_t *)0x400E85D4u, .sdi_select_val = 1u,
    .pad_ctl_val = 0x0Cu,                              /* DSE set */
};

/* ---- MU B side (the CM4's) ---- */
#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define SR_TE(n)   (1u << (23 - (n)))

/* The shared vector table (startup_cm4.S) references these C symbols. Polled
 * SPI needs neither, but the table entries must resolve. */
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
    /* --- self-config LPSPI1 via the shared core (default 4 MHz) --- */
    uint32_t tcr_base = 0;
    lpspi1176_begin(LPSPI1, &lpspi1_hw, 4000000u, &tcr_base);

    /* --- config readbacks --- */
    uint32_t cr    = LPSPI1->CR & LPSPI1176_CR_MEN;           /* -> 1 */
    uint32_t cfgr1 = LPSPI1->CFGR1 & LPSPI1176_CFGR1_MASTER;  /* -> 1 */
    uint32_t lpcg  = *lpspi1_hw.lpcg;                         /* informative */
    uint32_t croot = *lpspi1_hw.clock_root;                   /* informative */

    /* --- polled loopback (SDO->SDI jumper) --- */
    uint32_t a = lpspi1176_transfer_frame(LPSPI1, tcr_base, 0xA5u, 7u) & 0xFFu;
    uint32_t b = lpspi1176_transfer_frame(LPSPI1, tcr_base, 0x3Cu, 7u) & 0xFFu;
    uint32_t w = lpspi1176_transfer_frame(LPSPI1, tcr_base, 0xBEEFu, 15u) & 0xFFFFu;
    uint8_t bs[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };
    for (int i = 0; i < 4; i++) {
        bs[i] = (uint8_t)(lpspi1176_transfer_frame(LPSPI1, tcr_base, bs[i], 7u) & 0xFFu);
    }
    uint32_t buf = ((uint32_t)bs[0] << 24) | ((uint32_t)bs[1] << 16)
                 | ((uint32_t)bs[2] << 8) | (uint32_t)bs[3];

    uint32_t rxok = (a == 0xA5u && b == 0x3Cu && w == 0xBEEFu
                     && buf == 0xDEADBEEFu) ? 1u : 0u;

    /* --- stream the 9 observations to the CM7 (MU TR0, fixed order) --- */
    mu_send(0, cr);
    mu_send(0, cfgr1);
    mu_send(0, lpcg);
    mu_send(0, croot);
    mu_send(0, a);
    mu_send(0, b);
    mu_send(0, w);
    mu_send(0, buf);
    mu_send(0, rxok);

    for (;;) {
    }
}
