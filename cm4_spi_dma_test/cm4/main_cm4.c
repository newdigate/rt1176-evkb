/* cm4_spi_dma_test CM4 firmware (Phase 4.3): the CM4 SELF-CONFIGURES LPSPI1 via
 * the shared lpspi1176 core (polled begin), then runs full-duplex eDMA
 * self-loopback (SDO->SDI jumper) with 2 channels distilled from SPI.cpp's
 * startDMA/dma_rxisr — direct TCD/DMAMUX writes, NO DMAChannel/EventResponder.
 *   RX ch0: RDR(8-bit) -> rxbuf, DMAMUX src LPSPI1_RX=36 (its completion is the
 *           transfer's completion — RDR drains last)
 *   TX ch1: txbuf -> TDR(8-bit), DMAMUX src LPSPI1_TX=37
 * STAGE_BLOCKING polls the RX channel CSR.DONE; STAGE_ASYNC arms RX CSR.INTMAJOR
 * and NVIC-enables IRQ 0 (eDMA ch0 completion, on the CM4's OWN NVIC via the
 * qemu2 split-IRQ) so DMA_CH0_IRQHandler sets dmairq — the isolated routing
 * proof. Buffers live in OCRAM2 (.dmabuf): the eDMA is a system-bus master and
 * cannot reach the CM4's private DTCM.
 *
 * Silicon-truth: the qemu2 ssi-loopback echoes on CR.MEN alone, so rx==tx in
 * QEMU proves only the TCD/DMA sequence; the real SDO->SDI jumper on hardware
 * proves clock+pins+SCK. Public-domain scaffolding (N. Newdigate); shared-core
 * register logic MIT (newdigate/SPI). */
#include <stdint.h>
#include "lpspi1176.h"

/* LPSPI1 + its CCM/IOMUXC instance addresses (imxrt1176.h values; verbatim from
 * cm4_spi_test). */
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

/* ---- eDMA (base 0x40070000; same IP as Teensy 4, relocated) ---- */
#define DMA_CR      (*(volatile uint32_t *)0x40070000u)
#define DMA_SERQ    (*(volatile uint8_t  *)0x4007001Bu)   /* set enable request (channel #) */
#define DMA_CINT    (*(volatile uint8_t  *)0x4007001Fu)   /* clear interrupt (channel #) */
#define DMA_CR_GRP1PRI (1u << 10)
#define DMA_CR_EMLM    (1u << 7)
#define DMA_CR_EDBG    (1u << 1)
#define CCM_LPCG22_DIRECT (*(volatile uint32_t *)0x40CC62C0u)  /* eDMA clock gate */
#define DMAMUX_CHCFG(ch)  (*(volatile uint32_t *)(0x40074000u + (ch) * 4u))
#define DMAMUX_ENBL       (1u << 31)
#define SRC_LPSPI1_RX     36u
#define SRC_LPSPI1_TX     37u
#define TCD_CSR_DONE      0x0080u
#define TCD_CSR_DREQ      0x0008u   /* disable channel request on major-loop completion */
#define TCD_CSR_INTMAJOR  0x0002u
#define RX_CH  0u
#define TX_CH  1u
#define IRQ_DMA_RX  0u             /* channel 0 -> NVIC IRQ 0 */
#define NVIC_ISER0  (*(volatile uint32_t *)0xE000E100u)   /* IRQ 0..31 */
#define DMA_GUARD   4000000u

/* eDMA TCD — exact hardware layout (matches cores DMAChannel TCD_t, 32 bytes). */
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
#define TCD(ch)  ((volatile tcd_t *)(0x40071000u + (ch) * 0x20u))

/* DMA buffers in OCRAM2 (system-visible; DTCM is DMA-unreachable). */
#define N 16u
/* volatile so the rx==tx compare can't be hoisted across the DMA-arm barrier. */
static volatile uint8_t txbuf[N] __attribute__((section(".dmabuf")));
static volatile uint8_t rxbuf[N] __attribute__((section(".dmabuf")));

static volatile uint32_t dmairq = 0;
void DMA_CH0_IRQHandler(void)
{
    DMA_CINT = RX_CH;    /* clear ch0 interrupt request */
    __asm volatile ("dsb" ::: "memory");  /* retire the posted CINT before return so the
                                           * still-asserted INT line can't tail-chain a
                                           * spurious re-entry (keeps dmairq exact) */
    dmairq++;
}
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

static uint32_t g_tcr_base = 0;

/* Program one channel's TCD + DMAMUX. src/dst are byte pointers; soff/doff are
 * per-minor-loop increments (0 for a register, 1 for a buffer). 8-bit ATTR. */
static void dma_setup(uint8_t ch, const volatile void *src, volatile void *dst,
                      int16_t soff, int16_t doff, uint16_t n, uint8_t mux_src, int intr)
{
    volatile tcd_t *t = TCD(ch);
    t->SADDR = src; t->SOFF = soff; t->ATTR = 0u;   /* SSIZE=DSIZE=0 (8-bit) */
    t->NBYTES = 1u; t->SLAST = 0; t->DADDR = dst; t->DOFF = doff;
    t->CITER = n; t->BITER = n; t->DLASTSGA = 0;
    t->CSR = (uint16_t)(TCD_CSR_DREQ | (intr ? TCD_CSR_INTMAJOR : 0u));
    DMAMUX_CHCFG(ch) = ((uint32_t)mux_src & 0x7Fu) | DMAMUX_ENBL;
}

