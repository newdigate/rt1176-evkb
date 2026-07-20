/*
 * cm4_hotswap_test (D7): the CM7 boots the CM4 with image A, then hot-swaps it
 * to image B via a second Multicore.begin() (which re-pulses SRC SW_RESET on
 * the running CM4). Each image streams a ready handshake + its identity over
 * the MU. Seeing A's identity THEN B's identity proves the running CM4 was
 * rebooted into a different program at runtime. Resolves Phase-1 D7.
 * Tokens: readyA=CAFE0001, idA=A1A1A1A1, then readyB=CAFE0001, idB=B2B2B2B2.
 * HOTSWAP=PASS requires idA==A1A1A1A1 && idB==B2B2B2B2.
 */
#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"
#include "Multicore.h"
#include "MessagingUnit.h"
#include "cm4_hs_a.h"
#include "cm4_hs_b.h"

#define WAIT_LONG 3000000u

static void phex(const char *k, uint32_t v)
{
    Serial1.print(k); Serial1.print('=');
    for (int i = 28; i >= 0; i -= 4)
        Serial1.print("0123456789ABCDEF"[(v >> i) & 0xF]);
    Serial1.println();
}
static void ptimeout(const char *k) { Serial1.print(k); Serial1.println("=TIMEOUT"); }
static bool wait_recv(uint8_t ch, uint32_t *out)
{
    for (uint32_t n = WAIT_LONG; n; n--)
        if (MU.tryReceive(ch, out)) return true;
    return false;
}

void setup()
{
    Serial1.begin(115200);
    Serial1.println("CM4HOTSWAP-GATE v1");
    MU.begin();

    bool ok = true;
    uint32_t ra = 0, idA = 0, rb = 0, idB = 0;

    /* Boot image A. */
    Multicore.begin(cm4_hs_a, sizeof(cm4_hs_a));
    if (wait_recv(0, &ra)) phex("readyA", ra); else { ptimeout("readyA"); ok = false; }
    if (wait_recv(0, &idA)) phex("idA", idA); else { ptimeout("idA"); ok = false; }

    /* Hot-swap to image B: a second begin() re-pulses SW_RESET on the running CM4. */
    Multicore.begin(cm4_hs_b, sizeof(cm4_hs_b));
    if (wait_recv(0, &rb)) phex("readyB", rb); else { ptimeout("readyB"); ok = false; }
    if (wait_recv(0, &idB)) phex("idB", idB); else { ptimeout("idB"); ok = false; }

    bool pass = ok && idA == 0xA1A1A1A1u && idB == 0xB2B2B2B2u;
    Serial1.println(pass ? "HOTSWAP=PASS" : "HOTSWAP=FAIL");
    Serial1.println("CM4HOTSWAP-DONE");
}
void loop() {}
