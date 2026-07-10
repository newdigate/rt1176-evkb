#include "Arduino.h"
#include "HardwareSerial.h"

// rtc_get / rtc_set are declared in core_pins.h (pulled in by Arduino.h).
// SNVS_* macros come from imxrt1176.h.

// Architectural Cortex-M System Control Block: Application Interrupt & Reset
// Control Register. Writing VECTKEY(0x05FA) | SYSRESETREQ(bit2) requests a warm
// system reset (re-runs the boot ROM -> ResetHandler; the SNVS LP secure counter is
// preserved). On real silicon a debugger (LinkServer) halts the core on this reset;
// run the gate detached (physical reset / power-cycle) or rely on the counter having
// already persisted across the flash reset (phase 2 then reports on the first boot).
#define SCB_AIRCR_REG (*(volatile uint32_t *)0xE000ED0Cu)
#define AIRCR_SYSRESETREQ 0x05FA0004u

#define KNOWN_EPOCH 1700000000ul   // 2023-11-14 22:13:20 UTC — well above the 2019 default
#define DEFAULT_2019 1546300800ul  // startup.c cold-boot seed (Jan 1 2019)

// Persistence is detected via the LP SECURE COUNTER itself (rtc_get), NOT a scratch
// SNVS_LPGPR register. HW-observed on this RT1176 EVKB: a reset ZEROES the scratch
// SNVS_LPGPR while RETAINING the secure RTC counter (retaining time across reset is the
// counter's entire purpose) — so the counter is the correct, silicon-valid persistence
// sentinel. In QEMU the counter likewise survives the machine reset (reset_hold
// preserves the LP RTC). Run under -icount for deterministic delay()/RTC coupling.
void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}

    unsigned long now = rtc_get();
    if (now >= KNOWN_EPOCH && now < KNOWN_EPOCH + 86400ul) {
        // Phase 2: a prior boot set KNOWN_EPOCH and the secure counter survived the
        // warm reset (and kept running) -> time persisted; NOT reverted to the 2019
        // cold-boot default. This is the whole deliverable: rtc_get() returns live,
        // persisted epoch seconds after a reset.
        Serial1.print("phase2 now="); Serial1.print(now);
        Serial1.print(" (KNOWN+"); Serial1.print(now - KNOWN_EPOCH); Serial1.println("s)");
        Serial1.println("RTC_PERSIST=PASS");
        Serial1.println("RTC_ALL=PASS");
        while (1) {}   // stop: persistence demonstrated, do not reset again
    }

    // Phase 1: fresh clock (cold 2019 default / never set). Verify the set/get
    // round-trip + 1 Hz tick, then warm-reset and let phase 2 confirm persistence.
    bool ok = true;

    // Check A: set/get round-trip (exercises rtc_set -> LP secure counter -> HP_TS
    // sync -> rtc_get). Failed against unpatched QEMU (HP_TS ignored -> HP read ~0).
    rtc_set(KNOWN_EPOCH);
    unsigned long r0 = rtc_get();
    bool setget = (r0 >= KNOWN_EPOCH) && (r0 < KNOWN_EPOCH + 2);
    if (!setget) ok = false;

    // Check B: 1 Hz advance. delay ~2 s is deterministic under -icount; assert the
    // counter climbed ~2 s, cross-checked against the micros() delta.
    unsigned long t0 = rtc_get();
    unsigned long us0 = micros();
    delay(2000);
    unsigned long t1 = rtc_get();
    unsigned long us1 = micros();
    unsigned long dsec = t1 - t0;
    unsigned long dus = us1 - us0;                  // ~2_000_000
    bool tick = (dsec >= 1 && dsec <= 3) &&
                (dus > 1500000ul && dus < 2500000ul);
    if (!tick) ok = false;

    Serial1.print("phase1 r0="); Serial1.print(r0);
    Serial1.print(" dsec="); Serial1.print(dsec);
    Serial1.print(" dus="); Serial1.println(dus);
    Serial1.println(setget ? "RTC_SETGET=PASS" : "RTC_SETGET=FAIL");
    Serial1.println(tick ? "RTC_TICK=PASS" : "RTC_TICK=FAIL");

    if (!ok) {                                      // don't self-reset on phase-1 fail
        Serial1.println("RTC_ALL=FAIL");
        return;
    }

    Serial1.println("phase1 OK -> SYSRESETREQ");
    Serial1.flush();
    delay(50);
    SCB_AIRCR_REG = AIRCR_SYSRESETREQ;              // warm reset -> phase 2
    while (1) {}                                    // unreachable
}

void loop() {}
