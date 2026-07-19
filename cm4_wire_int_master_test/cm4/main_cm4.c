/* cm4_wire_int_master_test CM4 firmware (Phase 4.1 — HW-VERIFIED):
 * the CM4 SELF-CONFIGURES LPI2C5 (shared lpi2c1176_begin() core, exactly as
 * cm4_wire_test), NVIC-enables IRQ 36 on its OWN NVIC — the first non-MU
 * peripheral IRQ routed to the CM4, via the qemu2 per-line split-IRQ — and
 * reads the WM8962 R15 device ID with an INTERRUPT-DRIVEN read: the CM4's
 * LPI2C5 ISR (LPI2C5_IRQHandler) captures each RX byte (RDF) and completes on
 * SDF, on its own NVIC — proving the split IRQ delivers to the CM4 (irqcnt>0).
 *
 * ★ SILICON-TRUTH (probe cm4_wire_int_master, EVKB 2026-07-19): an earlier
 * fully-ISR master that issued the repeated START from the ISR the instant the
 * write cursor drained RACED the last register-pointer byte still clocking on a
 * COLD bus — the WM8962 never latched the pointer, so the read returned the
 * wrong register (0x0000).  Deterministic cold=0x0000 / warm=0x6243, same
 * irqcnt; QEMU's wm8962-stub (all reads 0x0000) structurally could not expose
 * it.  Fix (i2c_read_reg): sequence write->read exactly as the HW-verified
 * polled core does — the register-pointer WRITE via polled
 * lpi2c1176_master_write (sendStop=0, bus held), the repeated START + (after a
 * TDF wait) RXD, then the interrupt-driven RX capture.
 *
 * Tokens streamed to the CM7 over the MU (channel 0, fixed order):
 *   irqcnt = >0         CM4 serviced the LPI2C5 read IRQ on its own NVIC
 *                       (routing proof; value world-varies, only >0 asserted)
 *   mcr    = 00000001   LPI2C MCR.MEN — the CM4 enabled the master block
 *   lpcg   = ........   CCM_LPCG102 readback (informative)
 *   croot  = ........   CCM_CLOCK_ROOT41 readback (informative)
 *   rdv    = ????????   R15 read by the CM4 ISR — WORLD-SPLIT: QEMU wm8962-stub
 *                       reads 0x0000; silicon reads 0x6243 (device ID)
 *   err    = 00000000   ISR outcome — 0 OK (no NDF/ALF/FEF); asserted both worlds
 *   done   = 00000001   CM4 sequence completed
 *
 * The self-config AND the polled register-pointer write reuse the shared C core
 * lpi2c1176.c from newdigate/Wire (MIT, N. Newdigate) — the same code the CM7
 * TwoWire master path runs; only the read-completion ISR is new.  The R15
 * device-ID value (0x6243) is the same hardware FACT cm4_wire_test established:
 * from the Linux wm8962.c reg_default table (2026-07-18); no code from that GPL
 * source is used, only the fact.
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
#define MIER_RDIE  LPI2C1176_MSR_RDF
#define MIER_SDIE  LPI2C1176_MSR_SDF
#define MIER_NDIE  LPI2C1176_MSR_NDF
#define MIER_ALIE  LPI2C1176_MSR_ALF
#define MIER_FEIE  LPI2C1176_MSR_FEF
#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104u)   /* IRQ 32..63 */
#define IRQ_LPI2C5 36
#define ISR_XFER_GUARD 8000000u   /* bounded ISR-completion spin: a stalled ISR fails visibly, never hangs */

enum { PH_READ, PH_STOP, PH_DONE, PH_ERR };
/* Read-transfer descriptor: read rd[rn] (cursor ri); phase walks
 * READ->STOP->DONE (or ERR); err is the outcome (0 ok / 2 NACK / 3 ALF|FEF bus
 * error); irqcnt counts ISR entries.  Shared between LPI2C5_IRQHandler and
 * i2c_read_reg through the single static X. */
