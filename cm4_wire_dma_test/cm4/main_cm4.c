/* cm4_wire_dma_test CM4 firmware (Phase 4 — eDMA_LPSR DMA-Wire, GREEN):
 * the CM4 does genuine interrupt-driven DMA on its OWN eDMA (eDMA_LPSR, base
 * 0x40C14000): it self-configures LPI2C5 (shared lpi2c1176_begin, exactly as
 * cm4_wire_test / Phase 4.1) and DMA-reads the WM8962 R15 device ID over
 * LPI2C5's RX FIFO, with the eDMA_LPSR channel-0 major-completion interrupt
 * firing on the CM4's OWN NVIC natively (CM4 IRQ 0 -> DMA_CH0_IRQHandler ->
 * dmairq++).  This is the true CM4 interrupt-DMA the main eDMA cannot deliver:
 * the main eDMA's 16 channel IRQs are CM7-domain (RM Table 4-1), but eDMA_LPSR
 * channel n -> CM4 NVIC IRQ n natively (RM Table 4-2) -- the whole point of the
 * milestone (dmairq>0 from a real eDMA_LPSR transfer).
 *
 * DMA read path (LPI2C5 RX FIFO -> eDMA_LPSR RX channel 0 -> OCRAM2 buffer):
 *   (1) polled reg-pointer WRITE (R15 = {0x00,0x0F}, sendStop=0, bus held) via
 *       the shared HW-verified core -- the SAME 4.1 discipline that fixed the
 *       cold-bus race (the pointer is provably on the wire before the read).
 *   (2) repeated START + addr(R);   (3) TDF wait (address byte clocked out);
 *   (4) program eDMA_LPSR RX ch0 (TCD @ 0x40C15000): SADDR=&MRDR, DADDR=rxbuf,
 *       CITER=BITER=2, CSR=DREQ|INTMAJOR;  DMAMUX1/LPSR CHCFG(0)=52|ENBL;
 *   (5) SERQ ch0 (enable the channel's request);  (6) RXD (clock 2 bytes) then
 *       MDER.RDDE (enable RX-DMA request -> eDMA drains MRDR -> rxbuf);
 *   (7) wait dmairq (the eDMA_LPSR completion IRQ, taken on the CM4's own NVIC);
 *   (8) STOP + clean SDF check.
 *
 * -- Two QEMU-model accommodations, both silicon-NEUTRAL (documented so the HW
 *    probe stays trustworthy): --------------------------------------------------
 *  A. 32-bit MRDR transfers (ATTR SSIZE=DSIZE=2), not the NXP SDK's 8-bit
 *     (fsl_lpi2c_edma.c kEDMA_TransferSize1Bytes): qemu's imxrt_lpi2c model
 *     accepts word access only (valid.min_access_size=4), and MRDR's received
 *     byte is in bits[7:0] on silicon, so a 32-bit read yields the same byte in
 *     either world.  rdv is extracted with (rxbuf[i] & 0xFF).
 *  B. RXD-before-RDDE order: qemu's imxrt_lpi2c do_command(CMD_RXD) fills the RX
 *     FIFO but does not re-evaluate the DMA request; the MDER.RDDE write (which
 *     does) is therefore issued AFTER the RXD so the request asserts with the
 *     FIFO already primed.  On silicon the request is level-based (RDF && RDDE),
 *     so enabling RDDE after issuing RXD is equally valid (the 4-deep RX FIFO
 *     buffers the 2 bytes meanwhile).
 *
 * eDMA_LPSR clock gate: CCM LPCG23 (clk_enable_edma_lpsr, RM CCM LPCG table;
 * DIRECT @ 0x40CC6000 + 23*0x20 = 0x40CC62E0 -- the main eDMA's LPCG22 @0x40CC62C0
 * confirms the 0x20 stride).  Ungated defensively: qemu's CCM is RAM-backed so
 * this is inert there; on silicon it guarantees the eDMA_LPSR functional clock.
 *
 * CONTINGENCY: NOT fired.  The full LPI2C5-RX-DMA-of-WM8962-ID path gated cleanly
 * in QEMU -- accommodations A/B are firmware-only, no qemu2 change was needed.
 *
 * Tokens streamed to the CM7 over the MU (channel 0, fixed order):
 *   ready  = CAFE0001   CM4 alive
 *   croot  = ........   CCM_CLOCK_ROOT41 readback (informative)
 *   rdv    = ????????   R15 device ID DMA'd by eDMA_LPSR -- WORLD-SPLIT: QEMU
 *                       wm8962-stub reads 0x0000; silicon reads 0x6243 (asserted
 *                       HW-side, same discipline as 4.1's rdv)
 *   dmairq = >0         the CM4 took the eDMA_LPSR completion IRQ on its own NVIC
 *   err    = 00000000   DMA/transaction OK (no NDF/ALF/FEF, DMA not stalled)
 *   done   = 00000001   CM4 sequence completed
 *
 * The self-config and the polled reg-pointer write reuse the shared C core
 * lpi2c1176.c from newdigate/Wire (MIT, N. Newdigate) -- the same code the CM7
 * TwoWire master path runs; only the DMA plumbing is new.  The R15 device-ID
 * value (0x6243) is the hardware FACT cm4_wire_test established (from the Linux
 * wm8962.c reg_default table, 2026-07-18; fact only, no GPL code used).
 * Public-domain scaffolding (N. Newdigate); shared-core register logic MIT. */
