/* cm4_wire_int_slave_test CM4 firmware (Phase 4.2): the CM4 SELF-CONFIGURES
 * an LPI2C slave @0x42 via the shared lpi2c1176 core (the same
 * slave_config/slave_enable sequence the HW-verified CM7 TwoWire slave runs),
 * NVIC-enables the instance IRQ on its OWN NVIC, and services the exchange
 * entirely in its ISR: captures the master's write bytes (RDF), serves the
 * response byte (TDF, clock-stretched by TXDSTALL), counts STOPs (SDF).
 *
 * WORLD-SPLIT INSTANCE (spec §4.2): no LPI2C is both QEMU-bus-bridged and
 * EVKB-header-accessible, so the build binds the one slave implementation to
 *   QEMU: LPI2C2 persona 0x40108000, IRQ 33 (bridged onto LPI2C1's bus;
 *         the CM7 gate build is the polled master)
 *   HW:   LPI2C1 0x40104000, IRQ 32 (Arduino header A4=AD_09=SDA /
 *         A5=AD_08=SCL; an EXTERNAL master drives the exchange)
 * ISR logic distilled from TwoWire::handle_slave_isr (WireIMXRT1176.cpp:107,
 * MIT, HW-verified) — keep in sync. Deviation: rx count is NOT reset on AVF
 * (the read-phase address match must not discard the recorded write bytes).
 *
 * Tokens (MU ch0): READY, then {irqcnt, b0, b1, b2, resp, err, done}.
 *
 * DOCUMENTED MODEL LIMIT (Phase 4.2 contingency, 2026-07-19): in QEMU the
 * master's read-data byte is unasserted — qemu2's imxrt_lpi2c serves the
 * CM7 master's read synchronously on the CM7 vCPU (empty slave_tx -> 0xFF)
 * without modeling the TXDSTALL clock-stretch, so whether this CM4 ISR
 * refills STDR first races vCPU scheduling. The firmware below is correct
 * for silicon (TXDSTALL+CLKHOLD hold SCL until STDR is written); the
 * response byte is HW-verified by the EVKB probe's external master.
 *
 * Public-domain scaffolding (N. Newdigate); shared-core register logic MIT. */
#include <stdint.h>
#include "lpi2c1176.h"

#ifdef WIRE_SLAVE_WORLD_HW
#define SLAVE_LPI2C ((lpi2c1176_regs_t *)0x40104000u)   /* LPI2C1 */
#define SLAVE_IRQ   32u
static const lpi2c1176_hw_t slave_hw = {                /* verbatim Wire lpi2c1_hardware */
	.lpcg = (volatile uint32_t *)0x40CC6C40u,           /* CCM_LPCG98_DIRECT */
	.clock_root = (volatile uint32_t *)0x40CC1280u,     /* CCM_CLOCK_ROOT37_CONTROL */
	.clock_root_val = 0u,
	.scl_mux = (volatile uint32_t *)0x400E812Cu, .scl_mux_val = 0x11u, /* AD_08 ALT1|SION */
	.scl_pad = (volatile uint32_t *)0x400E8370u,
	.sda_mux = (volatile uint32_t *)0x400E8130u, .sda_mux_val = 0x11u, /* AD_09 ALT1|SION */
	.sda_pad = (volatile uint32_t *)0x400E8374u,
	.scl_select = (volatile uint32_t *)0x400E85ACu, .scl_select_val = 0u,
	.sda_select = (volatile uint32_t *)0x400E85B0u, .sda_select_val = 0u,
	.pad_ctl_val = 0x0000001Eu,                         /* ODE|DSE|PUE|PUS */
};
#else
#define SLAVE_LPI2C ((lpi2c1176_regs_t *)0x40108000u)   /* LPI2C2 persona */
#define SLAVE_IRQ   33u
static const lpi2c1176_hw_t slave_hw = {                /* verbatim Wire lpi2c2_hardware:
	pin refs bind to LPI2C1's IOMUXC regs (inert in QEMU) */
	.lpcg = (volatile uint32_t *)0x40CC6C60u,           /* CCM_LPCG99_DIRECT */
	.clock_root = (volatile uint32_t *)0x40CC1300u,     /* CCM_CLOCK_ROOT38_CONTROL */
	.clock_root_val = 0u,
	.scl_mux = (volatile uint32_t *)0x400E812Cu, .scl_mux_val = 0x11u,
	.scl_pad = (volatile uint32_t *)0x400E8370u,
	.sda_mux = (volatile uint32_t *)0x400E8130u, .sda_mux_val = 0x11u,
	.sda_pad = (volatile uint32_t *)0x400E8374u,
	.scl_select = (volatile uint32_t *)0x400E85ACu, .scl_select_val = 0u,
	.sda_select = (volatile uint32_t *)0x400E85B0u, .sda_select_val = 0u,
	.pad_ctl_val = 0x0000001Eu,
};
#endif

