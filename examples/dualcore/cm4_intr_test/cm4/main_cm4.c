/* Phase-2C CM4 firmware: stands up the CM4's own interrupt + timing units and
 * reports over the MU B side (MUB @ 0x40C4C000).
 *
 *   TR0 = DWT proof      0xD00D0001 if the DWT CYCCNT advanced, else ...0000
 *   TR1 = SysTick count  number of SysTick exceptions taken (characterisation)
 *   TR3 = MU-IRQ echo    RR3+1, sent from the MU interrupt handler (NVIC 118)
 *
 * DWT CYCCNT is the silicon-safe timing base (the CM7 core uses it because the
 * RT1176 SysTick tick ISR is unreliable — delay.c); SysTick here is measured,
 * not depended on.  Registers are the standard ARMv7-M SCS (same on CM4/CM7).
 * Public domain (author: Nicholas Newdigate).
 */
#include <stdint.h>

/* Cortex-M system control space */
#define SYST_CSR   (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR   (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR   (*(volatile uint32_t *)0xE000E018u)
#define DEMCR      (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL   (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define NVIC_ISER(n) (*(volatile uint32_t *)(0xE000E100u + 4u * (n)))
#define DEMCR_TRCENA   (1u << 24)
#define DWT_CYCCNTENA  (1u << 0)
#define SYST_CSR_ENABLE_TICKINT_CORECLK 0x7u

/* MU B side (the CM4's) */
#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_RR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x10u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define MUB_CR     (*(volatile uint32_t *)(MUB_BASE + 0x24u))
#define SR_TE(n)   (1u << (23 - (n)))
#define CR_RIE(n)  (1u << (27 - (n)))
#define IRQ_MU     118u

volatile uint32_t g_systick_count = 0;

static void mu_send(unsigned ch, uint32_t v)
{
    while (!(MUB_SR & SR_TE(ch))) {
    }
    MUB_TR(ch) = v;
}

/* Exception 15: increments a counter each SysTick reload. */
void SysTick_Handler(void)
{
    g_systick_count++;
}

/* External IRQ 118: the CM7 wrote MUA.TR3 -> our RR3 is full.  Reading RR3
 * clears it (de-asserting the interrupt); echo value+1 back on TR3. */
void MU_IRQHandler(void)
{
    uint32_t v = MUB_RR(3);
    mu_send(3, v + 1u);
}

int main(void)
{
    uint32_t t0, elapsed;

    /* Silicon-safe timing base: free-running DWT cycle counter. */
    DEMCR |= DEMCR_TRCENA;
    DWT_CTRL |= DWT_CYCCNTENA;

    /* SysTick: periodic, exception enabled, core clock (measured, not trusted). */
    SYST_RVR = 1000u - 1u;
    SYST_CVR = 0u;
    SYST_CSR = SYST_CSR_ENABLE_TICKINT_CORECLK;

    /* Enable the MU receive interrupt (channel 3) into the CM4 NVIC. */
    MUB_CR |= CR_RIE(3);
    NVIC_ISER(IRQ_MU >> 5) = 1u << (IRQ_MU & 31u);
    __asm__ volatile("cpsie i" ::: "memory");

    /* DWT proof: measure a bounded spin; report whether the counter moved. */
    t0 = DWT_CYCCNT;
    for (volatile uint32_t i = 0; i < 20000u; i++) {
    }
    elapsed = DWT_CYCCNT - t0;
    mu_send(0, (elapsed != 0u) ? 0xD00D0001u : 0xD00D0000u);

    /* Give SysTick time to fire, then report the exception count. */
    for (volatile uint32_t i = 0; i < 400000u; i++) {
    }
    mu_send(1, g_systick_count);

    /* Idle; the MU ISR services CM7 messages. */
    for (;;) {
    }
}
