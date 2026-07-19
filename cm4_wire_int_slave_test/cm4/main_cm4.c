/* Phase 4.2 scaffold stub: configures nothing, sends READY then parks.
 * Task 4 replaces this with the real interrupt-driven slave. */
#include <stdint.h>

#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define SR_TE(n)   (1u << (23 - (n)))

void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}
void LPI2C1_IRQHandler(void) {}
void LPI2C2_IRQHandler(void) {}

static void mu_send(unsigned ch, uint32_t v)
{
	while (!(MUB_SR & SR_TE(ch))) {
	}
	MUB_TR(ch) = v;
}

int main(void)
{
	mu_send(0, 0xCAFE0001u);   /* READY */
	for (;;) {}
}
