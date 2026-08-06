/* cm4_usb_irq_probe: can the CM4 own the USB host port outright -- bring up
 * the PHY and EHCI controller itself, and take USB OTG2's interrupt (IRQ 135,
 * RM Table 4-2 line 3910) on its own NVIC?
 *
 * Phase 7.1 of the CM4 USB arc. The register sequence is distilled from the
 * HW-verified CM7 path in USBHost_t36/ehci.cpp:233-300 -- the one measured
 * streaming at 1000 pkts/s on 2026-08-06 -- reduced to literals per the CM4
 * gate convention. Nothing here is a fresh RM derivation; the literals are
 * inherited from working silicon, and what is NEW is which core writes them.
 *
 * Block bases cross-checked against RM memory map rm_full.txt:2480-2483,
 * qemu2 hw/arm/fsl-imxrt1170.c:253, and cores/imxrt1176/imxrt1176.h:876/940/956.
 *
 * The probe deliberately does NOT run the async or periodic schedules -- no
 * descriptors, no DMA, no enumeration. It enables PCE (port change) only. That
 * keeps the OCRAM DMA trap (see the spec, section 7) out of 7.1 entirely.
 * Public domain (N. Newdigate). */
#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(a))

/* ---- MU B side (the CM4's), TR channel 0 -- HW-verified offsets, the
 * cm4_wire_test / cm4_sai_irq_probe pattern ---- */
#define MUB_BASE   0x40C4C000u
#define MUB_TR0    REG32(MUB_BASE + 0x00u)
#define MUB_SR     REG32(MUB_BASE + 0x20u)
#define MU_SR_TE0  (1u << 23)

static void mu_send(uint32_t v) {
    while (!(MUB_SR & MU_SR_TE0)) {}
    MUB_TR0 = v;
}

/* ---- clock gate: LPCG115 = kCLOCK_Usb (imxrt1176.h:876; 0x40CC6000 + 115*0x20) */
#define CCM_LPCG115_DIRECT  REG32(0x40CC6E60u)

/* ---- USBPHY2 @ 0x40438000 (RM rm_full.txt:2480; imxrt1176.h:956) ---- */
#define USBPHY2_BASE        0x40438000u
#define USBPHY2_PWD         REG32(USBPHY2_BASE + 0x00u)
#define USBPHY2_CTRL        REG32(USBPHY2_BASE + 0x30u)
#define USBPHY2_CTRL_SET    REG32(USBPHY2_BASE + 0x34u)
#define USBPHY2_CTRL_CLR    REG32(USBPHY2_BASE + 0x38u)
#define USBPHY2_PLL_SIC     REG32(USBPHY2_BASE + 0xA0u)
#define USBPHY2_PLL_SIC_SET REG32(USBPHY2_BASE + 0xA4u)
#define USBPHY2_PLL_SIC_CLR REG32(USBPHY2_BASE + 0xA8u)
#define PHY_CTRL_SFTRST     (1u << 31)
#define PHY_CTRL_CLKGATE    (1u << 30)
#define PLL_SIC_REG_ENABLE  (1u << 21)
#define PLL_SIC_POWER       (1u << 12)
#define PLL_SIC_EN_USB_CLKS (1u << 6)
#define PLL_SIC_LOCK        (1u << 31)
#define PLL_SIC_BYPASS      (1u << 16)
#define PLL_SIC_DIV_SEL_MSK (7u << 22)
#define PLL_SIC_DIV_SEL(n)  (((n) & 7u) << 22)

/* ---- USB OTG2 core @ 0x4042C000 (RM rm_full.txt:2483; imxrt1176.h:940) ---- */
#define USB2_BASE           0x4042C000u
#define USB2_USBCMD         REG32(USB2_BASE + 0x140u)
#define USB2_USBSTS         REG32(USB2_BASE + 0x144u)
#define USB2_USBINTR        REG32(USB2_BASE + 0x148u)
#define USB2_PORTSC1        REG32(USB2_BASE + 0x184u)
#define USB2_USBMODE        REG32(USB2_BASE + 0x1A8u)
#define USBCMD_RST          (1u << 1)
#define USBSTS_PCI          (1u << 2)
#define USBINTR_PCE         (1u << 2)
#define USBMODE_CM_HOST     (3u << 0)
#define USBMODE_SDIS        (1u << 4)
#define PORTSC_CCS          (1u << 0)
#define PORTSC_PP           (1u << 12)

/* IRQ 135 -> NVIC ISER4 (IRQs 128..159), bit 7. */
#define NVIC_ISER4          REG32(0xE000E110u)
#define NVIC_IPR135         (*(volatile uint8_t *)(0xE000E400u + 135u))

volatile uint32_t usb_irq_count = 0;
volatile uint32_t usb_last_sts  = 0;

/* Shared vector table (startup_cm4.S) also references these; unused here. */
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}
void SAI1_IRQHandler(void) {}

void USB_OTG2_IRQHandler(void) {
    uint32_t sts = USB2_USBSTS;
    USB2_USBSTS = sts;            /* W1C -- must clear or we re-enter forever */
    usb_last_sts = sts;
    usb_irq_count++;
}

