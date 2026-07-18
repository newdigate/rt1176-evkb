/*
 * RT1176 dual-core SRC/MU probe: hardware vs QEMU model cross-validation.
 *
 * Runs on the CM7 (this firmware), boots the CM4 with an embedded probe blob,
 * and prints one "token=HEXVALUE" line per observation over Serial1 (LPUART1)
 * so a hardware transcript and a QEMU transcript can be diffed directly.
 *
 * Probes, in order:
 *   sr0     MUA.SR at boot            expect 00F00080 (TE0-3 set, RS set)
 *   trrb    MUA.TR2 readback after writing 55AA55AA   (RM 35.7.1.2: reads 0)
 *   srte2   MUA.SR after the TR2 write expect 00D00080 (TE2 clear; msg parked)
 *   gpr0/1  IOMUXC_LPSR_GPR0/1 readback after setting CM4 VTOR 0x1FFE0000
 *   scr     SRC_SCR readback after setting BT_RELEASE_M4
 *   hello   first CM4 message (RR0)   expect C0FFEE42
 *   rsaft   MUA.SR.RS after release   expect 0
 *   girset  MUA.CR GIRn field right after triggering GIR0 (timing-sensitive)
 *   dbell   CM4 doorbell ack (RR1)    expect D00DFEED
 *   giraft  MUA.CR GIRn after the CM4 acked GIP0        expect 0 (auto-clear)
 *   echo    CM4 echo+1 via the MUA interrupt (NVIC 118) expect 12345679
 *   irqcnt  number of MU IRQs taken   expect 1
 *   ctrlrb  SRC CTRL_M4CORE readback right after SW_RESET
 *   hello2  CM4 hello after the SW-reset restart        expect C0FFEE42
 *   rshold  MUA.SR.RS after writing SCR.BT_RELEASE_M4 = 0   (divergence probe:
 *           the QEMU model does not re-hold; silicon behavior TBD)
 *   alive   CM4 echo after the SCR=0 experiment (AABBCC01 if still running,
 *           TIMEOUT if the core was re-held)
 *   hello3  CM4 hello after re-setting BT_RELEASE_M4 (reboot if an edge)
 *   bsr     MUB.SR read from the CM7 side, last in case cross-domain access
 *           faults on silicon
 */
#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"

#define REG32(a)   (*(volatile uint32_t *)(a))
#define MUA_TR(n)  REG32(0x40C48000u + 0x00 + 4 * (n))
#define MUA_RR(n)  REG32(0x40C48000u + 0x10 + 4 * (n))
#define MUA_SR     REG32(0x40C48000u + 0x20)
#define MUA_CR     REG32(0x40C48000u + 0x24)
#define MUB_SR     REG32(0x40C4C000u + 0x20)
#define LPSR_GPR0  REG32(0x40C0C000u + 0x00)
#define LPSR_GPR1  REG32(0x40C0C000u + 0x04)
#define SRC_SCR    REG32(0x40C04000u + 0x00)
#define SRC_CTRL_M4CORE REG32(0x40C04000u + 0x284)

#define SR_RF(n)   (1u << (27 - (n)))
#define SR_TE(n)   (1u << (23 - (n)))
#define SR_RS      (1u << 7)
#define CR_GIR(n)  (1u << (19 - (n)))
#define CR_RIE(n)  (1u << (27 - (n)))
#define CR_GIR_MASK 0x000F0000u

#define CM4_IMG_SYS  0x20200000u   /* CM4 ITCM through the system backdoor */
/*
 * Boot VTOR: the SDK/Zephyr dual-core demos program the *system* address of
 * the image (CORE1_BOOT_ADDRESS = 0x20200000), not the CM4-private ITCM
 * alias 0x1FFE0000 -- the initial vector-table fetch goes over the system
 * fabric, while the SP/PC values inside the table still use CM4-view
 * addresses (code at 0x1FFE0xxx).  A 0x1FFE0000 VTOR does not boot on
 * silicon (verified on the EVKB); QEMU accepts both.
 */
#define CM4_VTOR     CM4_IMG_SYS

/* Position-independent CM4 probe (see cm4_probe_blob.s alongside this file
 * for source + disassembly): hello on TR0, GIP0 -> ack + D00DFEED on TR1,
 * RF3 -> echo RR3+1 on TR3. */
static const uint32_t cm4_blob[] = {
    0x20020000, 0x1FFE0009, 0x490A4809, 0x6A026001,
    0x4F00F012, 0xF04FD004, 0x62034300, 0x60434B06,
    0x7F80F012, 0x69C3D0F3, 0x60C33301, 0x0000E7EF,
    0x40C4C000, 0xC0FFEE42, 0xD00DFEED,
};

/* Bounded MMIO polls: ~3M reads is >100 ms on silicon and a few seconds in
 * TCG, both far beyond the CM4's microsecond-scale response time. */
#define WAIT_LONG  3000000u
#define WAIT_SHORT 300000u

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

static bool wait_sr(uint32_t mask, uint32_t loops)
{
    while (loops--) {
        if (MUA_SR & mask) {
            return true;
        }
    }
    return false;
}

static volatile uint32_t mu_irq_val;
static volatile uint32_t mu_irq_count;

static void mu_isr(void)
{
    mu_irq_val = MUA_RR(3);        /* clears RF3, dropping the request */
    mu_irq_count++;
}