typedef struct {
    lpi2c1176_regs_t *p;
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

/* LPI2C5 read-completion ISR: services LPI2C5 on the CM4's own NVIC (IRQ 36,
 * via the qemu2 split-IRQ) to capture each RX byte (RDF) and complete the read
 * (SDF).  It reacts to whatever MSR flags are present per entry, so it is
 * correct for both the QEMU FIFO cadence and silicon's byte-paced RDF.  (The
 * register-pointer write and the repeated START are issued in i2c_read_reg
 * below, not here — see the silicon-truth note in the file header.) */
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
 * at vector index 52 (external IRQ 36), which captures the interrupt-driven
 * read. */
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}

static void mu_send(unsigned ch, uint32_t v)
{
    while (!(MUB_SR & SR_TE(ch))) {
    }
    MUB_TR(ch) = v;
}

/* Interrupt-driven register read.
 *
 * SILICON-TRUTH FIX (probe cm4_wire_int_master, EVKB 2026-07-19): issuing the
 * repeated START for the read from the ISR the instant the write cursor drained
 * RACED the last register-pointer byte still clocking out on a *cold* bus, so
 * the WM8962 never latched the pointer and the read returned the wrong register
 * (0x0000).  Deterministic cold=0x0000 / warm=0x6243, same irqcnt — QEMU's stub
 * (all reads 0x0000) could not expose it.  Fix: sequence write->read exactly as
 * the HW-verified polled core does (which never hit this): do the register-
 * pointer WRITE with the polled `lpi2c1176_master_write` (sendStop=0, bus held),
 * then issue the repeated START and, after a TDF wait, the RXD — so the pointer
 * write is provably on the wire before the read command.  The DATA READ stays
 * interrupt-driven: the CM4's LPI2C5 ISR captures each RX byte (RDF) and
 * completes on SDF, on its own NVIC (IRQ 36) — the split-IRQ proof (irqcnt>0). */
static uint32_t i2c_read_reg(lpi2c1176_regs_t *p, uint8_t addr,
                             const uint8_t *regptr, uint32_t np,
                             uint8_t *rd, uint32_t rn)
{
    uint32_t werr = lpi2c1176_master_write(p, addr, regptr, np, 0); /* set ptr, hold bus */
    if (werr) return werr;

    X.p = p; X.rd = rd; X.rn = rn; X.ri = 0; X.err = 0; X.phase = PH_READ;
    p->MSR = p->MSR;                                          /* clear stale flags */
    p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_START,
                               (uint32_t)(addr << 1) | 1u);   /* repeated START + addr(R) */
    uint32_t terr = 0xFFu;   /* seed so an addr-phase NACK codes 2 (shared-core convention) */
    if (!lpi2c1176_wait_flag(p, LPI2C1176_MSR_TDF,
            LPI2C1176_MSR_NDF | LPI2C1176_MSR_ALF | LPI2C1176_MSR_FEF, &terr)) {
        p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_STOP, 0);
        return terr;         /* wait_flag set it (2 addr-NACK / 3 bus-err) */
    }
    p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_RXD, (uint8_t)(rn - 1));
    p->MIER = MIER_RDIE | MIER_SDIE | MIER_NDIE | MIER_ALIE | MIER_FEIE; /* arm read ISR */
    for (uint32_t g = 0; g < ISR_XFER_GUARD; g++)             /* bounded: no hang */
        if (X.phase == PH_DONE || X.phase == PH_ERR) break;
    /* A stalled ISR (guard expired before PH_DONE) must NOT report false
     * success — surface it as err=4 so the CM7 PASS fails, not just rdv. */
    if (X.phase != PH_DONE && X.err == 0u) X.err = 4u;
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
    uint32_t err = i2c_read_reg(LPI2C5, WM8962_ADDR, reg_addr, 2, rd, 2);
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
