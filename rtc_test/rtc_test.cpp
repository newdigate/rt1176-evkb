#include "Arduino.h"
#include "HardwareSerial.h"

// rtc_get / rtc_set are declared in core_pins.h (pulled in by Arduino.h).
// SNVS_* macros come from imxrt1176.h.

// Architectural Cortex-M System Control Block: Application Interrupt & Reset
// Control Register. Writing VECTKEY(0x05FA) | SYSRESETREQ(bit2) requests a warm
// system reset (re-runs the boot ROM -> ResetHandler; SNVS LP domain preserved).
#define SCB_AIRCR_REG (*(volatile uint32_t *)0xE000ED0Cu)
#define AIRCR_SYSRESETREQ 0x05FA0004u

#define TOKEN_MAGIC 0x52544331u    // "RTC1" — phase-2 marker held in SNVS_LPGPR0
#define KNOWN_EPOCH 1700000000ul   // 2023-11-14 22:13:20 UTC — distinct from the
                                   // Jan 1 2019 (1546300800) cold-boot default
#define DEFAULT_2019 1546300800ul

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}

    if (SNVS_LPGPR0 == TOKEN_MAGIC) {
        // -------- Phase 2: after the self-reset. Prove persistence. --------
        unsigned long expected = SNVS_LPGPR1;      // rtc_get() captured pre-reset
        unsigned long got = rtc_get();
        bool srtc_env = (SNVS_LPCR & SNVS_LPCR_SRTC_ENV) != 0;
        // Persisted if HP resumed from the battery-backed LP secure counter:
        // monotonically >= the pre-reset value (time did not reset backwards) and
        // NOT reverted to the Jan-2019 cold-boot default (which would mean startup
        // re-seeded == persistence lost). Those two clauses are what actually prove
        // persistence. The upper bound is a generous 1-day sanity ceiling, NOT a
        // tight window: in QEMU the SYSRESETREQ self-reset reboots in ~60 ms, but on
        // real hardware a debugger halts the core on SYSRESETREQ, so this same gate
        // is driven across a two-invocation warm reset (`LinkServer run` x2) whose
        // wall-clock gap is tens of seconds — a tight bound would false-fail on
        // silicon while adding no persistence-proving power over got>=expected.
        bool persisted = (got >= expected) && (got < expected + 86400ul) &&
                         (got > DEFAULT_2019 + 100);
        Serial1.print("phase2 expected="); Serial1.print(expected);
        Serial1.print(" got="); Serial1.print(got);
        Serial1.print(" srtc_env="); Serial1.println(srtc_env ? 1 : 0);
        bool ok = persisted && srtc_env;
        Serial1.println(ok ? "RTC_PERSIST=PASS" : "RTC_PERSIST=FAIL");
        SNVS_LPGPR0 = 0;                            // clear token — do NOT reset again
        Serial1.println(ok ? "RTC_ALL=PASS" : "RTC_ALL=FAIL");
        return;
    }

    // -------- Phase 1: first boot. Set + tick, then self-reset. --------
    bool ok = true;

    // Check A: set/get round-trip. Fails on unpatched QEMU (HP_TS ignored -> ~0).
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

    // Arm phase 2: store the token + the current time as the persistence baseline.
    SNVS_LPGPR1 = rtc_get();
    SNVS_LPGPR0 = TOKEN_MAGIC;
    Serial1.println("phase1 OK -> SYSRESETREQ");
    Serial1.flush();
    delay(50);
    SCB_AIRCR_REG = AIRCR_SYSRESETREQ;              // warm reset -> phase 2
    while (1) {}                                    // unreachable
}

void loop() {}