void setup()
{
    Serial1.begin(115200);
    Serial1.println("DUALMU-PROBE v1");

    /* --- MU state with the CM4 held in reset --- */
    phex("sr0", MUA_SR);

    MUA_TR(2) = 0x55AA55AAu;
    phex("trrb", MUA_TR(2));
    phex("srte2", MUA_SR);

    /* --- stage the CM4 image and boot vector --- */
    volatile uint32_t *dst = (volatile uint32_t *)CM4_IMG_SYS;
    volatile uint32_t *dst_hi = (volatile uint32_t *)(CM4_IMG_SYS + 0x20000);
    for (unsigned i = 0; i < sizeof(cm4_blob) / 4; i++) {
        dst[i] = cm4_blob[i];      /* backdoor low half (RAM_L?) */
        dst_hi[i] = cm4_blob[i];   /* and high half, in case the order is swapped */
    }
    __asm__ volatile("dsb; isb");
    /* Backdoor readback: RM Table 3-1 note 3 says this window is
     * unpredictable while the CM4 is powered down. */
    phex("img0", dst[0]);

    LPSR_GPR0 = CM4_VTOR & 0xFFF8u;
    LPSR_GPR1 = CM4_VTOR >> 16;
    phex("gpr0", LPSR_GPR0);
    phex("gpr1", LPSR_GPR1);

    /* --- pre-release plumbing state: M4 clock gate, GPC CPU1 mode, slice
     * reset status --- */
    phex("lpcg", REG32(0x40CC6020u));       /* CCM LPCG1 (M4) DIRECT */
    REG32(0x40CC6020u) = 1u;                /* force the M4 clock gate on */
    __asm__ volatile("dsb; isb");
    phex("gpcst", REG32(0x40C00814u));      /* GPC_CPU_MODE_CTRL_1 CM_MODE_STAT */
    phex("statp", REG32(0x40C04290u));      /* SRC STAT_M4CORE */

    /* --- release, in the order Zephyr/SDK use: slice SW reset first, so a
     * CM4 left out of reset by the boot ROM or a debugger connect script is
     * forced back through a clean reset-vector fetch, then BT release --- */
    phex("scr_pre", SRC_SCR);
    SRC_CTRL_M4CORE = 1u;
    SRC_SCR |= 1u;
    phex("scr", SRC_SCR);
    for (volatile uint32_t n = WAIT_SHORT; n; n--) {
    }
    phex("stata", REG32(0x40C04290u));      /* slice status after release */
    phex("sraft", MUA_SR);                  /* full SR: does bit 9 drop? */

    if (wait_sr(SR_RF(0), WAIT_LONG)) {
        phex("hello", MUA_RR(0));
    } else {
        ptimeout("hello");
    }
    phex("rsaft", (MUA_SR >> 7) & 1u);

    /* --- doorbell: GIR0 -> CM4 GIP0 -> ack -> GIR auto-clear --- */
    MUA_CR = (MUA_CR & ~CR_GIR_MASK) | CR_GIR(0);
    phex("girset", MUA_CR & CR_GIR_MASK);
    if (wait_sr(SR_RF(1), WAIT_LONG)) {
        phex("dbell", MUA_RR(1));
    } else {
        ptimeout("dbell");
    }
    phex("giraft", MUA_CR & CR_GIR_MASK);

    /* --- echo through the MUA interrupt (NVIC 118) --- */
    attachInterruptVector((IRQ_NUMBER_t)118, mu_isr);
    NVIC_ENABLE_IRQ(118);
    MUA_CR |= CR_RIE(3);
    MUA_TR(3) = 0x12345678u;
    for (uint32_t n = WAIT_LONG; n && !mu_irq_count; n--) {
    }
    if (mu_irq_count) {
        phex("echo", mu_irq_val);
    } else {
        ptimeout("echo");
    }
    phex("irqcnt", mu_irq_count);
    MUA_CR &= ~CR_RIE(3);

    /* --- SW reset of the released core: must reboot from the GPR vector --- */
    SRC_CTRL_M4CORE = 1u;
    phex("ctrlrb", SRC_CTRL_M4CORE);
    if (wait_sr(SR_RF(0), WAIT_LONG)) {
        phex("hello2", MUA_RR(0));
    } else {
        ptimeout("hello2");
    }

    /* --- divergence probe: write BT_RELEASE_M4 back to 0 --- */
    SRC_SCR &= ~1u;
    for (volatile uint32_t n = WAIT_SHORT; n; n--) {
    }
    phex("scr0rb", SRC_SCR);   /* does the 0 stick, or is the bit W1-only? */
    phex("rshold", (MUA_SR >> 7) & 1u);
    MUA_TR(3) = 0xAABBCC00u;
    if (wait_sr(SR_RF(3), WAIT_SHORT)) {
        phex("alive", MUA_RR(3));
    } else {
        ptimeout("alive");
    }

    /* --- re-release: an edge if (and only if) the 0-write took effect --- */
    SRC_SCR |= 1u;
    if (wait_sr(SR_RF(0), WAIT_SHORT)) {
        phex("hello3", MUA_RR(0));
    } else {
        ptimeout("hello3");
    }

    /* --- cross-domain read, last in case it faults on silicon --- */
    phex("bsr", MUB_SR);

    Serial1.println("DUALMU-DONE");
}

void loop()
{
}
