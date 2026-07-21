/* mu_report.h — the CM4 image's MU send helper, factored from
 * cm4_sai_irq_probe/cm4/main_cm4.c (the cm4_wire_test pattern).
 *
 * MU B side (the CM4's), TR channel 0. HW-verified offsets: TR0 at +0x00,
 * SR at +0x20, TE0 = SR bit 23 (NOT the plan-draft +0x20/+0x60 layout).
 * C header, usable from both C and C++ image sources.
 * Public domain (N. Newdigate). */
#pragma once
#include <stdint.h>

#define MU_REPORT_REG32(a) (*(volatile uint32_t *)(a))

#define MUB_BASE   0x40C4C000u
#define MUB_TR0    MU_REPORT_REG32(MUB_BASE + 0x00u)
#define MUB_SR     MU_REPORT_REG32(MUB_BASE + 0x20u)
#define MU_SR_TE0  (1u << 23)

static inline void mu_send(uint32_t v) {
    while (!(MUB_SR & MU_SR_TE0)) {}
    MUB_TR0 = v;
}
