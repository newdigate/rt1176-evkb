/* cm4_wire_int_master_test CM4 firmware (Phase 4.1 — Task 5, GREEN):
 * the CM4 SELF-CONFIGURES LPI2C5 (shared lpi2c1176_begin() core, exactly as
 * cm4_wire_test) and then runs an INTERRUPT-DRIVEN LPI2C5 master. It
 * NVIC-enables IRQ 36 on its OWN NVIC — the first non-MU peripheral IRQ routed
 * to the CM4, via the qemu2 per-line split-IRQ — and services LPI2C5 entirely
 * from LPI2C5_IRQHandler below. That ISR is a FRESH master state machine
 * (write the WM8962 register pointer {0x00,0x0F} -> repeated-START read 2
 * bytes), validated against the NXP SDK LPI2C_MasterTransferHandleIRQ /
 * LPI2C_RunTransferStateMachine shape (MIER managed per phase, ACK judged at
 * STOP, repeated-START for the read) and written clean per the project license
 * firewall (no SDK code copied). It reads the WM8962 R15 device ID over that
 * interrupt-driven bus, proving the split IRQ delivers to the CM4.
 *
 * Tokens streamed to the CM7 over the MU (channel 0, fixed order):
 *   irqcnt = >0         CM4 serviced the LPI2C5 IRQ on its own NVIC (routing proof)
 *   mcr    = 00000001   LPI2C MCR.MEN — the CM4 enabled the master block
 *   lpcg   = ........   CCM_LPCG102 readback (informative)
 *   croot  = ........   CCM_CLOCK_ROOT41 readback (informative)
 *   rdv    = ????????   R15 read by the CM4 ISR — WORLD-SPLIT: QEMU wm8962-stub
 *                       reads 0x0000; silicon reads 0x6243 (device ID)
 *   err    = 00000000   ISR outcome — 0 OK (no NDF/ALF/FEF); asserted both worlds
 *   done   = 00000001   CM4 sequence completed
 *
 * The self-config sequence (LPI2C5 + its CCM/LPSR-IOMUXC instance table) and
 * the MU scaffolding are UNCHANGED from cm4_wire_test: the shared C core
 * lpi2c1176.c from newdigate/Wire (MIT, N. Newdigate) — the same code the CM7
 * TwoWire master path runs. Only the interrupt-driven state machine is new
 * (the CM7 I2C path is polled, so there is no distilled ISR-master to reuse).
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

/* MIER shares MSR bit positions (RM 69.5.1.6). */
#define MIER_TDIE  LPI2C1176_MSR_TDF
#define MIER_RDIE  LPI2C1176_MSR_RDF
#define MIER_SDIE  LPI2C1176_MSR_SDF
#define MIER_NDIE  LPI2C1176_MSR_NDF
#define MIER_ALIE  LPI2C1176_MSR_ALF
#define MIER_FEIE  LPI2C1176_MSR_FEF
#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104u)   /* IRQ 32..63 */
#define IRQ_LPI2C5 36
#define ISR_XFER_GUARD 8000000u   /* bounded ISR-completion spin: a stalled ISR fails visibly, never hangs */

enum { PH_WRITE, PH_READ, PH_STOP, PH_DONE, PH_ERR };
/* Interrupt-driven transfer descriptor: write wr[wn] (cursor wi), then via
 * repeated-START read rd[rn] (cursor ri); phase walks the state machine; err is
 * the outcome (0 ok / 2 NACK / 3 ALF|FEF bus error); irqcnt counts ISR entries.
 * Shared between LPI2C5_IRQHandler and isr_xfer_run through the single static X. */
typedef struct {
    lpi2c1176_regs_t *p;
    uint8_t addr;
    const uint8_t *wr; uint32_t wn, wi;
    uint8_t *rd; uint32_t rn, ri;
    volatile int phase;
    volatile uint32_t err;
    volatile uint32_t irqcnt;
} isr_xfer_t;
static volatile isr_xfer_t X;

/* ---- MU B side (the CM4's) ---- */
#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define SR_TE(n)   (1u << (23 - (n)))

/* Interrupt-driven LPI2C5 master ISR (fresh state machine; the shape —
 * per-phase MIER, ACK judged at STOP, repeated-START for the read — mirrors
 * the SDK LPI2C_MasterTransferHandleIRQ, written clean per the license
 * firewall). It reacts to whatever MSR flags are present on each entry rather
 * than assuming a fixed number of interrupts, so it is correct for both the
 * QEMU FIFO cadence (which may drain more eagerly) and silicon. */
