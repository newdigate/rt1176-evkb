/* cm4_usb_enum_probe -- Phase 7.2c of the CM4 USB arc: the REAL USBHost_t36
 * stack, compiled for the Cortex-M4 image world, enumerating a USB device and
 * reporting its VID/PID.
 *
 * 7.1 (cm4_usb_irq_probe) proved the CM4 can own the port: it brought up
 * LPCG115, USBPHY2's 480 MHz PLL, the EHCI reset, host mode and port power with
 * hand-written literals, and took IRQ 135 on its own NVIC.  This image deletes
 * all of that and calls USBHost::begin() instead -- the same function the CM7
 * has been running on silicon since the UAC work -- so what is under test is
 * the library, not a distillation of it.  If the register sequence were the
 * risk, 7.1 already answered it.
 *
 * WHAT IS ACTUALLY NEW HERE, and what a green run proves:
 *   - 5,797 lines of C++ USB host stack link and run -nostdlib on the CM4;
 *   - the EHCI bus master reaches the descriptors the CM4 allocated (they are
 *     in OCRAM2, NOT in the CM4's DTCM -- see cm4.ld);
 *   - the static vector table's slot 151 reaches USBHost::isr(), so the
 *     interrupt-driven half of enumeration (UAI -> followup_Transfer) closes;
 *   - a device's descriptors were fetched OVER THE WIRE, because the VID/PID
 *     reported are the attached device's and nothing on this core knows them.
 *
 * The CM7 boots this image and relays the MU stream.  It touches no USB
 * register -- a CM7 that configured the block would make a CM4 failure
 * invisible, the circular-pass shape 3.1 warned about.
 *
 * Public domain (N. Newdigate). */
#include <Arduino.h>          /* cm4_shim/Arduino.h -- resolved ahead of the
                               * core header by INCLUDE_DIRS ordering */
#include "USBHost_t36.h"

/* ---- MU B side (the CM4's), TR channel 0 ----------------------------------
 *
 * MUB_BASE, MUB_SR and MU_SR_TE() come from imxrt1176.h, which the shim now
 * pulls in for the USB register map -- so unlike the earlier CM4 gates this one
 * does NOT re-spell them as local literals. Same addresses either way (the MU is
 * one block on the system bus), but one definition cannot drift from itself.
 * Only MUB_TR is missing there: the core header declares the A-side MUA_TR(n)
 * because the CM7 library uses it, and no CM7 code writes the B side. */
#define REG32(a)   (*(volatile uint32_t *)(a))
#define MUB_TR(n)  REG32(MUB_BASE + 0x00u + ((unsigned)(n) << 2))

static void mu_send(uint32_t v) {
    while (!(MUB_SR & MU_SR_TE(0))) {}
    MUB_TR(0) = v;
}

/* ---- the C++ image world's prologue (Phase 5: cm4_cpp_test) ---- */
extern "C" void cm4_run_ctors(void);

/* ★★ DWT FIRST, AND ITS FAILURE IS SILENT.
 *
 * The shim derives millis()/micros()/delay()/delayMicroseconds() from
 * ARM_DWT_CYCCNT.  If DEMCR.TRCENA and DWT_CTRL.CYCCNTENA are not set, CYCCNT
 * reads 0 forever, so every one of those clocks stands still -- and USBHost_t36
 * is full of timeouts measured against them.  None of them would ever fire.
 * Enumeration would not fail; it would simply never finish and never give up,
 * with nothing in the transcript to say why.
 *
 * So it is the first thing main() does, and the FIRST observation on the wire
 * is a measured cycle delta, not an assertion that the register was written.
 * A dead counter then reads a literal zero in the transcript instead of being
 * inferred three stages later from a hang.
 *
 * ARM_DEMCR / ARM_DEMCR_TRCENA / ARM_DWT_CTRL / ARM_DWT_CYCCNT all come from
 * imxrt1176.h via the shim; the earlier CM4 gates re-spelled them locally
 * because the shim carried only ARM_DWT_CYCCNT. */
