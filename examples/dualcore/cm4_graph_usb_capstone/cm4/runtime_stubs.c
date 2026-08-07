/* Minimal C++ runtime for the freestanding CM4 image world (-nostdlib).
 * Phase 5 (cm4_cpp_test) established this file; the USBHost_t36 image needs
 * more of it than the AudioStream one did.  Public domain (N. Newdigate). */
#include <stdint.h>
#include <stddef.h>

typedef void (*init_fn)(void);
extern init_fn __init_array_start[], __init_array_end[];
void cm4_run_ctors(void) {
    for (init_fn *f = __init_array_start; f < __init_array_end; f++) (*f)();
}

void __cxa_pure_virtual(void) { for (;;) {} }

/* Sized operator delete (mangled _ZdlPvj): a class with a virtual destructor
 * gets a deleting destructor (D0) that calls it, so the symbol is referenced
 * even though nothing here heap-allocates. Never called at runtime. */
void _ZdlPvj(void *p, unsigned int n) { (void)p; (void)n; }

/* abs(): analyze_peak.h's read() uses it, and the Arduino surface is expected
 * to provide it (the real core via wiring.h -> <stdlib.h>; cm4_shim declares
 * <stdlib.h> for the same reason).
 *
 * ★ It needs a REAL DEFINITION here, and the shim's comment on that line
 * ("GCC lowers abs(int) to __builtin_abs, so this stays a pure declaration with
 * no -nostdlib link dependency") does not hold in this image world:
 * teensy_add_cm4_image compiles -ffreestanding, which implies -fno-builtin for
 * everything except mem*, so abs() is emitted as a genuine call and the link
 * fails with `undefined reference to abs'.  Phase 6's cm4_audio_test hit this
 * first and fixed it the same way; this is that fix, not a new one. */
int abs(int v) { return v < 0 ? -v : v; }

/* gcc emits calls to these from freestanding C++ -- enumeration.cpp memsets
 * each new Device_t, and the compiler lowers small struct copies to memcpy. */
void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *p = d; const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}

/* Shared vector table (startup_cm4.S) hard-references these; unused here.
 * The TWO LIVE SLOTS are NOT among them, deliberately: index 151 names
 * usbhost_isr_entry (the real USBHost_t36 ISR, from the library) and index 60
 * names Software_IRQHandler (main_cm4.cpp's extern "C" wrapper around
 * AudioStream.cpp's C++-linkage software_isr).  Stubbing either here would
 * make a missing source link CLEANLY and then do nothing -- USB would never
 * enumerate, or the graph would never run, with no link error to say so. */
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}
void SAI1_IRQHandler(void) {}