#include <stdint.h>
#include "lpi2c1176.h"

/* LPI2C5 + its CCM/LPSR-IOMUXC instance addresses (imxrt1176.h values; verbatim
 * from cm4_wire_int_master_test / cm4_wire_test -- the CM7 Wire library binds
 * the same registers via the header macros). */
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

/* LPI2C master DMA enable (MDER @ 0x1C; RM 69.5.1.7 / qemu imxrt_lpi2c). */
#define LPI2C_MDER_TDDE  (1u << 0)
#define LPI2C_MDER_RDDE  (1u << 1)

/* ---- eDMA_LPSR (base 0x40C14000; same IP as the main eDMA, but its 16 channel
 * IRQs land on the CM4 NVIC natively -- RM Table 4-2). ---- */
#define DMA_CR      (*(volatile uint32_t *)0x40C14000u)
#define DMA_SERQ    (*(volatile uint8_t  *)0x40C1401Bu)   /* set enable request (channel #) */
#define DMA_CINT    (*(volatile uint8_t  *)0x40C1401Fu)   /* clear interrupt (channel #) */
#define DMA_CR_GRP1PRI (1u << 10)
#define DMA_CR_EMLM    (1u << 7)
#define DMA_CR_EDBG    (1u << 1)
#define CCM_LPCG23_DIRECT (*(volatile uint32_t *)0x40CC62E0u)  /* eDMA_LPSR clock gate */
#define DMAMUX_LPSR_CHCFG(ch)  (*(volatile uint32_t *)(0x40C18000u + (ch) * 4u))
#define DMAMUX_ENBL       (1u << 31)
#define SRC_LPI2C5        52u          /* DMAMUX1/LPSR source: LPI2C5 RX/TX request */
#define TCD_ATTR_32BIT    0x0202u      /* SSIZE=2 (bits10:8), DSIZE=2 (bits2:0) -> 4 bytes */
#define TCD_CSR_DREQ      0x0008u      /* clear ERQ on major-loop completion */
#define TCD_CSR_INTMAJOR  0x0002u      /* raise the channel IRQ on major-loop completion */
#define RX_CH   0u
#define IRQ_DMA_RX  0u                 /* eDMA_LPSR channel 0 -> CM4 NVIC IRQ 0 (native) */
#define NVIC_ISER0  (*(volatile uint32_t *)0xE000E100u)   /* IRQ 0..31 */
#define DMA_GUARD   4000000u           /* bounded wait: a stalled DMA fails visibly, never hangs */