static void dwt_start(void) {
    ARM_DEMCR    |= ARM_DEMCR_TRCENA;
    ARM_DWT_CYCCNT = 0u;
    ARM_DWT_CTRL |= 1u;                          /* CYCCNTENA */
}

/* Bounded, and deliberately not delayMicroseconds(): if CYCCNT is dead, a
 * cycle-count wait never returns.  This one always returns, and returns 0. */
static uint32_t dwt_measure(void) {
    uint32_t t0 = ARM_DWT_CYCCNT;
    for (volatile uint32_t i = 0; i < 200u; i++) { }
    return ARM_DWT_CYCCNT - t0;
}

/* ---- the USB host ----------------------------------------------------------
 *
 * USBHub is DMAMEM because the object CONTAINS the DMA-walked structures
 * (Device_t mydevices[7], Pipe_t mypipes[2] -- which embed the queue heads --
 * and Transfer_t mytransfers[4], the qTDs), and its constructor contributes
 * them to the free pools enumeration allocates from.  In DTCM they would be
 * unreachable by the EHCI bus master; see cm4.ld and the shim's DMAMEM note.
 *
 * The hub is here for its MEMORY as much as for hubs: memory.cpp's seed pool is
 * 1 device / 1 pipe / 4 transfers and contributes no string buffers at all. */
USBHost myusb;
DMAMEM USBHub hub1(myusb);

/* ---- the enumeration observer ---------------------------------------------
 *
 * USBHost_t36 has no public API for "what is plugged in" -- a device is only
 * visible to something that claims it.  So this is the usb_descriptor_survey
 * pattern (examples/usb/usb_descriptor_survey, HW-proven on the CM7): a
 * USBDriver whose claim() records what it was offered and then returns FALSE.
 *
 * Returning false matters twice.  The device stays unclaimed, so this driver
 * remains eligible for the next one; and nothing here binds pipes or starts
 * transfers, so the probe measures enumeration alone.
 *
 * ★ claim_count IS the interrupt evidence.  claim() is reached only through
 * claim_drivers() <- enumeration_receive() <- followup_Transfer(), and
 * followup_Transfer runs in USBHost::isr() on the UAI (async schedule
 * complete) interrupt -- ehci.cpp:380.  A non-zero count therefore cannot
 * happen unless IRQ 135 was delivered to the CM4's own NVIC and dispatched
 * through vector slot 151.  There is no polled path to it. */
static volatile uint32_t claim_count;
static volatile uint32_t dev_claims;      /* type==0, i.e. device-level offers */
static volatile uint16_t seen_vid, seen_pid;
static volatile uint8_t  seen_class, seen_subclass, seen_speed, seen_addr;

class USBEnumProbe : public USBDriver {
public:
    USBEnumProbe(USBHost &host)  { (void)host; init(); }
protected:
    virtual bool claim(Device_t *dev, int type, const uint8_t *descriptors, uint32_t len) {
        (void)descriptors; (void)len;
        claim_count++;
        if (type == 0) {                  /* device level: the descriptor is fetched */
            seen_vid      = dev->idVendor;
            seen_pid      = dev->idProduct;
            seen_class    = dev->bDeviceClass;
            seen_subclass = dev->bDeviceSubClass;
            seen_speed    = dev->speed;
            seen_addr     = dev->address;
            dev_claims++;
        }
        return false;                     /* never bind -- see the note above */
    }
    virtual void disconnect() {}
private:
    void init() { driver_ready_for_device(this); }
};

USBEnumProbe enumprobe(myusb);

/* Observation window.  Long enough for a hand-plugged device on silicon, and
 * for QEMU's monitor round-trip plus the 100 ms debounce + reset + enumeration
 * in the emulator.  It exits early the moment a device-level claim lands, so a
 * green run does not pay for the whole window. */
#define WINDOW_MS       12000u
/* Hard iteration cap, belt-and-braces: if CYCCNT were dead, millis() would
 * never advance and the millis() bound alone would spin forever -- turning the
 * one failure this probe most wants to REPORT into a hang.  s1 would already
 * have shown dwt=0, but the transcript should still reach its done marker. */
#define WINDOW_LOOPS    200000000u

