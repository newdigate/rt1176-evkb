/* cm4_wire_test CM4 firmware (Phase 3.2): the CM4 SELF-CONFIGURES LPI2C5 (the
 * on-board WM8962 codec bus) and runs three polled I2C master transactions,
 * streaming each observation to the CM7 over the MU (channel 0, in order):
 *   1. reset-write R15<-0x6243 to the WM8962 @0x1A  -> ack  (expect 0)
 *   2. zero-byte probe of absent address 0x2A       -> nack (expect 2)
 *   3. device-ID read-back of R15 (repeated START)  -> rdn=2, rdv
 *
 * Adapted from this project's own newdigate/Wire WireIMXRT1176.cpp master path
 * (begin/setClock/endTransmission/requestFrom, MIT, N. Newdigate) and the
 * WM8962 register protocol from newdigate/Audio control_wm8962.cpp (MIT) — the
 * HW-verified LPI2C5 self-config + polled-master sequences, re-expressed in C
 * for the bare-metal CM4 image.  No logic change; register/clock/pin values
 * identical.  Keep in sync with WireIMXRT1176.cpp; Phase 3.3 consolidates onto
 * a shared C core.  The R15 readback default 0x6243 (= device ID) is a
 * hardware FACT taken from the Linux wm8962.c reg_default table (2026-07-18);
 * no code was taken from that GPL source.
 *
 * SILICON TRUTH: the qemu2 LPI2C model + wm8962-stub respond on MCR.MEN alone
 * (clock gate / clock root / LPSR pin mux ignored), so a green QEMU run proves
 * only the register/transfer sequence; the wiring-free EVKB run (real codec
 * ACK + ID) is what proves the CM4 brought up the clock and LPSR pins itself.
 * ACK/NACK is judged at STOP completion (SDF wait watching NDF), NEVER at TDF
 * — TDF leads the ACK bit by a byte-time on silicon (WireIMXRT1176.cpp note;
 * the qemu model's deferred-NDF mirrors exactly this).
 * Public-domain scaffolding (N. Newdigate); adapted register logic MIT. */
#include <stdint.h>

/* ---- CCM (shared): LPI2C5 clock gate + root (imxrt1176.h) ---- */
#define CCM_LPCG102_DIRECT       (*(volatile uint32_t *)0x40CC6CC0u) /* LPI2C5 gate */
#define CCM_CLOCK_ROOT41_CONTROL (*(volatile uint32_t *)0x40CC1480u) /* LPI2C5 root */
#define CROOT41_VAL  (1u << 8)   /* mux 1 -> 24 MHz (WireIMXRT1176 lpi2c5_hardware) */

/* ---- LPSR IOMUXC: SCL=GPIO_LPSR_05, SDA=GPIO_LPSR_04, ALT0|SION ---- */
#define IOMUX_MUX_LPSR_04 (*(volatile uint32_t *)0x40C08010u)  /* SDA */
#define IOMUX_MUX_LPSR_05 (*(volatile uint32_t *)0x40C08014u)  /* SCL */
#define IOMUX_PAD_LPSR_04 (*(volatile uint32_t *)0x40C08050u)
#define IOMUX_PAD_LPSR_05 (*(volatile uint32_t *)0x40C08054u)
#define IOMUX_SCL_SELECT  (*(volatile uint32_t *)0x40C08084u)  /* LPI2C5_SCL_SELECT_INPUT */
#define IOMUX_SDA_SELECT  (*(volatile uint32_t *)0x40C08088u)  /* LPI2C5_SDA_SELECT_INPUT */
#define IOMUX_ALT0_SION 0x10u
#define IOMUX_PAD_OD    0x0Au    /* LPSR open-drain pad config (lpi2c5_hardware) */
#define IOMUX_DAISY     0x0u