/* eDMA TCD -- exact hardware layout (matches cores DMAChannel TCD_t, 32 bytes). */
typedef struct __attribute__((packed, aligned(4))) {
    const volatile void *volatile SADDR;   /* 0x00 */
    int16_t  SOFF;                          /* 0x04 */
    uint16_t ATTR;                          /* 0x06 */
    uint32_t NBYTES;                        /* 0x08 */
    int32_t  SLAST;                         /* 0x0C */
    volatile void *volatile DADDR;          /* 0x10 */
    int16_t  DOFF;                          /* 0x14 */
    uint16_t CITER;                         /* 0x16 */
    int32_t  DLASTSGA;                      /* 0x18 */
    volatile uint16_t CSR;                  /* 0x1C */
    uint16_t BITER;                         /* 0x1E */
} tcd_t;
#define TCD(ch)  ((volatile tcd_t *)(0x40C15000u + (ch) * 0x20u))

/* DMA destination in OCRAM2 (.dmabuf): the eDMA is a system-bus master and
 * cannot reach the CM4's private DTCM.  uint32_t (32-bit transfers, accommodation
 * A); the received byte is the low 8 bits of each word.  volatile so the CM4's
 * post-DMA reads can't be hoisted across the completion barrier. */
static volatile uint32_t rxbuf[2] __attribute__((section(".dmabuf")));

/* eDMA_LPSR channel-0 completion ISR -- runs on the CM4's OWN NVIC (IRQ 0, via
 * the eDMA_LPSR->CM4 native routing).  dmairq>0 is the milestone proof. */
static volatile uint32_t dmairq = 0;
void DMA_CH0_IRQHandler(void)
{
    DMA_CINT = RX_CH;    /* clear ch0 interrupt request */
    __asm volatile ("dsb" ::: "memory");  /* retire the posted CINT before return so the
                                           * still-asserted INT line can't tail-chain a
                                           * spurious re-entry (keeps dmairq exact) */
    dmairq++;
}
/* SysTick and MU interrupts are unused here but the shared vector table
 * (startup_cm4.S) references the symbols -- keep empty stubs. */
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}

/* ---- MU B side (the CM4's) ---- */
#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define SR_TE(n)   (1u << (23 - (n)))
static void mu_send(unsigned ch, uint32_t v)
{
    while (!(MUB_SR & SR_TE(ch))) {
    }
    MUB_TR(ch) = v;
}

/* DMA register read: the WM8962 R15 device ID is captured by the eDMA_LPSR RX
 * channel (into rxbuf), completing with a native CM4 IRQ.  Returns the outcome
 * (0 ok / 2 addr-NACK / 3 data-err / 4 DMA stall / 5 timeout) -- a stall is
 * surfaced as err=4 so the CM7 PASS fails, never a false pass on rdv alone. */