/* One full-duplex DMA transfer. async=1 -> wait on the RX completion IRQ
 * (dmairq); async=0 -> poll the RX channel CSR.DONE. Returns 1 if rx==tx. */
static uint32_t run_dma(int async)
{
    for (uint32_t i = 0; i < N; i++) rxbuf[i] = 0u;

    /* RX drains RDR -> rxbuf (register src no-incr, buffer dst incr). */
    dma_setup(RX_CH, (const volatile void *)&LPSPI1->RDR, rxbuf, 0, 1, (uint16_t)N,
              SRC_LPSPI1_RX, async);
    /* TX feeds txbuf -> TDR (buffer src incr, register dst no-incr). */
    dma_setup(TX_CH, txbuf, (volatile void *)&LPSPI1->TDR, 1, 0, (uint16_t)N,
              SRC_LPSPI1_TX, 0);

    LPSPI1->TCR = (g_tcr_base & ~LPSPI1176_TCR_FRAMESZ(0xFFF)) | LPSPI1176_TCR_FRAMESZ(7); /* 8-bit */
    LPSPI1->FCR = 0u;                                        /* watermark 0 */
    dmairq = 0;
    LPSPI1->DER = LPSPI1176_DER_TDDE | LPSPI1176_DER_RDDE;   /* both DMA requests */
    /* Order the OCRAM2 buffer prep (txbuf fill + rxbuf-zero, Normal-memory stores
     * that may linger in the M4 write buffer) BEFORE arming the eDMA master — else
     * it can read stale txbuf or have its rxbuf writes clobbered by a late-landing
     * rxbuf-zero (WAW). The intervening Device writes don't drain Normal stores.
     * SPI.cpp got this free via arm_dcache_flush()'s DSB. QEMU's eDMA is synchronous
     * so it can't expose this — silicon-only ordering. */
    __asm volatile ("dsb" ::: "memory");
    DMA_SERQ = RX_CH;                                        /* arm RX before TX */
    DMA_SERQ = TX_CH;                                        /* arm TX -> transfer runs */

    uint32_t g = 0;
    if (async) {
        while (dmairq == 0u && ++g < DMA_GUARD) { }
    } else {
        while (!(TCD(RX_CH)->CSR & TCD_CSR_DONE) && ++g < DMA_GUARD) { }
    }
    __asm volatile ("dsb" ::: "memory");
    LPSPI1->DER = 0u;                                        /* stop DMA requests */

    uint32_t okc = 1u;
    for (uint32_t i = 0; i < N; i++) if (rxbuf[i] != txbuf[i]) okc = 0u;
    return okc;
}

int main(void)
{
    /* --- eDMA global init (mirrors DMAChannel::begin) --- */
    CCM_LPCG22_DIRECT |= 1u;                                 /* ungate eDMA clock */
    /* SHARED eDMA STATE owned globally by this image: this blind DMA_CR write (GRP1PRI,
     * EMLM) plus channels 0/1 are safe now only because the CM7 gate uses no DMA. A
     * future CM7- or second-CM4-task that uses the eDMA MUST read-modify-write DMA_CR
     * and coordinate channel allocation instead of clobbering this. */
    DMA_CR = DMA_CR_GRP1PRI | DMA_CR_EMLM | DMA_CR_EDBG;

    /* --- self-config LPSPI1 via the shared core (4 MHz) --- */
    lpspi1176_begin(LPSPI1, &lpspi1_hw, 4000000u, &g_tcr_base);
    uint32_t cr    = LPSPI1->CR & LPSPI1176_CR_MEN;          /* -> 1 */
    uint32_t cfgr1 = LPSPI1->CFGR1 & LPSPI1176_CFGR1_MASTER; /* -> 1 */
    uint32_t lpcg  = *lpspi1_hw.lpcg;
    uint32_t croot = *lpspi1_hw.clock_root;

    for (uint32_t i = 0; i < N; i++) txbuf[i] = (uint8_t)(0xA0u ^ (i * 7u));

    /* STAGE_BLOCKING: poll the RX channel DONE (no split needed). */
    uint32_t rxb = run_dma(0);

    /* STAGE_ASYNC: RX major-complete IRQ on the CM4's own NVIC (eDMA split). */
    NVIC_ISER0 = (1u << IRQ_DMA_RX);                         /* enable IRQ 0 */
    __asm volatile ("cpsie i" ::: "memory");                /* reset handler left IRQs masked */
    uint32_t rxa = run_dma(1);

    mu_send(0, 0xCAFE0001u);   /* ready (sent after the work so tokens stream in order) */
    mu_send(0, cr);
    mu_send(0, cfgr1);
    mu_send(0, lpcg);
    mu_send(0, croot);
    mu_send(0, rxb);
    mu_send(0, dmairq);
    mu_send(0, rxa);
    mu_send(0, 1u);            /* done */
    for (;;) {}
}