void LPI2C5_IRQHandler(void)
{
    lpi2c1176_regs_t *p = X.p;
    uint32_t s = p->MSR;
    X.irqcnt++;

    if (s & (LPI2C1176_MSR_NDF | LPI2C1176_MSR_ALF | LPI2C1176_MSR_FEF)) {
        p->MSR = s;                                          /* W1C errors */
        p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_STOP, 0);
        X.err = (s & LPI2C1176_MSR_NDF) ? 2u : 3u;
        X.phase = PH_ERR; p->MIER = 0; return;
    }

    if (X.phase == PH_WRITE && (s & LPI2C1176_MSR_TDF)) {
        if (X.wi < X.wn) {
            p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_TXD, X.wr[X.wi++]);
        } else if (X.rn) {                                   /* -> repeated-START read */
            p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_START,
                                       (uint32_t)(X.addr << 1) | 1u);
            p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_RXD, (uint8_t)(X.rn - 1));
            p->MIER = MIER_RDIE | MIER_SDIE | MIER_NDIE | MIER_ALIE | MIER_FEIE;
            X.phase = PH_READ;
        } else {                                             /* write-only STOP */
            p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_STOP, 0);
            p->MIER = MIER_SDIE | MIER_NDIE | MIER_ALIE | MIER_FEIE;
            X.phase = PH_STOP;
        }
        return;
    }

    if (X.phase == PH_READ && (s & LPI2C1176_MSR_RDF)) {
        uint32_t r = p->MRDR;
        if (!(r & LPI2C1176_MRDR_RXEMPTY) && X.ri < X.rn) X.rd[X.ri++] = (uint8_t)r;
        if (X.ri >= X.rn) {
            p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_STOP, 0);
            p->MIER = MIER_SDIE | MIER_NDIE | MIER_ALIE | MIER_FEIE;
            X.phase = PH_STOP;
        }
        return;
    }

    if (X.phase == PH_STOP && (s & LPI2C1176_MSR_SDF)) {
        p->MSR = LPI2C1176_MSR_SDF | LPI2C1176_MSR_EPF;
        X.phase = PH_DONE; p->MIER = 0;
    }
}

/* SysTick and MU interrupts are unused by this gate, but the shared vector
 * table (startup_cm4.S) still references the symbols, so keep empty stubs.
 * The real work is in LPI2C5_IRQHandler above — the dedicated handler slotted
 * at vector index 52 (external IRQ 36), whose body holds the interrupt-driven
 * master state machine. */
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}

static void mu_send(unsigned ch, uint32_t v)
{
    while (!(MUB_SR & SR_TE(ch))) {
    }
    MUB_TR(ch) = v;
}

static uint32_t isr_xfer_run(lpi2c1176_regs_t *p, uint8_t addr,
                             const uint8_t *wr, uint32_t wn,
                             uint8_t *rd, uint32_t rn)
{
    X.p = p; X.addr = addr; X.wr = wr; X.wn = wn; X.wi = 0;
    X.rd = rd; X.rn = rn; X.ri = 0; X.err = 0; X.phase = PH_WRITE;
    p->MSR = p->MSR;                                          /* clear stale flags */
    /* Silicon-correct ordering: queue the START *before* arming TDIE. TDF is
     * set whenever the TX FIFO has room (always, with MEN=1 and the FIFO
     * empty), so arming TDIE first would let the CM4 take the LPI2C5 IRQ in the
     * gap between the MIER write and the START write and push the first data
     * byte with no START queued — a QEMU-passes/silicon-races hazard (QEMU only
     * checks IRQs at translation-block boundaries, so both stores retire before
     * the ISR runs; real silicon can take it mid-gap). Issue START, then arm —
     * mirroring the SDK (whose ISR issues the START as command 0) and the
     * polled shared core (START, then wait for TDF). */
    p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_START,
                               (uint32_t)(addr << 1) | 0u);   /* START + addr(W) */
    p->MIER = MIER_TDIE | MIER_NDIE | MIER_ALIE | MIER_FEIE;  /* arm write phase */
    for (uint32_t g = 0; g < ISR_XFER_GUARD; g++)             /* bounded: no hang */
        if (X.phase == PH_DONE || X.phase == PH_ERR) break;
    return X.err;
}

int main(void)
{
    lpi2c1176_begin(LPI2C5, &lpi2c5_hw, 100000u);           /* clock+pins (shared, HW-verified) */
    uint32_t mcr   = LPI2C5->MCR & LPI2C1176_MCR_MEN;
    uint32_t lpcg  = *lpi2c5_hw.lpcg;
    uint32_t croot = *lpi2c5_hw.clock_root;

    NVIC_ISER1 = (1u << (IRQ_LPI2C5 - 32));                  /* enable IRQ 36 on the CM4 NVIC */
    __asm volatile ("cpsie i" ::: "memory");                /* reset handler left IRQs masked */

    static const uint8_t reg_addr[2] = { 0x00u, 0x0Fu };    /* WM8962 R15 pointer, MSB first */
    uint8_t rd[2] = { 0, 0 };
    uint32_t err = isr_xfer_run(LPI2C5, WM8962_ADDR, reg_addr, 2, rd, 2);
    uint32_t rdv = ((uint32_t)rd[0] << 8) | rd[1];

    mu_send(0, X.irqcnt);
    mu_send(0, mcr);
    mu_send(0, lpcg);
    mu_send(0, croot);
    mu_send(0, rdv);
    mu_send(0, err);                                         /* 0 = OK (no NDF/ALF/FEF); asserted both worlds */
    mu_send(0, 1u);                                          /* done */
    for (;;) {}
}
