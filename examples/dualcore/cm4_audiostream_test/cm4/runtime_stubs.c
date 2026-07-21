/* Minimal C++ runtime for the freestanding CM4 image world (-nostdlib).
 * Public domain (N. Newdigate). */
#include <stdint.h>

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

/* gcc may emit calls to these even in freestanding C++: */
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

/* Shared vector table (startup_cm4.S, plain cm4_wire_test copy) hard-
 * references these; unused here. */
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}