static uint32_t wire_dma_read_reg(lpi2c1176_regs_t *p, uint8_t addr,
                                  const uint8_t *regptr, uint32_t np)
{
    /* (1) polled reg-pointer WRITE, bus held (sendStop=0) -- shared HW-verified
     * core; the 4.1 cold-bus discipline (pointer provably on the wire first). */
    uint32_t werr = lpi2c1176_master_write(p, addr, regptr, np, 0);
    if (werr) {
        return werr;
    }

    /* (2) repeated START + addr(R). */
    p->MSR = p->MSR;                                          /* clear stale flags */
    p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_START,
                               (uint32_t)(addr << 1) | 1u);

    /* (3) wait for the address byte to clock (TDF), watching addr-NACK / bus err. */
    uint32_t terr = 0xFFu;   /* seed so an addr-phase NACK codes 2 (shared-core convention) */
    if (!lpi2c1176_wait_flag(p, LPI2C1176_MSR_TDF,
            LPI2C1176_MSR_NDF | LPI2C1176_MSR_ALF | LPI2C1176_MSR_FEF, &terr)) {
        p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_STOP, 0);
        return terr;
    }

    /* (4) program the eDMA_LPSR RX channel: MRDR (no incr) -> rxbuf (incr),
     * 32-bit x 2, DREQ|INTMAJOR; route LPI2C5's request (DMAMUX1/LPSR src 52). */
    volatile tcd_t *t = TCD(RX_CH);
    t->SADDR = (const volatile void *)&p->MRDR;  t->SOFF = 0;
    t->ATTR = TCD_ATTR_32BIT;
    t->NBYTES = 4u;  t->SLAST = 0;
    t->DADDR = (volatile void *)rxbuf;  t->DOFF = 4;
    t->CITER = 2u;  t->BITER = 2u;  t->DLASTSGA = 0;
    t->CSR = (uint16_t)(TCD_CSR_DREQ | TCD_CSR_INTMAJOR);
    DMAMUX_LPSR_CHCFG(RX_CH) = SRC_LPI2C5 | DMAMUX_ENBL;

    /* (5) arm: SERQ enables the channel's request (ERQ).  The request LINE is
     * asserted below by MDER.RDDE with the FIFO primed (accommodation B). */
    dmairq = 0;
    rxbuf[0] = 0u;  rxbuf[1] = 0u;
    __asm volatile ("dsb" ::: "memory");   /* order the OCRAM2 buffer prep before the eDMA master runs */
    DMA_SERQ = RX_CH;

    /* (6) RXD (clock rn-1+1 = 2 bytes) THEN enable RX DMA -- see accommodation B. */
    p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_RXD, 1u);       /* rn-1 = 1 -> 2 bytes */
    p->MDER = LPI2C_MDER_RDDE;

    /* (7) wait for the eDMA_LPSR completion IRQ on the CM4's own NVIC (bounded). */
    for (uint32_t g = 0; dmairq == 0u && g < DMA_GUARD; g++) {
    }
    __asm volatile ("dsb" ::: "memory");   /* the eDMA's rxbuf writes precede the IRQ; publish them */
    p->MDER = 0u;                                            /* stop the RX-DMA request */

    if (dmairq == 0u) {
        p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_STOP, 0);
        return 4u;                                           /* DMA stalled -> visible fail, not a false pass */
    }

    /* (8) STOP + judge a clean completion (SDF, watching NDF/ALF/FEF). */
    p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_STOP, 0);
    uint32_t serr = 0u;
    if (!lpi2c1176_wait_flag(p, LPI2C1176_MSR_SDF,
            LPI2C1176_MSR_NDF | LPI2C1176_MSR_ALF | LPI2C1176_MSR_FEF, &serr)) {
        return serr;
    }
    p->MSR = LPI2C1176_MSR_SDF | LPI2C1176_MSR_EPF;          /* W1C */
    return 0u;
}

int main(void)
{
    /* self-config LPI2C5 (clock+pins+enable) via the shared HW-verified core. */
    lpi2c1176_begin(LPI2C5, &lpi2c5_hw, 100000u);
    uint32_t croot = *lpi2c5_hw.clock_root;                 /* informative */

    /* eDMA_LPSR global init (mirrors DMAChannel::begin, adapted to eDMA_LPSR).
     * This image owns eDMA_LPSR exclusively (the CM7 gate uses no DMA), so the
     * blind DMA_CR write + channel-0 use are safe; a future concurrent user must
     * read-modify-write DMA_CR and coordinate channel allocation instead. */
    CCM_LPCG23_DIRECT |= 1u;                                 /* ungate the eDMA_LPSR clock */
    DMA_CR = DMA_CR_GRP1PRI | DMA_CR_EMLM | DMA_CR_EDBG;

    /* enable the eDMA_LPSR ch0 completion IRQ on the CM4's OWN NVIC (IRQ 0). */
    NVIC_ISER0 = (1u << IRQ_DMA_RX);
    __asm volatile ("cpsie i" ::: "memory");                /* reset handler left IRQs masked */

    static const uint8_t reg_addr[2] = { 0x00u, 0x0Fu };    /* WM8962 R15 pointer, MSB first */
    uint32_t err = wire_dma_read_reg(LPI2C5, WM8962_ADDR, reg_addr, 2);
    uint32_t rdv = ((rxbuf[0] & 0xFFu) << 8) | (rxbuf[1] & 0xFFu);

    mu_send(0, 0xCAFE0001u);   /* ready (after the work, so tokens stream in order) */
    mu_send(0, croot);
    mu_send(0, rdv);
    mu_send(0, dmairq);        /* >0 = the CM4 took the eDMA_LPSR completion IRQ */
    mu_send(0, err);
    mu_send(0, 1u);            /* done */
    for (;;) {}
}
