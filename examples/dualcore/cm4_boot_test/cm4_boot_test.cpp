/*
 * cm4_boot_test - QEMU/HW gate for the Phase-1 dual-core library
 * (cores/imxrt1176: Multicore.h + MessagingUnit.h).
 *
 * Runs on the CM7.  Boots an embedded CM4 blob via Multicore.begin(), then
 * exercises the MU: mailbox receive, the GIR/GIP doorbell handshake (incl.
 * GIR auto-clear on the CM4's ack), an interrupt-driven receive callback on
 * NVIC 118, and Multicore.restart().  Prints one "token=HEXVALUE" line per
 * observation over Serial1 (LPUART1) so a QEMU transcript and an EVKB
 * transcript diff directly -- the same discipline as dualcore_mu_test, but
 * driven entirely through the new library API instead of raw registers.
 *
 * Expected tokens (deterministic unless noted):
 *   boot   1          Multicore.begin() reports the CM4 left reset
 *   run    1          Multicore.running()
 *   hello  C0FFEE42   first CM4 mailbox message (MU.receive ch0)
 *   gir    0 or 1     GIR0 still pending right after trigger() (timing-sensitive)
 *   dbell  D00DFEED   CM4 doorbell ack (MU.receive ch1)
 *   giraft 0          GIR0 auto-cleared after the CM4 acked GIP0
 *   echo   12345679   CM4 echo+1 delivered via the MU receive interrupt
 *   irqcnt 1          number of MU receive IRQs taken
 *   run2   1          CM4 still running before restart
 *   hello2 C0FFEE42   CM4 hello after Multicore.restart()
 */
#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"
#include "Multicore.h"
#include "MessagingUnit.h"
#include "cm4_blob.h"

/* Bounded MMIO poll: far beyond the CM4's microsecond response on silicon and
 * a couple of seconds in TCG. */
#define WAIT_LONG 3000000u

static void phex(const char *k, uint32_t v)
{
    Serial1.print(k);
    Serial1.print('=');
    for (int i = 28; i >= 0; i -= 4) {
        Serial1.print("0123456789ABCDEF"[(v >> i) & 0xF]);
    }
    Serial1.println();
}

static void ptimeout(const char *k)
{
    Serial1.print(k);
    Serial1.println("=TIMEOUT");
}

/* Bounded wait for a mailbox message on a channel. */
static bool wait_recv(uint8_t ch, uint32_t *out)
{
    for (uint32_t n = WAIT_LONG; n; n--) {
        if (MU.tryReceive(ch, out)) {
            return true;
        }
    }
    return false;
}

static volatile uint32_t echo_val;
static volatile uint32_t echo_count;

static void echo_isr(uint8_t ch, uint32_t msg)
{
    (void)ch;
    echo_val = msg;
    echo_count++;
}

void setup()
{
    uint32_t v;

    Serial1.begin(115200);
    Serial1.println("CM4BOOT-GATE v1");

    MU.begin();

    /* --- boot the CM4 --- */
    bool ok = Multicore.begin(cm4_blob, sizeof(cm4_blob));
    phex("boot", ok ? 1u : 0u);
    phex("run", Multicore.running() ? 1u : 0u);

    /* --- mailbox: first CM4 message --- */
    if (wait_recv(0, &v)) {
        phex("hello", v);
    } else {
        ptimeout("hello");
    }

    /* --- doorbell: trigger GIR0 -> CM4 GIP0 -> ack -> GIR auto-clear --- */
    MU.trigger(0);
    phex("gir", MU.triggerPending(0) ? 1u : 0u);   /* timing-sensitive */
    if (wait_recv(1, &v)) {
        phex("dbell", v);
    } else {
        ptimeout("dbell");
    }
    phex("giraft", MU.triggerPending(0) ? 1u : 0u);

    /* --- interrupt-driven receive on channel 3 (NVIC 118) --- */
    echo_val = 0;
    echo_count = 0;
    MU.onReceive(3, echo_isr);
    MU.send(3, 0x12345678u);
    for (uint32_t n = WAIT_LONG; n && !echo_count; n--) {
    }
    if (echo_count) {
        phex("echo", echo_val);
    } else {
        ptimeout("echo");
    }
    phex("irqcnt", echo_count);
    MU.onReceive(3, 0);   /* detach */

    /* --- restart the CM4: reboot from the same image --- */
    phex("run2", Multicore.running() ? 1u : 0u);
    Multicore.restart();
    if (wait_recv(0, &v)) {
        phex("hello2", v);
    } else {
        ptimeout("hello2");
    }

    Serial1.println("CM4BOOT-DONE");
}

void loop()
{
}
