/* cm4_wire_int_master_test CM4 firmware (Phase 4.1 scaffold — Task 3, RED):
 * the CM4 SELF-CONFIGURES LPI2C5 exactly as in cm4_wire_test (Phase 3.2/3.3)
 * via the shared lpi2c1176_begin() core, but does NOT yet run an
 * interrupt-driven transaction — Tasks 4-5 add the LPI2C5 ISR on the CM4
 * NVIC (the first non-MU peripheral IRQ routed there, via the qemu2
 * split-IRQ). This stub streams a fixed 6-token MU sequence with
 * irqcnt=0 (no IRQ serviced) and rdv=0 (no read performed); the CM7
 * reporter's irqcnt>0 guard turns that into an intentional
 * WIRE_INT_MASTER_CM4=FAIL — this RED state is the Task-3 deliverable,
 * proving the gate detects a missing interrupt path before Tasks 4-5
 * implement it.
 *
 * Tokens streamed to the CM7 over the MU (channel 0, fixed order):
 *   irqcnt = 00000000   stub: CM4 LPI2C5 ISR not wired yet (RED marker)
 *   mcr    = 00000001   LPI2C MCR.MEN — the CM4 enabled the master block
 *   lpcg   = ........   CCM_LPCG102 readback (informative)
 *   croot  = ........   CCM_CLOCK_ROOT41 readback (informative)
 *   rdv    = 00000000   stub: no transaction performed yet
 *   done   = 00000001   CM4 sequence completed
 *
 * The self-config sequence (LPI2C5 + its CCM/LPSR-IOMUXC instance table)
 * and the MU scaffolding below are UNCHANGED from cm4_wire_test: the shared
 * C core lpi2c1176.c from newdigate/Wire (MIT, N. Newdigate) — the same
 * code the CM7 TwoWire master path runs. WM8962_ADDR/ABSENT_ADDR are kept
 * (unused by this stub) for the interrupt-driven transaction Tasks 4-5 add.
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
#define ABSENT_ADDR  0x2Au   /* clear of WM8962 0x1A + FXLS8974 accel 0x18 */

/* ---- MU B side (the CM4's) ---- */
#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define SR_TE(n)   (1u << (23 - (n)))

/* The shared vector table (startup_cm4.S) references these C symbols. The
 * stub needs neither yet (no LPI2C5 IRQ wired), but the table entries must
 * resolve. Tasks 4-5 replace MU_IRQHandler's body with the real LPI2C5 ISR
 * dispatch (or add a dedicated handler slotted at vector index 52). */
void SysTick_Handler(void) {}
void LPI2C5_IRQHandler(void) {}   /* Task 4 placeholder; real ISR in Task 5 */
void MU_IRQHandler(void) {}

static void mu_send(unsigned ch, uint32_t v)
{
    while (!(MUB_SR & SR_TE(ch))) {
    }
    MUB_TR(ch) = v;
}

int main(void)
{
    lpi2c1176_begin(LPI2C5, &lpi2c5_hw, 100000u);
    uint32_t mcr = LPI2C5->MCR & LPI2C1176_MCR_MEN;
    mu_send(0, 0u);                 /* irqcnt (stub) */
    mu_send(0, mcr);
    mu_send(0, *lpi2c5_hw.lpcg);
    mu_send(0, *lpi2c5_hw.clock_root);
    mu_send(0, 0u);                 /* rdv (stub) */
    mu_send(0, 1u);                 /* done */
    for (;;) {}
}
