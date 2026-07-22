/* codec_wm8962.h - CM4-side WM8962 bring-up entry point (see codec_wm8962.c).
 * Public domain (N. Newdigate); the register program is MIT (control_wm8962.cpp). */
#ifndef CM4_CODEC_WM8962_H
#define CM4_CODEC_WM8962_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* Self-config LPI2C5 + run the full HW-verified WM8962 record/playback init
 * (44.1 kHz). Returns 1 iff every I2C transaction ACKed. The SAI TX clock must
 * already be running (MCLK present) before this is called. */
uint32_t codec_wm8962_init(void);

#if defined(__cplusplus)
}
#endif

#endif /* CM4_CODEC_WM8962_H */
