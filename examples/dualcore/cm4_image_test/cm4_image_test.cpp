/*
 * cm4_image_test - QEMU/HW gate for Phase-2A: a REAL COMPILED Cortex-M4 image.
 *
 * Runs on the CM7.  Boots a CM4 image that was built from actual C + a real
 * startup/linker (cm4/), staged into the CM4 TCM backdoor by the Phase-1
 * Multicore.begin().  Unlike the Phase-1 hand-asm leaf blob, this CM4 image
 * runs a real reset handler: it copies .data (ITCM LMA -> DTCM VMA), zeroes
 * .bss, and uses a DTCM stack.  It reports three "canaries" over the MU that
 * only read correct if that startup machinery worked on silicon:
 *   data  = 0xDA7A0001  a .data global   -> proves the LMA->VMA copy
 *   bss   = 0x00000B55  a .bss  global+0xB55 -> proves .bss was zeroed
 *   stack = 0x0000008C  sum of a stack array (0+1+4+9+16+25+36+49=140) -> stack
 * then echoes RR3+1 to prove the real image keeps running and the MU still
 * works.  A restart re-runs the whole startup (data2 canary again).
 *
 * One "token=HEXVALUE" line per observation over Serial1 (LPUART1) so QEMU and
 * EVKB transcripts diff directly.
 */
#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"
#include "Multicore.h"
#include "MessagingUnit.h"
#include "cm4_image.h"     /* generated: const uint32_t cm4_image[] (objcopy of the CM4 ELF) */

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

static bool wait_recv(uint8_t ch, uint32_t *out)
{
    for (uint32_t n = WAIT_LONG; n; n--) {
        if (MU.tryReceive(ch, out)) {
            return true;
        }
    }
    return false;
}

static void report_canaries(const char *dk, const char *bk, const char *sk)
{
    uint32_t v;
    if (wait_recv(0, &v)) { phex(dk, v); } else { ptimeout(dk); }
    if (wait_recv(1, &v)) { phex(bk, v); } else { ptimeout(bk); }
    if (wait_recv(2, &v)) { phex(sk, v); } else { ptimeout(sk); }
}

void setup()
{
    uint32_t v;

    Serial1.begin(115200);
    Serial1.println("CM4IMG-GATE v1");

    MU.begin();

    bool ok = Multicore.begin(cm4_image, sizeof(cm4_image));
    phex("boot", ok ? 1u : 0u);
    phex("run", Multicore.running() ? 1u : 0u);

    /* the real-image startup canaries */
    report_canaries("data", "bss", "stack");

    /* prove the image keeps running + MU round-trips */
    MU.send(3, 0x12345678u);
    if (wait_recv(3, &v)) { phex("echo", v); } else { ptimeout("echo"); }

    /* restart re-runs the whole CM4 startup from the staged image */
    Multicore.restart();
    report_canaries("data2", "bss2", "stack2");

    Serial1.println("CM4IMG-DONE");
}

void loop()
{
}
