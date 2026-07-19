/* cm4_wire_dma_test CM4 firmware — Phase 4 eDMA_LPSR DMA-Wire, RED SCAFFOLD.
 *
 * This is the Task-3 RED stub: the CM4 boots, sends only the READY token
 * (0xCAFE0001) over the MU, and parks.  It emits NONE of the DMA tokens the CM7
 * reporter expects (croot, rdv, dmairq, err, done), so the gate ends
 * WIRE_DMA_CM4=FAIL with those tokens TIMEOUT — that RED is the Task-3 pass
 * criterion.
 *
 * Task 4 (GREEN) fills this in: ungate the eDMA_LPSR clock, self-config LPI2C5
 * (shared lpi2c1176_begin), set up an eDMA_LPSR RX channel (TCD at 0x40C15000)
 * triggered by LPI2C5's DMA request (DMAMUX1/LPSR source 52) to DMA-read the
 * WM8962 device ID into an OCRAM2 .dmabuf buffer, with CSR.INTMAJOR so the
 * eDMA_LPSR channel-0 completion IRQ fires on the CM4's OWN NVIC (CM4 IRQ 0,
 * native — the whole point), the ISR (DMA_CH0_IRQHandler) setting dmairq.
 *
 * The vector table (startup_cm4.S) already routes eDMA_LPSR ch0 -> vector index
 * 16 (DMA_CH0_IRQHandler) and MU -> index 134 (MU_IRQHandler); the empty stubs
 * below satisfy those symbol references until Task 4 gives them bodies.
 * Public-domain scaffolding (N. Newdigate). */
#include <stdint.h>

/* ---- MU B side (the CM4's) ---- */
#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define SR_TE(n)   (1u << (23 - (n)))

/* eDMA_LPSR ch0 completion, SysTick, and MU IRQs are referenced by the vector
 * table but unused by this RED scaffold — empty stubs keep the linker happy.
 * Task 4 gives DMA_CH0_IRQHandler the real eDMA_LPSR completion body. */
void DMA_CH0_IRQHandler(void) {}
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
    mu_send(0, 0xCAFE0001u);   /* READY — the only token the RED scaffold emits */
    for (;;) {}
}
