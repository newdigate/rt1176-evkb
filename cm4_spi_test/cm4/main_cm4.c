/* cm4_spi_test CM4 firmware (Phase 3.1): the CM4 SELF-CONFIGURES LPSPI1 and runs
 * a polled master self-loopback (external SDO->SDI jumper), streaming each
 * observation to the CM7 over the MU (channel 0, in order).
 *
 * Adapted from this project's own newdigate/SPI SPIIMXRT1176.cpp begin()/
 * transfer() (MIT, N. Newdigate) — the HW-verified LPSPI1 self-config +
 * polled-master sequence, re-expressed in C for the bare-metal CM4 image. No
 * logic change; every register/clock/pin value is identical. Keep in sync with
 * SPIIMXRT1176.cpp; Phase 3.3 consolidates both onto a shared C core.
 *
 * SILICON TRUTH: the qemu2 board attaches an ssi-loopback child to LPSPI1 that
 * echoes on CR.MEN ALONE — it ignores the clock gate, clock root, and pin mux.
 * So rx==tx in QEMU proves only the register/transfer sequence; the real
 * SDO->SDI jumper on hardware is what proves the CM4 ungated the clock + muxed
 * the pins + drove a real SCK.  Public-domain scaffolding (N. Newdigate);
 * adapted register logic MIT as noted above. */
#include <stdint.h>

/* ---- CCM (shared): LPSPI1 clock gate + root (imxrt1176.h) ---- */
#define CCM_LPCG104_DIRECT       (*(volatile uint32_t *)0x40CC6D00u) /* LPSPI1 gate */
#define CCM_CLOCK_ROOT43_CONTROL (*(volatile uint32_t *)0x40CC1580u) /* LPSPI1 root */

/* ---- IOMUXC (shared): SCK=GPIO_AD_28, SDO=GPIO_AD_30, SDI=GPIO_AD_31, ALT0 ---- */
#define IOMUX_MUX_AD_28  (*(volatile uint32_t *)0x400E817Cu)
#define IOMUX_MUX_AD_30  (*(volatile uint32_t *)0x400E8184u)
#define IOMUX_MUX_AD_31  (*(volatile uint32_t *)0x400E8188u)
#define IOMUX_PAD_AD_28  (*(volatile uint32_t *)0x400E83C0u)
#define IOMUX_PAD_AD_30  (*(volatile uint32_t *)0x400E83C8u)
#define IOMUX_PAD_AD_31  (*(volatile uint32_t *)0x400E83CCu)
#define IOMUX_SCK_SELECT (*(volatile uint32_t *)0x400E85D0u)  /* LPSPI1_SCK_SELECT_INPUT */
#define IOMUX_SDO_SELECT (*(volatile uint32_t *)0x400E85D8u)  /* LPSPI1_SDO_SELECT_INPUT */
#define IOMUX_SDI_SELECT (*(volatile uint32_t *)0x400E85D4u)  /* LPSPI1_SDI_SELECT_INPUT */
#define IOMUX_ALT0   0x0u
#define IOMUX_PAD    0x0Cu    /* SPIIMXRT1176.cpp pad_ctl_val (DSE set) */
#define IOMUX_DAISY  0x1u     /* select-input value */

/* ---- LPSPI1 (base 0x40114000; offsets == IMXRT_LPSPI_t / qemu2 imxrt_lpspi) ---- */
#define LPSPI1_BASE  0x40114000u
#define LPSPI_CR     (*(volatile uint32_t *)(LPSPI1_BASE + 0x10u))
#define LPSPI_CFGR1  (*(volatile uint32_t *)(LPSPI1_BASE + 0x24u))
#define LPSPI_CCR    (*(volatile uint32_t *)(LPSPI1_BASE + 0x40u))
#define LPSPI_TCR    (*(volatile uint32_t *)(LPSPI1_BASE + 0x60u))
#define LPSPI_TDR    (*(volatile uint32_t *)(LPSPI1_BASE + 0x64u))
#define LPSPI_RSR    (*(volatile uint32_t *)(LPSPI1_BASE + 0x70u))
#define LPSPI_RDR    (*(volatile uint32_t *)(LPSPI1_BASE + 0x74u))
#define CR_MEN       (1u << 0)
#define CR_RST       (1u << 1)
#define CFGR1_MASTER (1u << 0)
#define RSR_RXEMPTY  (1u << 1)
#define TCR_BASE     0u          /* MODE0, MSB-first, prescale 0 */
#define SCKDIV_4MHZ  4u          /* SCK = 24MHz / (2^0 * (4+2)) = 4 MHz */
#define SPI_TIMEOUT  100000u

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

