/*
 * cm4_wire_int_slave_test — Phase 4.2: the CM4 runs an INTERRUPT-DRIVEN I2C
 * slave @0x42 (distilled from the HW-verified TwoWire slave via the shared
 * lpi2c1176 core), servicing its LPI2C IRQ on its own NVIC. World-split
 * instance (spec §4.2): QEMU = LPI2C2 persona (IRQ 33, bridged onto LPI2C1's
 * bus; this CM7 build is the polled master); HW = LPI2C1 (IRQ 32, Arduino
 * header A4=SDA/A5=SCL; an EXTERNAL I2C master drives the exchange and its
 * own serial output is the HW-side oracle for the response byte).
 *
 * Protocol constants (both worlds): master writes {A5 5A C3}, then reads 1
 * byte; the slave responds 3C.
 *
 * Tokens (MU ch0, fixed order after the READY handshake):
 *   ready  = CAFE0001   CM4 slave configured + enabled
 *   irqcnt = >0         CM4 serviced its slave IRQ on its own NVIC
 *   b0/b1/b2 = A5/5A/C3 bytes the master wrote, captured by the CM4 ISR
 *   resp   = 0000003C   byte the ISR loaded into STDR when TDF fired
 *   err    = 00000000   0 OK / 4 = QEMU wait-guard expired (stalled exchange)
 *   done   = 00000001
 * HW build: WIRE_INT_SLAVE_CM4=PASS requires irqcnt>0, b0/b1/b2, resp=3C,
 * err=0, done=1 (plus the external master's own rd=3C serial oracle).
 * QEMU build: write-path only — irqcnt>0, b0/b1/b2, err=0, done=1; wr/mrd/
 * resp are printed but UNASSERTED. DOCUMENTED MODEL LIMIT (Phase 4.2
 * contingency, 2026-07-19): qemu2's imxrt_lpi2c serves the master's read
 * synchronously on the CM7 vCPU (empty slave_tx -> 0xFF; TXDSTALL clock-
 * stretching is not modeled across vCPUs), so whether the CM4's TDF ISR
 * refills STDR before the master is served races vCPU thread scheduling.
 * On silicon TXDSTALL holds SCL until STDR is written — no race exists.
 */
#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"
#include "Multicore.h"
#include "MessagingUnit.h"
#include "lpi2c1176.h"
#include "imxrt1176.h"

#ifdef WIRE_SLAVE_WORLD_HW
#include "cm4_wire_int_slave_h.h"
#define CM4_IMG cm4_wire_int_slave_h
#else
#include "cm4_wire_int_slave_q.h"
#define CM4_IMG cm4_wire_int_slave_q
#endif

#define SLAVE_ADDR 0x42u
#define WAIT_LONG  3000000u

static void phex(const char *k, uint32_t v)
{
    Serial1.print(k); Serial1.print('=');
    for (int i = 28; i >= 0; i -= 4)
        Serial1.print("0123456789ABCDEF"[(v >> i) & 0xF]);
    Serial1.println();
}
static void ptimeout(const char *k) { Serial1.print(k); Serial1.println("=TIMEOUT"); }

static bool wait_recv_bounded(uint8_t ch, uint32_t *out)
{
    for (uint32_t n = WAIT_LONG; n; n--)
        if (MU.tryReceive(ch, out)) return true;
    return false;
}

#ifndef WIRE_SLAVE_WORLD_HW
/* QEMU-world master: LPI2C1 via the shared core (values verbatim from the
 * HW-verified Wire lpi2c1_hardware descriptor). */
#define LPI2C1 ((lpi2c1176_regs_t *)0x40104000u)
static const lpi2c1176_hw_t lpi2c1_hw = {
    &CCM_LPCG98_DIRECT, &CCM_CLOCK_ROOT37_CONTROL, 0u,
    &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_08, 0x11u, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_08,
    &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_09, 0x11u, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_09,
    &IOMUXC_LPI2C1_SCL_SELECT_INPUT, 0u,
    &IOMUXC_LPI2C1_SDA_SELECT_INPUT, 0u,
    0x0000001Eu,
};
#endif

void setup()
{
    Serial1.begin(115200);
    Serial1.println("CM4WIRESLV-GATE v1");

    MU.begin();
    Multicore.begin(CM4_IMG, sizeof(CM4_IMG));

    uint32_t ready = 0;
    bool ok = true;
    if (wait_recv_bounded(0, &ready)) phex("ready", ready);
    else { ptimeout("ready"); ready = 0; ok = false; }

    uint32_t wr = 0xFFFFFFFFu, mrd = 0xFFFFFFFFu;
#ifndef WIRE_SLAVE_WORLD_HW
    if (ok) {
        lpi2c1176_begin(LPI2C1, &lpi2c1_hw, 100000u);
        static const uint8_t tx[3] = { 0xA5u, 0x5Au, 0xC3u };
        wr = lpi2c1176_master_write(LPI2C1, SLAVE_ADDR, tx, 3, 1);
        uint8_t rb = 0;
        uint32_t n = lpi2c1176_master_read(LPI2C1, SLAVE_ADDR, &rb, 1, 1);
        mrd = n ? rb : 0xFFFFFFFFu;
    }
    phex("wr", wr);
    phex("mrd", mrd);
#else
    Serial1.println("wr=EXTERN");
    Serial1.println("mrd=EXTERN");
    Serial1.println("EXT-MASTER: run ext_master now (write A5 5A C3 to 0x42, read 1 byte)");
#endif

    static const char *labels[7] = { "irqcnt", "b0", "b1", "b2", "resp", "err", "done" };
    uint32_t v[7];
    for (int i = 0; i < 7; i++) {
#ifdef WIRE_SLAVE_WORLD_HW
        while (!MU.tryReceive(0, &v[i])) {}     /* human-paced external master */
        phex(labels[i], v[i]);
#else
        if (wait_recv_bounded(0, &v[i])) phex(labels[i], v[i]);
        else { ptimeout(labels[i]); v[i] = 0xFFFFFFFFu; ok = false; }
#endif
    }

    bool pass = ok
        && ready == 0xCAFE0001u
        && v[0] != 0x0u          /* irqcnt: CM4 took its slave IRQ */
        && v[1] == 0xA5u && v[2] == 0x5Au && v[3] == 0xC3u
        && v[5] == 0x0u          /* err */
        && v[6] == 0x1u;         /* done */
#ifdef WIRE_SLAVE_WORLD_HW
    pass = pass && v[4] == 0x3Cu;   /* resp loaded into STDR — HW-asserted */
#else
    /* wr/mrd/resp unasserted in QEMU — documented model limit (see header):
     * the master's read-data byte races CM4 vCPU scheduling in the model. */
#endif
    Serial1.println(pass ? "WIRE_INT_SLAVE_CM4=PASS" : "WIRE_INT_SLAVE_CM4=FAIL");
    Serial1.println("CM4WIRESLV-DONE");
}

void loop() {}