/* ---- LPI2C5 (base 0x40C34000; offsets == IMXRT_LPI2C_t / qemu2 imxrt_lpi2c) ---- */
#define LPI2C5_BASE  0x40C34000u
#define LPI2C_MCR    (*(volatile uint32_t *)(LPI2C5_BASE + 0x10u))
#define LPI2C_MSR    (*(volatile uint32_t *)(LPI2C5_BASE + 0x14u))
#define LPI2C_MCFGR1 (*(volatile uint32_t *)(LPI2C5_BASE + 0x24u))
#define LPI2C_MCCR0  (*(volatile uint32_t *)(LPI2C5_BASE + 0x48u))
#define LPI2C_MTDR   (*(volatile uint32_t *)(LPI2C5_BASE + 0x60u))
#define LPI2C_MRDR   (*(volatile uint32_t *)(LPI2C5_BASE + 0x70u))
#define MCR_MEN  (1u << 0)
#define MCR_RST  (1u << 1)
#define MCR_RTF  (1u << 8)
#define MCR_RRF  (1u << 9)
#define MSR_TDF  (1u << 0)
#define MSR_RDF  (1u << 1)
#define MSR_EPF  (1u << 8)
#define MSR_SDF  (1u << 9)
#define MSR_NDF  (1u << 10)
#define MSR_ALF  (1u << 11)
#define MSR_FEF  (1u << 12)
#define TX_CMD(cmd, data)  (((uint32_t)(cmd) << 8) | ((data) & 0xFFu))
#define CMD_TXD    0u
#define CMD_RXD    1u
#define CMD_STOP   2u
#define CMD_START  4u
#define MRDR_RXEMPTY (1u << 14)
/* setClock(100000) @24 MHz -> pre=1, div=120, clklo=63 (clamped from 72),
 * clkhi=48, DATAVD=SETHOLD=clkhi/2=24 (WireIMXRT1176.cpp::setClock math). */
#define MCFGR1_VAL  0x1u
#define MCCR0_VAL   0x1818303Fu
#define WIRE_TIMEOUT 100000u

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

/* Wait until any bit in `mask` is set, or an error bit appears / timeout.
 * Mirrors TwoWire::wait_flag: on NDF, *err's prior value classifies the NACK
 * (0xFF = address NACK -> 2, else data NACK -> 3); ALF/FEF -> 4; timeout 5. */
static int wait_flag(uint32_t mask, uint32_t error_mask, uint32_t *err)
{
    for (uint32_t g = 0; g < WIRE_TIMEOUT; g++) {
        uint32_t s = LPI2C_MSR;
        if (s & error_mask) {
            if (s & MSR_NDF) *err = (*err == 0xFFu) ? 2u : 3u;
            else *err = 4u;
            LPI2C_MSR = s;                       /* W1C the flags */
            return 0;
        }
        if (s & mask) return 1;
    }
    *err = 5u;
    return 0;
}

/* After a NACK/error, flush the FIFOs so the next transaction starts clean
 * (TwoWire::bus_recover). */
static void bus_recover(void)
{
    LPI2C_MCR = MCR_MEN | MCR_RTF | MCR_RRF;
    LPI2C_MCR = MCR_MEN;
    LPI2C_MSR = LPI2C_MSR;
}

/* Polled master write, mirroring TwoWire::endTransmission(sendStop):
 * START+addr(W), per byte TDF-wait + TXD, optional STOP with the ACK/NACK
 * judged at the SDF wait (watching NDF).  Returns 0 ok / 2 addr-NACK /
 * 3 data-NACK / 4 error / 5 timeout. */
static uint32_t i2c_write(uint8_t addr, const uint8_t *data, uint32_t len,
                          int send_stop)
{
    uint32_t err = 0xFFu;                        /* NACK now = address NACK */
    LPI2C_MSR = LPI2C_MSR;                       /* clear stale flags */
    LPI2C_MTDR = TX_CMD(CMD_START, (uint32_t)(addr << 1) | 0u);
    for (uint32_t i = 0; i < len; i++) {
        if (!wait_flag(MSR_TDF, MSR_NDF | MSR_ALF | MSR_FEF, &err)) {
            bus_recover();
            return err;
        }
        err = 0u;                                /* past the address */
        LPI2C_MTDR = TX_CMD(CMD_TXD, data[i]);
    }
    if (send_stop) {
        LPI2C_MTDR = TX_CMD(CMD_STOP, 0);
        if (!wait_flag(MSR_SDF, MSR_NDF | MSR_ALF | MSR_FEF, &err)) {
            bus_recover();
            return err;
        }
        LPI2C_MSR = MSR_SDF | MSR_EPF;
    }
    return 0u;
}

