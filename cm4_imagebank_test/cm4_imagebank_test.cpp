/* cm4_imagebank_test: Cm4ImageBank over Multicore.switchImage -- 4 CM4 images in
 * uniform ITCM slots prove the unified residency model. A/B/C in distinct slots
 * 0/1/2 (co-resident); D shares A's slot 0 (pages). switchTo() flips (no copy)
 * when resident, stages+evicts when not.
 *   idA/idB/idC : stage each (A's is the BT_RELEASE edge); A,B,C co-resident.
 *   resABC=E    : isResident nibble A=8,B=4,C=2,D=1 -> A,B,C set (co-residency).
 *   idA2        : switchTo(A) after B,C -> FAST FLIP (A still resident).
 *   idD         : switchTo(D) -> slot 0, evicts A.
 *   resD=7      : B,C,D set, A clear (slot-scoped eviction).
 *   idA3        : switchTo(A) -> RE-STAGE. ==A1 (not D4) proves A was re-copied,
 *                 not a stale flip onto D's slot -- the un-fakeable evict proof.
 *   resA3=E     : A,B,C set, D clear (A re-staged, D evicted).
 * PASS = all identities correct AND resABC/resD/resA3 == E/7/E. Public domain. */
#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"
#include "Multicore.h"
#include "MessagingUnit.h"
#include "Cm4ImageBank.h"
#include "Cm4Slots.h"
#include "cm4_ib_a.h"
#include "cm4_ib_b.h"
#include "cm4_ib_c.h"
#include "cm4_ib_d.h"

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
    if (!wait_recv(0, &r)) { ptimeout(tag); *ok = false; return id; }
    if (wait_recv(0, &id)) phex(tag, id); else { ptimeout(tag); *ok = false; }
    return id;
}

static Cm4ImageBank bank;
static int hA, hB, hC, hD;

/* isResident nibble: A=8, B=4, C=2, D=1. */
static uint32_t resmap(void)
{
    return ((uint32_t)bank.isResident(hA) << 3) | ((uint32_t)bank.isResident(hB) << 2)
         | ((uint32_t)bank.isResident(hC) << 1) |  (uint32_t)bank.isResident(hD);
}
/* switchTo + read this image's identity; flags a switch failure. */
static uint32_t sw(int h, const char *tag, bool *ok)
{
    if (!bank.switchTo(h)) { Serial1.print(tag); Serial1.println("=SWFAIL"); *ok = false; return 0xFFFFFFFFu; }
    return read_identity(tag, ok);
}

void setup()
{
    Serial1.begin(115200);
    Serial1.println("CM4IMAGEBANK-GATE v1");
    MU.begin();
    bool ok = true;

    hA = bank.add(cm4_ib_a, sizeof(cm4_ib_a), CM4_SLOT_STAGE(0), "A");
    hB = bank.add(cm4_ib_b, sizeof(cm4_ib_b), CM4_SLOT_STAGE(1), "B");
    hC = bank.add(cm4_ib_c, sizeof(cm4_ib_c), CM4_SLOT_STAGE(2), "C");
    hD = bank.add(cm4_ib_d, sizeof(cm4_ib_d), CM4_SLOT_STAGE(0), "D");   /* shares A's slot 0 */
    if (hA < 0 || hB < 0 || hC < 0 || hD < 0) { Serial1.println("ADD=FAIL"); ok = false; }

    uint32_t idA = sw(hA, "idA", &ok);          /* stage A (BT_RELEASE edge) */
    uint32_t idB = sw(hB, "idB", &ok);          /* stage B (distinct slot; A resident) */
    uint32_t idC = sw(hC, "idC", &ok);          /* stage C (distinct slot; A,B resident) */
    uint32_t resABC = resmap();  phex("resABC", resABC);   /* expect E: A,B,C */

    uint32_t idA2 = sw(hA, "idA2", &ok);        /* A resident -> FAST FLIP */
    uint32_t idD  = sw(hD, "idD", &ok);         /* slot 0 -> evicts A */
    uint32_t resD = resmap();  phex("resD", resD);         /* expect 7: B,C,D */

    uint32_t idA3 = sw(hA, "idA3", &ok);        /* A not resident -> RE-STAGE (==A1, not D4) */
    uint32_t resA3 = resmap(); phex("resA3", resA3);       /* expect E: A,B,C */

    bool pass = ok
        && idA == 0xA1A1A1A1u && idB == 0xB2B2B2B2u && idC == 0xC3C3C3C3u
        && idA2 == 0xA1A1A1A1u && idD == 0xD4D4D4D4u && idA3 == 0xA1A1A1A1u
        && resABC == 0xEu && resD == 0x7u && resA3 == 0xEu;
    Serial1.println(pass ? "IMAGEBANK=PASS" : "IMAGEBANK=FAIL");
    Serial1.println("CM4IMAGEBANK-DONE");
}
void loop() {}
