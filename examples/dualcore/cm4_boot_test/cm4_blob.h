/* cm4_blob.h - a tiny position-independent CM4 firmware image, embedded as a
 * const uint32_t[] for the CM7 to stage at 0x20200000 and boot.
 *
 * This is the HW-verified probe blob from evkb/dualcore_mu_test (assembled
 * from cm4_probe_blob.s alongside; CM4 view: vectors at ITCM 0x1FFE0000).
 * Behavior once running on the CM4:
 *   - sends 0xC0FFEE42 on MUB.TR0 at boot ("hello", resent after a SW reset);
 *   - polls MUB.SR forever:
 *       GIP0 set -> ack it (W1C) and send 0xD00DFEED on TR1 (doorbell ack);
 *       RF3  set -> read RR3, send value+1 on TR3 (echo).
 *
 * The Arduino single-binary equivalent of the NXP two-project .incbin scheme:
 * a pre-assembled blob as a C array.  Public domain (author: N. Newdigate).
 */
#ifndef cm4_blob_h
#define cm4_blob_h

#include <stdint.h>

static const uint32_t cm4_blob[] = {
    0x20020000, 0x1FFE0009, 0x490A4809, 0x6A026001,
    0x4F00F012, 0xF04FD004, 0x62034300, 0x60434B06,
    0x7F80F012, 0x69C3D0F3, 0x60C33301, 0x0000E7EF,
    0x40C4C000, 0xC0FFEE42, 0xD00DFEED,
};

#endif /* cm4_blob_h */