/* Polled master read, mirroring TwoWire::requestFrom: repeated-START+addr(R),
 * RXD with N-1 encoding, per-byte RDF-wait + MRDR, STOP.  Returns bytes read. */
static uint32_t i2c_read(uint8_t addr, uint8_t *buf, uint32_t quantity)
{
    uint32_t err = 0xFFu, n = 0;
    LPI2C_MSR = LPI2C_MSR;
    LPI2C_MTDR = TX_CMD(CMD_START, (uint32_t)(addr << 1) | 1u);
    if (!wait_flag(MSR_TDF, MSR_NDF | MSR_ALF | MSR_FEF, &err)) {
        LPI2C_MTDR = TX_CMD(CMD_STOP, 0);
        return 0;
    }
    LPI2C_MTDR = TX_CMD(CMD_RXD, (uint8_t)(quantity - 1));
    for (uint32_t i = 0; i < quantity; i++) {
        err = 0u;
        if (!wait_flag(MSR_RDF, MSR_ALF | MSR_FEF, &err)) break;
        uint32_t r = LPI2C_MRDR;
        if (r & MRDR_RXEMPTY) break;
        buf[n++] = (uint8_t)(r & 0xFFu);
    }
    LPI2C_MTDR = TX_CMD(CMD_STOP, 0);
    wait_flag(MSR_SDF, MSR_ALF | MSR_FEF, &err);
    LPI2C_MSR = MSR_SDF | MSR_EPF;
    return n;
}

int main(void)
{
    /* --- self-config LPI2C5 (mirrors TwoWire::begin for lpi2c5_hardware) --- */
    CCM_LPCG102_DIRECT = 1u;                 /* ungate the LPI2C5 clock */
    CCM_CLOCK_ROOT41_CONTROL = CROOT41_VAL;  /* mux 1 -> 24 MHz */

    IOMUX_MUX_LPSR_05 = IOMUX_ALT0_SION;  IOMUX_PAD_LPSR_05 = IOMUX_PAD_OD;  /* SCL */
    IOMUX_MUX_LPSR_04 = IOMUX_ALT0_SION;  IOMUX_PAD_LPSR_04 = IOMUX_PAD_OD;  /* SDA */
    IOMUX_SCL_SELECT = IOMUX_DAISY;
    IOMUX_SDA_SELECT = IOMUX_DAISY;

    LPI2C_MCR = MCR_RST;  LPI2C_MCR = 0u;    /* reset the master block */
    LPI2C_MCFGR1 = MCFGR1_VAL;               /* prescale 1 (MEN=0) */
    LPI2C_MCCR0  = MCCR0_VAL;                /* ~100 kHz timing (MEN=0) */
    LPI2C_MCR = MCR_MEN;                     /* enable */

    /* --- config readbacks --- */
    uint32_t mcr   = LPI2C_MCR & MCR_MEN;              /* -> 1 */
    uint32_t lpcg  = CCM_LPCG102_DIRECT;               /* informative */
    uint32_t croot = CCM_CLOCK_ROOT41_CONTROL;         /* informative */

    /* --- 1. reset-write R15<-0x6243 (WM8962_Init's own first write) --- */
    static const uint8_t reset_wr[4] = { 0x00u, 0x0Fu, 0x62u, 0x43u };
    uint32_t ack = i2c_write(WM8962_ADDR, reset_wr, 4, 1);

    /* --- 2. zero-byte probe of an absent address -> address NACK --- */
    uint32_t nack = i2c_write(ABSENT_ADDR, 0, 0, 1);

    /* --- 3. device-ID read-back of R15 (write reg addr, repeated START) --- */
    static const uint8_t reg_addr[2] = { 0x00u, 0x0Fu };
    uint8_t rd[2] = { 0, 0 };
    uint32_t rdn = 0, rdv = 0;
    if (i2c_write(WM8962_ADDR, reg_addr, 2, 0) == 0u) {   /* no STOP */
        rdn = i2c_read(WM8962_ADDR, rd, 2);
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
