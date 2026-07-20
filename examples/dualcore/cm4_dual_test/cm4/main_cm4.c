/* Phase-2D capstone CM4 firmware: drive an independent GPIO ("blink") and act
 * as the IPC compute responder.
 *
 *   - configures GPIO5.12 (pad GPIO_DISP_B2_11, ALT5) as an output and drives
 *     it high, then reports its own GPIO5.DR bit over MU TR0 (0x1000);
 *   - MU_IRQHandler (external NVIC IRQ 118): for each command V on RR3, replies
 *     f(V) = V*3 + 7 on TR3.
 *
 * GPIO5 lives in shared system memory, reachable from the CM4 through its view;
 * driving DR + reading it back is the QEMU/silicon-consistent proof (PSR masks
 * output bits in the qemu2 model, so it is not used).  Public domain (N.N.).
 */
#include <stdint.h>

/* GPIO5 (the CM4's blink port) */
#define GPIO5_BASE     0x4013C000u
#define GPIO5_DR       (*(volatile uint32_t *)(GPIO5_BASE + 0x00u))
#define GPIO5_GDIR     (*(volatile uint32_t *)(GPIO5_BASE + 0x04u))
#define GPIO5_DR_SET   (*(volatile uint32_t *)(GPIO5_BASE + 0x84u))
#define GPIO5_DR_CLEAR (*(volatile uint32_t *)(GPIO5_BASE + 0x88u))
#define GPIO_BIT       12u
#define GPIO_MASK      (1u << GPIO_BIT)

/* IOMUX for pad GPIO_DISP_B2_11 -> GPIO5.12 (ALT5) */
#define IOMUX_MUX_DISP_B2_11 (*(volatile uint32_t *)0x400E8240u)
#define IOMUX_PAD_DISP_B2_11 (*(volatile uint32_t *)0x400E8484u)
#define IOMUX_ALT5           0x5u
#define IOMUX_PAD_DEFAULT    0x08u        /* DSE set; pull/keeper off */

/* MU B side (the CM4's) */
#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_RR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x10u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define MUB_CR     (*(volatile uint32_t *)(MUB_BASE + 0x24u))
#define SR_TE(n)   (1u << (23 - (n)))
#define CR_RIE(n)  (1u << (27 - (n)))
#define NVIC_ISER(n) (*(volatile uint32_t *)(0xE000E100u + 4u * (n)))
#define IRQ_MU     118u

static void mu_send(unsigned ch, uint32_t v)
{
    while (!(MUB_SR & SR_TE(ch))) {
    }
    MUB_TR(ch) = v;
}

/* SysTick unused here; the shared vector table still references the symbol. */
void SysTick_Handler(void)
{
}

/* External IRQ 118: IPC compute responder. */
void MU_IRQHandler(void)
{
    uint32_t v = MUB_RR(3);
    mu_send(3, v * 3u + 7u);
}

int main(void)
{
    /* GPIO5.12 as an output, driven high ("blink" ending high). */
    IOMUX_MUX_DISP_B2_11 = IOMUX_ALT5;
    IOMUX_PAD_DISP_B2_11 = IOMUX_PAD_DEFAULT;
    GPIO5_GDIR |= GPIO_MASK;
    for (int i = 0; i < 6; i++) {
        if (i & 1) {
            GPIO5_DR_CLEAR = GPIO_MASK;
        } else {
            GPIO5_DR_SET = GPIO_MASK;
        }
    }
    GPIO5_DR_SET = GPIO_MASK;

    /* Enable the MU IPC interrupt. */
    MUB_CR |= CR_RIE(3);
    NVIC_ISER(IRQ_MU >> 5) = 1u << (IRQ_MU & 31u);
    __asm__ volatile("cpsie i" ::: "memory");

    /* Report our GPIO drive (DR bit 12) to the CM7. */
    mu_send(0, GPIO5_DR & GPIO_MASK);

    /* Idle; the MU ISR services IPC requests. */
    for (;;) {
    }
}
