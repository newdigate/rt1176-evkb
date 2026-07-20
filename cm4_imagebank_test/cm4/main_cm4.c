/* cm4_hotswap_test CM4 image (D7 hot-swap): send a ready handshake + a build-
 * time identity constant over the MU, then PARK IN WFI. Two images are built
 * from this one source with different HS_IDENTITY (A=0xA1A1A1A1, B=0xB2B2B2B2);
 * the CM7 boots A, then begin(B) re-pulses SRC SW_RESET to reboot the running
 * CM4 into B. WFI (not a spin) so the CM4 isn't fetching its own code while the
 * CM7 overwrites the 0x20200000 backdoor with B just before the reset lands.
 * Public domain (N. Newdigate). */
#include <stdint.h>
#ifndef HS_IDENTITY
#define HS_IDENTITY 0xDEADDEADu
#endif

#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define SR_TE(n)   (1u << (23 - (n)))

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
    mu_send(0, 0xCAFE0001u);   /* ready handshake */
    mu_send(0, HS_IDENTITY);   /* which image am I */
    for (;;) {
        __asm volatile ("wfi");
    }
}
