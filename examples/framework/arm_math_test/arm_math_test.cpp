#include "Arduino.h"
#include "HardwareSerial.h"
#include <arm_math.h>
#include <math.h>
#include <stdlib.h>

// Known-answer tests for the CMSIS-DSP manifest library. Pure CPU math:
// identical output expected in QEMU and on silicon.

// STAGE_FFT: 256-pt radix-4 complex q15 FFT of cos(2*pi*8*i/256) at 0.5 FS.
// All energy must land in bin 8 (mirror bin 248 ignored; we scan 1..127).
static q15_t fft_buf[512];
static bool stage_fft(void) {
    arm_cfft_radix4_instance_q15 inst;
    if (arm_cfft_radix4_init_q15(&inst, 256, 0, 1) != ARM_MATH_SUCCESS) {
        Serial1.println("ARM-MATH: fft init failed");
        return false;
    }
    for (int i = 0; i < 256; i++) {
        float c = cosf(2.0f * (float)M_PI * 8.0f * (float)i / 256.0f);
        fft_buf[2 * i]     = (q15_t)lrintf(c * 16384.0f);
        fft_buf[2 * i + 1] = 0;
    }
    arm_cfft_radix4_q15(&inst, fft_buf);          // in-place, output scaled 1/N
    uint32_t best = 0, best_mag = 0, second = 0;
    for (uint32_t k = 1; k < 128; k++) {
        int32_t re = fft_buf[2 * k], im = fft_buf[2 * k + 1];
        uint32_t mag = (uint32_t)(re * re + im * im);
        if (mag > best_mag) { second = best_mag; best_mag = mag; best = k; }
        else if (mag > second) { second = mag; }
    }
    Serial1.print("ARM-MATH: fft bin=");  Serial1.print(best);
    Serial1.print(" mag2=");              Serial1.print(best_mag);
    Serial1.print(" next2=");             Serial1.println(second);
    return best == 8 && best_mag > 0 && best_mag > 4 * second;
}

// STAGE_FIR: unit impulse through an 8-tap FIR echoes the coefficients.
static const q15_t fir_coeffs[8] = {1000, 2000, 3000, 4000, 4000, 3000, 2000, 1000};
static q15_t fir_state[8 + 32 - 1];
static bool stage_fir(void) {
    arm_fir_instance_q15 f;
    if (arm_fir_init_q15(&f, 8, (q15_t *)fir_coeffs, fir_state, 32) != ARM_MATH_SUCCESS) {
        Serial1.println("ARM-MATH: fir init failed");
        return false;
    }
    q15_t x[32] = {0}, y[32] = {0};
    x[0] = 32767;                                  // ~1.0 in q15
    arm_fir_fast_q15(&f, x, y, 32);
    bool ok = true;
    for (int i = 0; i < 8; i++)
        if (abs((int)y[i] - (int)fir_coeffs[i]) > 4) ok = false;
    for (int i = 8; i < 32; i++)
        if (abs((int)y[i]) > 4) ok = false;
    Serial1.print("ARM-MATH: fir y=");
    for (int i = 0; i < 8; i++) { Serial1.print((int)y[i]); Serial1.print(i < 7 ? "," : "\n"); }
    return ok;
}

// STAGE_SIN: arm_sin_q31 vs libm across one turn (q31 angle maps [0,1) -> [0,2pi)).
static bool stage_sin(void) {
    float maxerr = 0.0f;
    for (int i = 0; i < 64; i++) {
        float x = (float)i / 64.0f;
        q31_t a = (q31_t)llrintf(x * 2147483648.0f);
        float got  = (float)arm_sin_q31(a) / 2147483648.0f;
        float want = sinf(2.0f * (float)M_PI * x);
        float err  = fabsf(got - want);
        if (err > maxerr) maxerr = err;
    }
    Serial1.print("ARM-MATH: sin maxerr="); Serial1.println(maxerr, 7);
    return maxerr < 1.0e-4f;
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    bool fft = stage_fft();
    bool fir = stage_fir();
    bool sn  = stage_sin();
    Serial1.println(fft ? "STAGE_FFT=PASS" : "STAGE_FFT=FAIL");
    Serial1.println(fir ? "STAGE_FIR=PASS" : "STAGE_FIR=FAIL");
    Serial1.println(sn  ? "STAGE_SIN=PASS" : "STAGE_SIN=FAIL");
    Serial1.println((fft && fir && sn) ? "ARM_MATH_ALL=PASS" : "ARM_MATH_ALL=FAIL");
}
void loop() {}
