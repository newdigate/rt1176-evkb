#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"
#include "imxrt1176.h"
#include <math.h>

// QEMU gate (setup): asserts ONLY architecturally readable state -- CR config
// bits, FIFO pointers (PTR), status flags, watermark IRQ.  DATA is write-only
// on silicon, so the QEMU model's conversion codes are checked by the runner
// via the .dbg log, never by reading DATA back (anti-circularity rule).
// HW phase (loop): staircase + sine on TP18 for scope/DMM verification.

static volatile uint8_t dac_irq_fired = 0;
static void dac_isr(void) {
    // WMF is level status; mask the interrupt to stop refires. RMW of CR is
    // safe here: SWTRG/FIFORST/SWRST read as 0, but avoid clearing W1C flags.
    DAC_CR = (DAC_CR & ~(DAC_CR_UDFF_MASK | DAC_CR_OVFF_MASK)) & ~DAC_CR_WTMIE_MASK;
    dac_irq_fired = 1;
}

static void check(const char *name, uint32_t got, uint32_t want) {
    Serial1.print(name); Serial1.print("="); Serial1.print(got);
    if (got == want) Serial1.println(" OK");
    else { Serial1.print(" WANT "); Serial1.println(want); }
}

void setup() {
    Serial1.begin(115200);
    Serial1.println("RT1176 DAC12 test");

    // --- Phase 1: analogWriteDAC0 path ---
    analogWriteDAC0(128);                    // default 8-bit res -> code 0x800
    check("cr_dacen",  (DAC_CR & DAC_CR_DACEN_MASK)  ? 1u : 0u, 1);
    check("cr_dacrfs", (DAC_CR & DAC_CR_DACRFS_MASK) ? 1u : 0u, 1);
    check("cr_fifoen", (DAC_CR & DAC_CR_FIFOEN_MASK) ? 1u : 0u, 0);
    analogWriteResolution(12);
    analogWriteDAC0(4095);                   // -> 0xfff (dbg trace)
    analogWriteDAC0(2048);                   // -> 0x800 (dbg trace)

    // --- Phase 2: raw FIFO path (architectural readback; runs on HW too) ---
    DAC_CR &= ~DAC_CR_DACEN_MASK;
    DAC_CR |= DAC_CR_FIFORST_MASK;           // reset pointers (self-clearing)
    DAC_CR = DAC_CR_DACRFS_MASK | DAC_CR_TRGSEL_MASK | DAC_CR_FIFOEN_MASK
           | (4u << DAC_CR_WML_SHIFT);       // SW trigger, FIFO on, watermark 4
    DAC_CR |= DAC_CR_DACEN_MASK;
    DAC_DATA = 0x100; DAC_DATA = 0x200; DAC_DATA = 0x300; DAC_DATA = 0x400;
    check("wfp",  DAC_PTR & DAC_PTR_DACWFP_MASK, 4);
    check("rfp0", (DAC_PTR & DAC_PTR_DACRFP_MASK) >> DAC_PTR_DACRFP_SHIFT, 0);
    DAC_CR |= DAC_CR_SWTRG_MASK;             // pop one -> 3 left (< WML 4)
    check("rfp1", (DAC_PTR & DAC_PTR_DACRFP_MASK) >> DAC_PTR_DACRFP_SHIFT, 1);

    dac_irq_fired = 0;
    attachInterruptVector(IRQ_DAC, dac_isr);
    NVIC_ENABLE_IRQ(IRQ_DAC);
    DAC_CR |= DAC_CR_WTMIE_MASK;             // remaining 3 < WML 4 -> WMF -> IRQ
    for (uint32_t t = 0; t < 1000000u && !dac_irq_fired; t++) __asm__ volatile("nop");
    check("irq", dac_irq_fired, 1);
    check("wmf", (DAC_CR & DAC_CR_WMF_MASK) ? 1u : 0u, 1);
    DAC_CR |= DAC_CR_SWTRG_MASK;             // 2 left
    DAC_CR |= DAC_CR_SWTRG_MASK;             // 1 left -> nearly empty
    check("nemptf", (DAC_CR & DAC_CR_NEMPTF_MASK) ? 1u : 0u, 1);
    Serial1.println("[dac] done");

    // Restore the analogWriteDAC0 (non-FIFO) config for the HW phase in loop().
    DAC_CR &= ~DAC_CR_DACEN_MASK;
    DAC_CR = DAC_CR_DACRFS_MASK | DAC_CR_TRGSEL_MASK;
    DAC_CR |= DAC_CR_DACEN_MASK;
    analogWriteResolution(12);

    // --- Phase 3: internal DAC->ADC loopback (RM ch.87 internal sources:
    // ADC1 CH6A = DAC output).  DAC and ADC share the VREFH reference, so on
    // silicon read ~= written code -- a probe-free analog verification.  On
    // QEMU the LPADC model returns synthetic ch*0x111 (ch6 -> 1638 at 12-bit),
    // so these lines are informational there, decisive on HW.
    analogReadResolution(12);
    static const uint16_t lb[] = {0, 1024, 2048, 3072, 4095};
    for (unsigned i = 0; i < 5; i++) {
        analogWriteDAC0(lb[i]);
        delayMicroseconds(100);              // DAC settle
        Serial1.print("dacloop["); Serial1.print(lb[i]); Serial1.print("]=");
        Serial1.println(analogReadChannel(0, 6));
    }
    Serial1.println("[dacloop] done");

    // --- Phase 3b: loopback attenuation experiment (raw ADC1 command).
    // Baseline (phase 3) read ~0.23x on silicon: CMDL.CSCALE=0 scales the input
    // 30/64 (analog.c leaves it 0; SDK default is 1 = full scale), and the
    // remaining ~0.49x looks like S/H undercharge (STS=0 minimum sample time vs
    // the unbuffered DAC's output impedance).  Variants isolate each factor:
    //   B: CSCALE=1              (full scale, min sample, DAC unbuffered)
    //   C: CSCALE=1 + STS=7      (131.5-cycle sample)
    //   D: CSCALE=1 + STS=7 + DAC opamp buffer (BFEN|BFHS)
    struct { const char *tag; uint32_t cmdl; uint32_t cmdh; uint32_t cr2; } v[] = {
        { "B", ADC_CMDL_ADCH(6) | ADC_CMDL_CSCALE, 0,               DAC_CR2_IREF_MASK | DAC_CR2_OEN_MASK },
        { "C", ADC_CMDL_ADCH(6) | ADC_CMDL_CSCALE, ADC_CMDH_STS(7), DAC_CR2_IREF_MASK | DAC_CR2_OEN_MASK },
        { "D", ADC_CMDL_ADCH(6) | ADC_CMDL_CSCALE, ADC_CMDH_STS(7), DAC_CR2_IREF_MASK | DAC_CR2_BFEN_MASK | DAC_CR2_BFHS_MASK },
    };
    for (unsigned k = 0; k < 3; k++) {
        DAC_CR2 = v[k].cr2;
        for (unsigned i = 0; i < 5; i++) {
            analogWriteDAC0(lb[i]);
            delayMicroseconds(100);
            ADC1_CMDL1 = v[k].cmdl; ADC1_CMDH1 = v[k].cmdh;
            ADC1_TCTRL0 = ADC_TCTRL_TCMD(1);
            ADC1_SWTRIG = 1u;
            uint32_t guard = 100000u;
            while (!(ADC1_STAT & ADC_STAT_RDY) && --guard) { }
            uint32_t r = ADC1_RESFIFO;
            uint16_t d = (r & ADC_RESFIFO_VALID) ? (uint16_t)((r & ADC_RESFIFO_D) >> 4) : 0xFFFF;
            Serial1.print("lb"); Serial1.print(v[k].tag); Serial1.print("[");
            Serial1.print(lb[i]); Serial1.print("]="); Serial1.println(d);
        }
    }
    DAC_CR2 = DAC_CR2_IREF_MASK | DAC_CR2_OEN_MASK;   // restore unbuffered
    Serial1.println("[dacloop-exp] done");
}

// HW phase (scope/DMM on TP18; VREFH=1.8V nominal): 5-step staircase 2s each
// (0V, ~0.45V, ~0.9V, ~1.35V, ~1.8V), then 5s of 100Hz sine, repeating.
void loop() {
    static const uint16_t steps[] = {0, 1024, 2048, 3072, 4095};
    for (unsigned i = 0; i < 5; i++) {
        Serial1.print("dac="); Serial1.println(steps[i]);
        analogWriteDAC0(steps[i]);
        delay(2000);
    }
    Serial1.println("sine 100Hz x5s");
    uint32_t t0 = millis();
    while (millis() - t0 < 5000u) {
        float ph = (float)(micros() % 10000u) / 10000.0f;   // 100 Hz
        analogWriteDAC0((uint32_t)(2048.0f + 2047.0f * sinf(6.2831853f * ph)));
        delayMicroseconds(100);
    }
}
