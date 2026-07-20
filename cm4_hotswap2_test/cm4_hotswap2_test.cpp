/*
 * cm4_hotswap2_test: two CM4 images co-resident in ITCM (A @ 0x1FFE0000 staged
 * at backdoor 0x20200000; B @ 0x1FFF0000 staged at 0x20210000). The CM7 boots
 * A, boots B (both now resident), then SWITCHES the boot VTOR back and forth
 * with Multicore.switchImage() -- reprogram GPR0/1 + re-pulse SW_RESET, NO
 * re-staging. idA==idA2 and idB==idB2 prove both images stayed resident and the
 * new-VTOR reboot works bidirectionally. The literal D7 + two-resident hot-swap.
 * Tokens per boot: ready=CAFE0001 then identity. PASS requires
 * idA==A1A1A1A1 && idB==B2B2B2B2 && idA2==A1A1A1A1 && idB2==B2B2B2B2.
 */
#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"
#include "Multicore.h"
#include "MessagingUnit.h"
#include "cm4_hs2_a.h"
#include "cm4_hs2_b.h"

#define STAGE_A 0x20200000u
#define STAGE_B 0x20210000u
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
/* Read a boot's {ready, identity}; return the identity (0xFFFFFFFF on timeout). */
static uint32_t read_identity(const char *tag, bool *ok)
{
    uint32_t r = 0, id = 0xFFFFFFFFu;
    if (wait_recv(0, &r)) { /* ready */ } else { ptimeout(tag); *ok = false; return id; }
    if (wait_recv(0, &id)) phex(tag, id); else { ptimeout(tag); *ok = false; }
    return id;
}

void setup()
{
    Serial1.begin(115200);
    Serial1.println("CM4HOTSWAP2-GATE v1");
    MU.begin();
    bool ok = true;

    Multicore.begin(cm4_hs2_a, sizeof(cm4_hs2_a), STAGE_A);   /* stage+boot A */
    uint32_t idA = read_identity("idA", &ok);

    Multicore.begin(cm4_hs2_b, sizeof(cm4_hs2_b), STAGE_B);   /* stage+boot B (A stays resident) */
    uint32_t idB = read_identity("idB", &ok);

    Multicore.switchImage(STAGE_A);                          /* reboot A -- no re-stage */
    uint32_t idA2 = read_identity("idA2", &ok);

    Multicore.switchImage(STAGE_B);                          /* reboot B -- no re-stage */
    uint32_t idB2 = read_identity("idB2", &ok);

    bool pass = ok && idA == 0xA1A1A1A1u && idB == 0xB2B2B2B2u
                   && idA2 == 0xA1A1A1A1u && idB2 == 0xB2B2B2B2u;
    Serial1.println(pass ? "HOTSWAP2=PASS" : "HOTSWAP2=FAIL");
    Serial1.println("CM4HOTSWAP2-DONE");
}
void loop() {}
