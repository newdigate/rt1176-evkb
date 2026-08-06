/* cm4_sai_irq_probe: does SAI1's FIFO-request interrupt (IRQ 76 per RM Table
 * 4-2 line 3760) reach the CM4 NVIC on real silicon? The fast-GPIO precedent
 * says this table cannot be trusted without a probe.
 *
 * The CM4 self-configures SAI1 TX exactly as the HW-verified CM7 sequence
 * does (cores/imxrt1176/I2S.cpp configureSAI(), distilled to literals per the
 * CM4 gate convention), but enables FRIE (bit 8, FIFO-request INTERRUPT)
 * instead of FRDE (bit 0, DMA). TX watermark 16 on a 32-deep FIFO: FRF stays
 * asserted whenever count <= 16, so with the FIFO drained the IRQ fires
 * immediately on enable and re-fires as the shift clock drains words.
 * Observations stream to the CM7 over the MU (cm4_wire_test pattern).
 *
 * Register literals cross-checked against cores/imxrt1176/imxrt1176.h macro
 * expansions and cm4_wire_test/cm4/main_cm4.c (MU) on 2026-07-21; the plan
 * document's draft offsets for SAI1 TX (TCSR is at +0x08, not +0x00 — VERID/
 * PARAM come first), the AD pad muxes (AD_00 mux is 0x400E810C, so AD_17 is
 * 0x400E8150), and the MU B side (TR0 at +0x00, SR at +0x20) were corrected.
 * Public domain (N. Newdigate). */
#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(a))

/* ---- MU B side (the CM4's), TR channel 0 ---- (cm4_wire_test pattern:
 * TR0 at +0x00, SR at +0x20, TE0 = SR bit 23 — HW-verified offsets) */
#define MUB_BASE   0x40C4C000u
#define MUB_TR0    REG32(MUB_BASE + 0x00u)
#define MUB_SR     REG32(MUB_BASE + 0x20u)
#define MU_SR_TE0  (1u << 23)

static void mu_send(uint32_t v) {
    while (!(MUB_SR & MU_SR_TE0)) {}
    MUB_TR0 = v;
}

/* ---- SAI1 @ 0x40404000 (RM 58.5; offsets per imxrt1176.h: VERID/PARAM
 * occupy +0x00/+0x04, so TCSR starts at +0x08) ---- */
#define SAI1_BASE  0x40404000u
#define SAI1_TCSR  REG32(SAI1_BASE + 0x08u)
#define SAI1_TCR1  REG32(SAI1_BASE + 0x0Cu)
#define SAI1_TCR2  REG32(SAI1_BASE + 0x10u)
#define SAI1_TCR3  REG32(SAI1_BASE + 0x14u)
#define SAI1_TCR4  REG32(SAI1_BASE + 0x18u)
#define SAI1_TCR5  REG32(SAI1_BASE + 0x1Cu)
#define SAI1_TDR0  REG32(SAI1_BASE + 0x20u)
#define SAI1_TMR   REG32(SAI1_BASE + 0x60u)
#define TCSR_TE    (1u << 31)
#define TCSR_BCE   (1u << 28)
#define TCSR_FR    (1u << 25)
#define TCSR_SR    (1u << 24)
#define TCSR_FRF   (1u << 16)
#define TCSR_FRIE  (1u << 8)

/* ---- clocking/pins: literals = imxrt1176.h macro expansions used by the
 * HW-verified cores/imxrt1176/I2S.cpp configureSAI() sequence ---- */
#define CCM_CLOCK_ROOT64_CONTROL REG32(0x40CC2000u)  /* sai1 root (0x40CC0000+0x80*64) */
#define CCM_LPCG123_DIRECT       REG32(0x40CC6F60u)  /* sai1 gate (+0x6000+0x20*123) */
#define IOMUXC_GPR_GPR0          REG32(0x400E4000u)
#define PADMUX_AD_17             REG32(0x400E8150u)  /* SAI1_MCLK      */
#define PADMUX_AD_21             REG32(0x400E8160u)  /* SAI1_TX_DATA00 */
#define PADMUX_AD_22             REG32(0x400E8164u)  /* SAI1_TX_BCLK   */
#define PADMUX_AD_23             REG32(0x400E8168u)  /* SAI1_TX_SYNC   */

#define NVIC_ISER2 REG32(0xE000E108u)          /* IRQs 64..95; SAI1 = 76 */

volatile uint32_t sai_irq_count = 0;

/* Shared vector table (startup_cm4.S) also references these; unused here. */
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}

void SAI1_IRQHandler(void) {
    sai_irq_count++;
    /* Feed one word per entry so FRF eventually clears if the FIFO fills;
     * after 64 entries stop feeding and disable FRIE so the probe terminates. */
    if (sai_irq_count < 64u) SAI1_TDR0 = 0u;
    else SAI1_TCSR &= ~TCSR_FRIE;
}

int main(void) {
    /* NOTE: the Audio PLL is expected to be OFF in this probe (nobody set it
     * up); the SAI bit clock then free-runs from whatever the root mux
     * provides. That is fine: the probe only needs FRF && FRIE -> NVIC entry,
     * which is FIFO-level logic, not clock-quality logic. */
    CCM_CLOCK_ROOT64_CONTROL = (4u << 8) | (15u << 0);   /* mux 4, div 16 */
    CCM_LPCG123_DIRECT = 1u;                              /* ungate SAI1 */
    PADMUX_AD_17 = 0x10u; PADMUX_AD_22 = 0x10u; PADMUX_AD_23 = 0x10u;
    PADMUX_AD_21 = 0x0u;
    IOMUXC_GPR_GPR0 |= (1u << 8);                         /* SAI1_MCLK_DIR */

    SAI1_TCSR = TCSR_SR; SAI1_TCSR = 0u;
    SAI1_TCSR = TCSR_FR; SAI1_TCSR = 0u;
    SAI1_TMR  = 0u;
    SAI1_TCR1 = 16u;                                      /* watermark 16 */
    SAI1_TCR2 = (1u << 26) | (1u << 24) | (1u << 25) | 7u; /* MSEL(1)|BCD|BCP|DIV(7) */
    SAI1_TCR3 = (1u << 16);                               /* TCE */
    SAI1_TCR4 = (1u << 16) | (15u << 8) | (1u << 4) | (1u << 3) | (1u << 1) | 1u
              | (1u << 28);                               /* FRSZ|SYWD|MF|FSE|FSP|FSD|FCONT */
    SAI1_TCR5 = (15u << 24) | (15u << 16) | (15u << 8);   /* WNW|W0W|FBT */

    mu_send(0xCAFE0001u);                 /* ready marker */
    __asm volatile ("cpsie i" ::: "memory"); /* reset handler left IRQs masked */
    NVIC_ISER2 = (1u << (76u - 64u));     /* enable IRQ 76 on the CM4 NVIC */
    SAI1_TCSR = TCSR_TE | TCSR_BCE | TCSR_FRIE;  /* empty FIFO: FRF=1 now */

    /* Let interrupts accumulate; a bounded spin, then report. */
    for (volatile uint32_t i = 0; i < 2000000u; i++) {}
    mu_send(sai_irq_count);               /* the probe's answer */
    mu_send((SAI1_TCSR >> 16) & 0x7u);    /* FRF/FWF/FEF snapshot */
    mu_send(0xD0DE0001u);                 /* done marker */
    for (;;) { __asm volatile ("wfi"); }
}
