/* cm4_cpp_test CM4 firmware: the first C++ CM4 image. Proves, via
 * MU-reported values, that the teensy_add_cm4_image g++ path gives a working
 * freestanding C++ runtime:
 *   (a) static constructors run (cm4_run_ctors walks .init_array):
 *       g_proof.magic == 0xCAFEC201 only if CtorProof's ctor executed;
 *   (b) virtual dispatch works (vtable in ITCM .rodata, .data pointer
 *       relocated by startup): g_base->id() == 0xCAFEC202 only if the
 *       Derived override is reached through the Base pointer;
 *   (c) the freestanding runtime stubs (runtime_stubs.c) link.
 * Reported over MU channel 0, in order: ctor, virt, done=0xD0DE0002.
 *
 * NOTE: the shared startup_cm4.S (plain cm4_wire_test copy) enters `main`
 * with PRIMASK set — irrelevant here, no interrupts are used.
 * Public domain (N. Newdigate). */
#include <stdint.h>
extern "C" {
#include "mu_report.h"
void cm4_run_ctors(void);
}

struct Base { virtual uint32_t id() const { return 0xDEAD0000u; } virtual ~Base() {} };
struct Derived : Base { uint32_t id() const override { return 0xCAFEC202u; } };

struct CtorProof { uint32_t magic; CtorProof() : magic(0xCAFEC201u) {} };
static CtorProof g_proof;
static Derived  g_derived;
static Base    *g_base = &g_derived;

extern "C" int main(void) {
    cm4_run_ctors();
    mu_send(g_proof.magic);      // 0xCAFEC201 iff the ctor ran
    mu_send(g_base->id());       // 0xCAFEC202 iff vtables work
    mu_send(0xD0DE0002u);
    for (;;) { __asm volatile ("wfi"); }
}