/* Cycle-counted spins. The CM4 has no millis() in this world.
 *
 * Two functions, not one with a big argument: (CM4_HZ/1000000)*us overflows
 * uint32 above ~10.7 s of requested delay (400 * 10.7e6 > 2^32), and the
 * observation window below is 8 s -- close enough to that edge that a later
 * "let's make the window longer" edit would silently wrap and produce a spin
 * of nearly zero. spin_ms scales by 1000 fewer, so it cannot get there. */
#define CM4_HZ 400000000u
static void spin_us(uint32_t us) {
    volatile uint32_t n = (CM4_HZ / 1000000u) * us / 3u;   /* ~3 cycles/iter */
    while (n--) { }
}
static void spin_ms(uint32_t ms) {
    while (ms--) { spin_us(1000u); }
}

int main(void) {
    /* Stage 1: ungate the shared USB clock. RISK TRIGGER: clock/power gating
     * from the CM4. Precedent: LPCG104 (3.1) and LPCG102 (3.2) self-ungated
     * on silicon. */
    CCM_LPCG115_DIRECT = 1u;
    mu_send(0xC5B00001u);                        /* stage: clock gate written */
    mu_send(CCM_LPCG115_DIRECT);                 /* observation: gate readback */

    /* Stage 2: USBPHY2 480 MHz PLL. NOT an ANATOP AI-write handshake -- the
     * PHY has its own PLL behind plain MMIO, so Phase 6 finding 1 (the CM4
     * hangs on ai_write) is predicted NOT to apply. This stage is where that
     * prediction is tested; if the image hangs here, the fallback is the
     * Phase 6 pattern (CM7 pre-arms, CM4 skips). */
    USBPHY2_CTRL_CLR    = PHY_CTRL_SFTRST;
    USBPHY2_PLL_SIC_SET = PLL_SIC_REG_ENABLE;
    spin_us(25u);                                /* SDK: >= 15 us */
    USBPHY2_PLL_SIC_SET = PLL_SIC_POWER;
    USBPHY2_PLL_SIC     = (USBPHY2_PLL_SIC & ~PLL_SIC_DIV_SEL_MSK)
                          | PLL_SIC_DIV_SEL(3); /* 24 -> 480 MHz */
    USBPHY2_PLL_SIC_CLR = PLL_SIC_BYPASS;
    USBPHY2_PLL_SIC_SET = PLL_SIC_EN_USB_CLKS;
    USBPHY2_CTRL_CLR    = PHY_CTRL_CLKGATE;
    {
        uint32_t i;
        for (i = 0; i < 100u && !(USBPHY2_PLL_SIC & PLL_SIC_LOCK); i++) {
            spin_us(10u);
        }
    }
    USBPHY2_PWD = 0u;
    mu_send(0xC5B00002u);                        /* stage: PHY up */
    mu_send(USBPHY2_PLL_SIC);                    /* observation: LOCK is bit 31 */

    /* Stage 3: EHCI controller reset. */
    USB2_USBCMD |= USBCMD_RST;
    {
        uint32_t guard = 0;
        while ((USB2_USBCMD & USBCMD_RST) && ++guard < 1000000u) { }
        mu_send(0xC5B00003u);                    /* stage: reset done */
        mu_send(guard);                          /* observation: spin count */
    }

    /* Stage 4: host mode + stream disable, matching ehci.cpp:283. */
    USB2_USBMODE = USBMODE_CM_HOST | USBMODE_SDIS;
    mu_send(0xC5B00004u);
    mu_send(USB2_USBMODE);                       /* observation: CM(3) readback */

    /* Stage 5: port power, then run. PORTSC PP must be set before a device
     * can be detected at all. */
    USB2_PORTSC1 |= PORTSC_PP;
    USB2_USBCMD  |= 1u;                          /* RS: run */
    spin_ms(100u);                               /* attach/debounce */
    mu_send(0xC5B00005u);
    mu_send(USB2_PORTSC1);                       /* observation: CCS is bit 0 */

    /* Stage 6: interrupts. PCE only -- no schedules run, so UAI/UPI would
     * never fire and enabling them would be noise. */
    USB2_USBSTS  = USB2_USBSTS;                  /* W1C any stale status */
    USB2_USBINTR = USBINTR_PCE;
    NVIC_IPR135  = 128u;                         /* mid priority */
    __asm volatile ("cpsie i" ::: "memory");     /* PER-IMAGE: the copied
                                                  * startup_cm4.S leaves
                                                  * PRIMASK set. Phase 5 hit
                                                  * this; it false-FAILs on
                                                  * silicon and PASSES in QEMU. */
    NVIC_ISER4 = (1u << (135u - 128u));
    mu_send(0xC5B00006u);                        /* stage: IRQ armed */

    /* Observation window. On silicon the operator plugs and unplugs a device
     * on J47 during this; in QEMU the emulated usb-audio device is present
     * from reset, so the port-change interrupt fires on the initial attach. */
    spin_ms(8000u);                              /* ~8 s observation window */

    mu_send(usb_irq_count);                      /* THE ANSWER: >0 = CM4 took it */
    mu_send(usb_last_sts);                       /* which status bit(s) */
    mu_send(USB2_PORTSC1);                       /* CCS after the window */
    mu_send(0xD0DE0007u);                        /* done marker */
    for (;;) { __asm volatile ("wfi"); }
}