/* Polled full-duplex transfer of one frame (framesz = bits-1), mirroring
 * SPIIMXRT1176.cpp::transfer(): load TCR, write TDR, spin on RSR.RXEMPTY, read
 * RDR.  Returns 0xFFFFFFFF on timeout (no functional clock -> nothing shifts). */
static uint32_t spi_transfer(uint32_t data, uint32_t framesz)
{
    LPSPI_TCR = TCR_BASE | (framesz & 0xFFFu);
    LPSPI_TDR = data;
    for (uint32_t g = 0; g < SPI_TIMEOUT; g++) {
        if (!(LPSPI_RSR & RSR_RXEMPTY)) {
            return LPSPI_RDR;
        }
    }
    return 0xFFFFFFFFu;
}

int main(void)
{
    /* --- self-config LPSPI1 (mirrors SPIIMXRT1176.cpp::begin) --- */
    CCM_LPCG104_DIRECT = 1u;               /* ungate the LPSPI1 clock */
    CCM_CLOCK_ROOT43_CONTROL = 0u;         /* mux0 OSC24M, div0 -> 24 MHz */

    IOMUX_MUX_AD_28 = IOMUX_ALT0;  IOMUX_PAD_AD_28 = IOMUX_PAD;  /* SCK */
    IOMUX_MUX_AD_30 = IOMUX_ALT0;  IOMUX_PAD_AD_30 = IOMUX_PAD;  /* SDO */
    IOMUX_MUX_AD_31 = IOMUX_ALT0;  IOMUX_PAD_AD_31 = IOMUX_PAD;  /* SDI */
    IOMUX_SCK_SELECT = IOMUX_DAISY;
    IOMUX_SDO_SELECT = IOMUX_DAISY;
    IOMUX_SDI_SELECT = IOMUX_DAISY;

    LPSPI_CR = CR_RST;  LPSPI_CR = 0u;     /* reset the block (MEN=0) */
    LPSPI_CFGR1 = CFGR1_MASTER;            /* master mode (write while MEN=0) */
    LPSPI_CCR = (LPSPI_CCR & ~0xFFu) | SCKDIV_4MHZ;  /* SCKDIV for 4 MHz */
    LPSPI_CR = CR_MEN;                     /* enable */

    /* --- config readbacks --- */
    uint32_t cr    = LPSPI_CR & CR_MEN;             /* -> 1 */
    uint32_t cfgr1 = LPSPI_CFGR1 & CFGR1_MASTER;    /* -> 1 */
    uint32_t lpcg  = CCM_LPCG104_DIRECT;            /* informative */
    uint32_t croot = CCM_CLOCK_ROOT43_CONTROL;      /* informative */

    /* --- polled loopback (SDO->SDI jumper) --- */
    uint32_t a = spi_transfer(0xA5u, 7u) & 0xFFu;        /* 8-bit */
    uint32_t b = spi_transfer(0x3Cu, 7u) & 0xFFu;        /* 8-bit */
    uint32_t w = spi_transfer(0xBEEFu, 15u) & 0xFFFFu;   /* 16-bit */
    uint8_t bs[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };
    for (int i = 0; i < 4; i++) {
        bs[i] = (uint8_t)(spi_transfer(bs[i], 7u) & 0xFFu);
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
