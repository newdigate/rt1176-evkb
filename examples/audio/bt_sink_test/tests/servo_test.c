// Host test for the A2DP sink's clock servo (NEW-41).  A discrete model of the closed loop: the source delivers
// blocks at (1 + drift), we consume them at (1 + trim), and the ring integrates the difference -- fill is carried
// in units of 1e-6 blocks so one block of drift-ppm is exactly one unit per block period, and the servo is fed the
// INTEGER block count it sees on the target (which is what makes the accumulator resolution load-bearing; see
// servo.h).  MIT.
#include "../servo.h"
#include <stdio.h>
#include <stdlib.h>
static int g_fails = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static void run(int32_t drift_ppm, int blocks, int64_t *fill_e6, int32_t *trim, int32_t *max_dev, int start_fill) {
    sink_servo_t s; servo_init(&s, 8); int64_t fill = (int64_t)start_fill * 1000000; *max_dev = 0;
    for (int i = 0; i < blocks; i++) {
        int32_t t = servo_step(&s, (int32_t)(fill / 1000000), 0);
        fill += drift_ppm - t;
        int32_t dev = (int32_t)(fill / 1000000) - 8; if (dev < 0) dev = -dev; if (dev > *max_dev) *max_dev = dev;
        if (fill < 0 || fill > 16000000) { *fill_e6 = fill; *trim = t; return; }
    }
    *fill_e6 = fill; *trim = s.trim_ppm;
}
int main(void) {
    int64_t f; int32_t t, dev;
    // 300 s, not 60: the closed-loop time constant is 1/(kp*1e-6) = 25000 blocks = 72.7 s, so a P-only loop needs
    // minutes to stand the ring off target by the drift/kp = 2.5 blocks that MAKE trim == drift.  Measured: at
    // 60 s this arm reads t=39 (it has covered only 1.7 blocks of the 2.5 it needs); it first reaches 95 ppm at
    // block 73346 = 213.2 s and settles at exactly 100.  The fix is simulated time, NOT a bigger kp -- kp sets
    // both the standing offset and how fast the PITCH moves, and this trims pitch.
    run(+100, 344 * 300, &f, &t, &dev, 8);  CHECK(t >= 95 && t <= 105);  CHECK(f > 0 && f < 16000000); CHECK(dev <= 4);
    run(-100, 344 * 300, &f, &t, &dev, 8);  CHECK(t <= -95 && t >= -105); CHECK(dev <= 4);
    run(+300, 344 * 120, &f, &t, &dev, 8);  CHECK(t == 200);                 // beyond the clamp: saturate, do not chase
    run(0, 344 * 10, &f, &t, &dev, 12);     CHECK(t > 0 && dev <= 4);        // started 4 blocks long: drain it
    // No dither: at rest on target the servo must emit exactly 0 and never twitch, or the pitch wanders audibly.
    // ★ ONE-SIDED, deliberately: this arm proves the servo does not move when it SHOULD NOT, and says nothing
    // about whether it moves when it should -- a servo hard-wired to return 0 passes it.  The arms above are
    // what carry that half (a +100 ppm drift must reach t == 100), and they are why this one is safe to keep
    // as a pure quiet check rather than a band.
    {   sink_servo_t s; servo_init(&s, 8); int32_t worst = 0;
        for (int i = 0; i < 344 * 60; i++) { int32_t x = servo_step(&s, 8, 0); if (x < 0) x = -x; if (x > worst) worst = x; }
        CHECK(worst == 0); CHECK(s.trim_ppm == 0); }
    // Burst rejection: a +4-block jump held for 20 blocks (a packet burst) must barely move the trim -- the 1 s
    // filter is what buys that.  Measured peak 9 ppm, and it decays back to 0 once the burst clears.
    {   sink_servo_t s; servo_init(&s, 8); for (int i = 0; i < 344 * 5; i++) servo_step(&s, 8, 0);
        int32_t peak = 0;
        for (int i = 0; i < 20; i++) { int32_t x = servo_step(&s, 12, 0); if (x < 0) x = -x; if (x > peak) peak = x; }
        CHECK(peak < 10);
        for (int i = 0; i < 344 * 5; i++) servo_step(&s, 8, 0);
        CHECK(s.trim_ppm == 0); }
    // hold freezes the trim (link down), and a new START drops the filter history but KEEPS the trim.
    {   sink_servo_t s; servo_init(&s, 8); for (int i = 0; i < 2000; i++) servo_step(&s, 10, 0); int32_t held = s.trim_ppm;
        CHECK(held > 0);
        for (int i = 0; i < 100; i++) CHECK(servo_step(&s, 0, 1) == held);
        servo_recentre(&s); CHECK(s.trim_ppm == held && s.filt_x65536 == 8 * 65536); }
    printf("servo_test: %d checks, %d failures\n", g_checks, g_fails); return g_fails ? 1 : 0;
}