int main(void) {
    dwt_start();                          /* ★ before ANY clock call -- see above */
    uint32_t dwt = dwt_measure();

    /* Static constructors: USBHost/USBHub/USBEnumProbe are file-scope objects
     * whose constructors call driver_ready_for_device() and contribute the DMA
     * pools.  Skipping this walk gives a host with NO registered drivers and NO
     * hub memory -- it would enumerate and then report nothing, silently. */
    cm4_run_ctors();

    mu_send(0xE5B00001u);                 /* stage 1: image alive, clocks up */
    mu_send(dwt);                         /* observation: 0 => CYCCNT is dead */

    /* ★ PER-IMAGE OBLIGATION.  The copied startup_cm4.S leaves PRIMASK set
     * (Reset_Handler opens with `cpsid i`).  QEMU dispatches the ISR anyway in
     * some paths, so a missing cpsie i PASSES in the emulator and false-FAILs
     * on silicon.  Phase 5 hit this; 7.1 hit it; it lives in each image's main.
     *
     * Before begin(), not after: begin() arms the NVIC and the interrupt mask
     * itself, and a device already present on the port raises PCI inside it. */
    __enable_irq();

    /* Everything 7.1 did by hand, done by the library instead: LPCG115, the
     * USBPHY2 PLL, the EHCI reset, the DMA pools, host mode + SDIS, the
     * periodic list, port power, RUN, the vector, NVIC 135, and USBINTR.
     *
     * ★ Do NOT re-clear USBSTS after this returns, and do not narrow USBINTR.
     * 7.1 enabled PCE alone because it ran no schedules; enumeration completes
     * every control transfer through UAI -> followup_Transfer (ehci.cpp:380),
     * so the mask begin() installs (PCE|TIE0|TIE1|UEE|SEE|UPIE|UAIE) is load
     * bearing.  usbintr below is that mask read back, so a narrowed one is
     * visible in the transcript rather than inferred from a stall. */
    myusb.begin();

    mu_send(0xE5B00002u);                 /* stage 2: begin() RETURNED */
    mu_send(USB2_USBINTR);                /* observation: UAIE(18) must be set */

    /* Settle, then report the port BEFORE anything is plugged.  CCS is expected
     * 0 here: the QEMU gate device_adds on this marker and the operator plugs
     * J47 on it, so portsc -> portsc2 is a real 0->1 connect edge rather than a
     * device that was present all along.  Same un-fakeable shape as 7.1. */
    delay(200);
    mu_send(0xE5B00003u);                 /* stage 3: ARMED AND WAITING */
    mu_send(USB2_PORTSC1);                /* observation: CCS(bit0) expected 0 */

    uint32_t t0 = millis();
    uint32_t loops = 0;
    while ((uint32_t)(millis() - t0) < WINDOW_MS && loops < WINDOW_LOOPS) {
        myusb.Task();                     /* driver Task()s; enumeration itself
                                           * is entirely interrupt-driven */
        loops++;
        if (dev_claims) break;            /* got a device descriptor -- done */
    }
    /* A short grace period so the interface-level claim() offers that follow
     * the device-level one are counted before the totals are read. */
    if (dev_claims) delay(50);

    mu_send(0xE5B00004u);                 /* stage 4: window closed */
    mu_send(claim_count);                 /* observation: claim() call count */

    mu_send((uint32_t)seen_vid);          /* THE ANSWER: fetched over the wire */
    mu_send((uint32_t)seen_pid);
    mu_send(((uint32_t)seen_class    << 24) | ((uint32_t)seen_subclass << 16)
          | ((uint32_t)seen_speed    <<  8) |  (uint32_t)seen_addr);
    mu_send(USB2_PORTSC1);                /* portsc2: CCS expected 1 */
    mu_send(USB2_USBSTS);                 /* stsraw: what the ISR did NOT clear.
                                           * Non-zero with claims=0 means the
                                           * condition arose but was never
                                           * serviced -- i.e. the vector, not
                                           * the controller, is the problem. */
    mu_send(loops);                       /* a spin vs a hang, distinguishably */
    mu_send(0xD0DE0008u);                 /* done marker */

    for (;;) { __asm volatile ("wfi"); }
}
