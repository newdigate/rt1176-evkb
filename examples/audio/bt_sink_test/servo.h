// The A2DP sink's clock servo (NEW-41), pure C so the host test can drive it.  Input: the PCM ring's fill in blocks,
// sampled once per audio block (every 128 samples).  Output: the PLL trim in ppm.  Proportional on FILL -- which is
// already the integral of the rate error -- with a first-order filter on the fill (tau ~1 s = 344 blocks) so packet
// bursts do not move the pitch, a +-200 ppm clamp, and hold (no update) when told the link is down.
//
// ** The filter accumulator is x65536, NOT x256. **  The fill this servo is fed is an INTEGER block count, so the
// smallest step it ever sees is one whole block.  At x256 that step is 256 units and the EMA's `diff / 344`
// truncates it to ZERO -- the filter never moves at all and the servo outputs 0 ppm forever, whatever the drift.
// (Measured, not argued: the x256 form fails the +100 ppm arm of tests/servo_test.c with t == 0.)  At x65536 a
// one-block step is 65536 units, the increment is 190, and the residual stall band is 343/65536 = 0.005 blocks.
//
// ** A P-only servo has a steady-state fill offset of drift/kp BY CONSTRUCTION, and that is a design property, not
// a defect. **  The loop converges when trim == drift, which requires err == drift/kp blocks of standing fill
// error: at kp = 40 ppm/block, +-100 ppm of source drift parks the ring 2.5 blocks off TARGET.  Latency is
// therefore TARGET +- 2.5 blocks (+-7 ms) over +-100 ppm -- stated against TARGET, not a constant, because
// NEW-42 made the ring a build knob and the CONTROL arm still builds at 16 / 8.  Adding
// an integral term would remove the offset and buy nothing: the offset is small, bounded and silent, while an
// integrator on a link that stalls and resumes is a wind-up hazard.  The closed-loop time constant is
// 1 / (kp * 1e-6) = 25000 blocks = 72.7 s, so convergence to a few percent takes minutes -- deliberately slower
// than any burst, because this trims PITCH.
#pragma once
#include <stdint.h>
typedef struct { int32_t target_x65536, filt_x65536, trim_ppm, kp_ppm_per_block_x256, clamp_ppm; } sink_servo_t;
static inline void servo_init(sink_servo_t *s, int32_t target_blocks) {
    s->target_x65536 = target_blocks * 65536; s->filt_x65536 = s->target_x65536; s->trim_ppm = 0;
    s->kp_ppm_per_block_x256 = 40 * 256;    /* 40 ppm per block of fill error, so the clamp below is reached at +-5 blocks */
    s->clamp_ppm = 200;
}
/* one audio block elapsed; fill = blocks currently in the ring (0..ring size); hold = link down (freeze the trim) */
static inline int32_t servo_step(sink_servo_t *s, int32_t fill, int hold) {
    if (hold) return s->trim_ppm;
    s->filt_x65536 += ((fill * 65536) - s->filt_x65536) / 344;                     /* EMA, tau = 344 blocks = 1 s */
    int32_t err_x65536 = s->filt_x65536 - s->target_x65536;                        /* + = ring filling: source faster than us -> speed up */
    int32_t t = (int32_t)(((int64_t)err_x65536 * s->kp_ppm_per_block_x256) / ((int64_t)65536 * 256));
    if (t > s->clamp_ppm) t = s->clamp_ppm;
    if (t < -s->clamp_ppm) t = -s->clamp_ppm;
    s->trim_ppm = t; return t;
}
static inline void servo_recentre(sink_servo_t *s) { s->filt_x65536 = s->target_x65536; }   /* a new START: drop the history, keep the trim */