#define SLAVE_ADDR  0x42u
#define RESP_BYTE   0x3Cu
#define NVIC_ISER1  (*(volatile uint32_t *)0xE000E104u)  /* IRQ 32..63 */
#define WAIT_GUARD  50000000u   /* QEMU-world bounded exchange wait */

/* Slave exchange state, shared ISR<->main. */
static volatile struct {
	uint32_t irqcnt, rx_n, tx_served, stops, resp;
	uint8_t  b[3];
} S;

/* ---- MU B side (the CM4's) ---- */
#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define SR_TE(n)   (1u << (23 - (n)))

static void mu_send(unsigned ch, uint32_t v)
{
	while (!(MUB_SR & SR_TE(ch))) {
	}
	MUB_TR(ch) = v;
}

/* Distilled TwoWire::handle_slave_isr (keep in sync — see file header). */
static void slave_isr_body(void)
{
	lpi2c1176_regs_t *p = SLAVE_LPI2C;
	uint32_t ssr = p->SSR;
	S.irqcnt++;

	if (ssr & (LPI2C1176_SSR_BEF | LPI2C1176_SSR_FEF))   /* latched error -> W1C, FIFO recovers */
		p->SSR = LPI2C1176_SSR_BEF | LPI2C1176_SSR_FEF;
	if (ssr & LPI2C1176_SSR_AVF) {                       /* address match: read SASR clears AVF */
		volatile uint32_t sasr = p->SASR; (void)sasr;    /* rx count NOT reset (see header) */
	}
	if (ssr & LPI2C1176_SSR_RDF) {                       /* master wrote a byte */
		uint8_t d = (uint8_t)p->SRDR;
		if (S.rx_n < 3u) S.b[S.rx_n] = d;
		S.rx_n++;
	}
	if (ssr & LPI2C1176_SSR_TDF) {                       /* master wants a byte (TXDSTALL holds SCL) */
		p->STDR = RESP_BYTE;
		S.resp = RESP_BYTE;
		S.tx_served++;
	}
	if (ssr & LPI2C1176_SSR_SDF) {                       /* STOP -> one transfer done */
		p->SSR = LPI2C1176_SSR_SDF;
		S.stops++;
	}
}

void LPI2C1_IRQHandler(void) { slave_isr_body(); }
void LPI2C2_IRQHandler(void) { slave_isr_body(); }
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}

static int exchange_complete(void)
{
	return S.stops >= 2u && S.rx_n >= 3u && S.tx_served >= 1u;
}

int main(void)
{
	lpi2c1176_slave_config(SLAVE_LPI2C, &slave_hw, SLAVE_ADDR);
	NVIC_ISER1 = (1u << (SLAVE_IRQ - 32u));
	__asm volatile ("cpsie i" ::: "memory");   /* reset handler left IRQs masked */
	lpi2c1176_slave_enable(SLAVE_LPI2C);       /* SEN last — same order as TwoWire */

	mu_send(0, 0xCAFE0001u);                   /* READY: master may transact now */

	uint32_t err = 0u;
#ifdef WIRE_SLAVE_WORLD_HW
	while (!exchange_complete()) {             /* human-paced external master */
	}
#else
	err = 4u;                                  /* stalled-exchange sentinel (4.1 lesson) */
	for (uint32_t g = 0; g < WAIT_GUARD; g++)
		if (exchange_complete()) { err = 0u; break; }
#endif

	mu_send(0, S.irqcnt);
	mu_send(0, S.b[0]);
	mu_send(0, S.b[1]);
	mu_send(0, S.b[2]);
	mu_send(0, S.tx_served ? S.resp : 0xDEADDEADu);
	mu_send(0, err);
	mu_send(0, 1u);                            /* done */
	for (;;) {}
}
