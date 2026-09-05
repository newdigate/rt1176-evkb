/* icache_bench_hw -- CM7 L1 instruction-cache microbench for the MIMXRT1170-EVKB
 * (NEW-36, 2026-09-05).  SILICON-ONLY, no QEMU gate: qemu2 masks SCB_CCR.IC out of
 * every CCR write and NOPs ICIALLU, so under QEMU every row honestly reads
 * icache=off and the cycle counts mean nothing.  It still runs there, and the
 * wit_match / sbc_crc_match lines are a real check of the bench's own logic.
 *
 * What it measures, in ONE boot, with the DWT cycle counter and IRQs masked
 * around each timed rep (millis()/micros() are DWT-based in this core, so a
 * deferred SysTick costs nothing):
 *   wl=calls  64 three-instruction functions in an unrolled direct chain --
 *             fetch/branch-bound, the "5-30 us per small call" shape NEW-33
 *             measured on flash-resident code.  Reported per call.
 *   wl=crc    table-driven CRC32 over a 4 KB DTCM buffer, table in DTCM, so the
 *             data side is TCM and the row measures instruction fetch.  Per byte.
 *   wl=sbc    the real M2Radio Sbc encoder, one 128-sample stereo frame per unit,
 *             acid_box's negotiated config (44.1 kHz, joint stereo, 16 blocks x 8
 *             subbands, loudness, bitpool 53 -> 119-byte frames).  The default
 *             build routes Sbc.cpp.obj to FLASH (CMakeLists, acid_box's derived-ld
 *             mechanism); -DICACHE_BENCH_SBC_ITCM=ON leaves it in ITCM for the
 *             reference cell.  Per frame, plus realtime_x = 2.902 ms / us per frame.
 * calls and crc are instantiated TWICE from one macro: the ITCM twin (the core
 * script's default .text placement) and the flash twin (.progmem.icb_code, which
 * imxrt1176.ld sends to .text.progmem in FLASH).  Every function under test is
 * noipa, so no twin is inlined, cloned or ICF-folded into the other.  place= is
 * DERIVED from the function's address, never asserted.
 *
 * Per cell: arm_icache_disable() -> K reps (min, median) -> arm_icache_enable()
 * (which invalidates first, so the next rep is COLD) -> cold rep -> K warm reps.
 * Every row prints the CCR.IC state it actually measured under, and a witness
 * (the workload's result, or the CRC of the last SBC frame) that must agree
 * between the off and on halves -- a cache that changed a result would be the
 * one finding worth more than any speedup.
 */
#include "Arduino.h"
#include "Sbc.h"
#include <math.h>      /* sinf/cosf for the SBC input */
#include <stddef.h>    /* ptrdiff_t in the pointer-to-member union */

#define CONSOLE Serial1

#define ICB_K        8      /* reps per half-cell */
#define ICB_N_CALLS  64     /* chain passes per rep (64 calls each) */
#define ICB_N_CRC    32     /* 4 KB passes per rep */
#define ICB_N_SBC    16     /* frames per rep */

#define ICB_NOIPA __attribute__((noipa))                       /* implies noinline, noclone, no_icf */
#define ICB_FLASH __attribute__((section(".progmem.icb_code")))
#define ICB_ITCM  /* default placement: imxrt1176.ld routes .text* to ITCM */

static volatile uint32_t icb_seed = 0x12345678u;   /* volatile source: nothing folds */
static volatile uint32_t icb_sink;                 /* volatile sink: nothing is dead */

/* ---- wl=calls: 64 tiny functions, direct unrolled chain ------------------- */
#define ICB_FN(S, A, N) \
    ICB_NOIPA A static uint32_t icb_##S##_f##N(uint32_t x) { return (x * 3u) ^ (N + 1u); }
#define ICB_CALL(S, A, N) x = icb_##S##_f##N(x);
#define ICB_X64(M, S, A) \
    M(S,A,0)  M(S,A,1)  M(S,A,2)  M(S,A,3)  M(S,A,4)  M(S,A,5)  M(S,A,6)  M(S,A,7)  \
    M(S,A,8)  M(S,A,9)  M(S,A,10) M(S,A,11) M(S,A,12) M(S,A,13) M(S,A,14) M(S,A,15) \
    M(S,A,16) M(S,A,17) M(S,A,18) M(S,A,19) M(S,A,20) M(S,A,21) M(S,A,22) M(S,A,23) \
    M(S,A,24) M(S,A,25) M(S,A,26) M(S,A,27) M(S,A,28) M(S,A,29) M(S,A,30) M(S,A,31) \
    M(S,A,32) M(S,A,33) M(S,A,34) M(S,A,35) M(S,A,36) M(S,A,37) M(S,A,38) M(S,A,39) \
    M(S,A,40) M(S,A,41) M(S,A,42) M(S,A,43) M(S,A,44) M(S,A,45) M(S,A,46) M(S,A,47) \
    M(S,A,48) M(S,A,49) M(S,A,50) M(S,A,51) M(S,A,52) M(S,A,53) M(S,A,54) M(S,A,55) \
    M(S,A,56) M(S,A,57) M(S,A,58) M(S,A,59) M(S,A,60) M(S,A,61) M(S,A,62) M(S,A,63)
#define ICB_DEFINE_CALLS(S, A) \
    ICB_X64(ICB_FN, S, A) \
    ICB_NOIPA A static uint32_t icb_##S##_calls_rep(void) { \
        uint32_t x = icb_seed; \
        for (uint32_t p = 0; p < ICB_N_CALLS; p++) { ICB_X64(ICB_CALL, S, A) } \
        return x; }
ICB_DEFINE_CALLS(itcm, ICB_ITCM)
ICB_DEFINE_CALLS(flash, ICB_FLASH)

/* ---- wl=crc: table-driven CRC32, table and buffer in DTCM ----------------- */
static uint32_t icb_crc_table[256];   /* .bss (DTCM): built at runtime so it is NOT .rodata in flash */
static uint8_t  icb_crc_buf[4096];    /* .bss (DTCM) */
#define ICB_DEFINE_CRC(S, A) \
    ICB_NOIPA A static uint32_t icb_##S##_crc32(uint32_t c, const uint8_t *p, uint32_t n) { \
        c = ~c; \
        while (n--) c = icb_crc_table[(c ^ *p++) & 0xFFu] ^ (c >> 8); \
        return ~c; } \
    ICB_NOIPA A static uint32_t icb_##S##_crc_rep(void) { \
        uint32_t c = icb_seed; \
        for (uint32_t p = 0; p < ICB_N_CRC; p++) c = icb_##S##_crc32(c, icb_crc_buf, sizeof icb_crc_buf); \
        return c; }
ICB_DEFINE_CRC(itcm, ICB_ITCM)
ICB_DEFINE_CRC(flash, ICB_FLASH)

/* ---- wl=sbc: the real encoder, wherever the linker put Sbc.cpp.obj -------- */
static Sbc      icb_sbc;
static int16_t  icb_pcm_l[128], icb_pcm_r[128];
static uint8_t  icb_sbc_out[ICB_N_SBC * 128];   /* one 128-B slot per frame: 119 B at bitpool 53 (Sbc::frameLength is
                                                  * fixed by icb_sbc_params; a params edit that exceeds 128 B/frame must
                                                  * grow the slot).  The unwritten tail of each slot is .bss zero, so the
                                                  * witness below covers the CONCATENATED output of every frame in a rep. */
static uint16_t icb_sbc_flen;
static Sbc::Params icb_sbc_params = { Sbc::RATE_44100, Sbc::JOINT_STEREO, 16, 8, Sbc::LOUDNESS, 53 };
ICB_NOIPA static uint32_t icb_sbc_rep(void)   /* ITCM wrapper; the cost under test is Sbc::encode */
{
    uint16_t n = 0;
    for (uint32_t f = 0; f < ICB_N_SBC; f++) n = icb_sbc.encode(icb_pcm_l, icb_pcm_r, icb_sbc_out + f * 128u);
    icb_sbc_flen = n;
    return n;
}
static void icb_sbc_reset(void) { icb_sbc.begin(icb_sbc_params); }   /* fresh state per rep => identical frames */
static uint32_t icb_sbc_witness(void) { return icb_itcm_crc32(0, icb_sbc_out, sizeof icb_sbc_out); }
static const void *icb_sbc_encode_addr(void)
{
    /* Non-virtual member: under the ARM C++ ABI (§3.2.1) the first word of a
     * pointer-to-member-function holds the THUMB-TAGGED code address (bit 0 set)
     * and the adjustment word carries the virtual flag -- so the `& ~1u` that
     * icb_place()/icb_row() apply is load-bearing, not defensive.  Read it
     * through a union. */
    union { uint16_t (Sbc::*mf)(const int16_t *, const int16_t *, uint8_t *); struct { const void *p; ptrdiff_t adj; } r; } u;
    u.mf = &Sbc::encode;
    return u.r.p;
}

/* ---- harness ---------------------------------------------------------------- */
static uint32_t icb_time(uint32_t (*rep)(void))
{
    __disable_irq();
    uint32_t t0 = ARM_DWT_CYCCNT;
    uint32_t r = rep();
    uint32_t dt = ARM_DWT_CYCCNT - t0;
    __enable_irq();
    icb_sink = r;
    return dt;
}
static void icb_sort(uint32_t *v, int n)
{
    for (int i = 1; i < n; i++) { uint32_t x = v[i]; int j = i; while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; } v[j] = x; }
}
static bool icb_ic(void) { return (SCB_CCR & SCB_CCR_IC) != 0; }
static uint32_t icb_us(uint32_t cyc) { return (uint32_t)(((uint64_t)cyc * 1000000u) / F_CPU_ACTUAL); }
static const char *icb_place(const void *fn)
{
    uintptr_t a = (uintptr_t)fn & ~(uintptr_t)1u;            /* strip the Thumb bit */
    if (a < 0x00200000u) return "itcm";                       /* TCM window, below the boot ROM */
    if (a >= 0x30000000u && a < 0x31000000u) return "flash";  /* FlexSPI1 XIP */
    return "other";
}
static uint32_t icb_ratio10(uint32_t a, uint32_t b) { return b ? (uint32_t)(((uint64_t)a * 10u + b / 2u) / b) : 0; }   /* a/b in tenths; 64-bit: an uncached SBC rep is ~50 M cycles */

struct IcbHalf { bool ic; uint32_t min, med, cold, wit; };
struct IcbCell { IcbHalf off, on; };

static IcbHalf icb_half(uint32_t (*rep)(void), void (*reset)(void), uint32_t (*witness)(void), bool with_cold)
{
    IcbHalf h; uint32_t r[ICB_K];
    h.ic = icb_ic();                                           /* the state this half measures UNDER */
    h.cold = 0;
    if (with_cold) {
        if (reset) reset();
        h.cold = icb_time(rep);
        (void)micros();   /* outside the window: services cycles64()'s 4.3 s CYCCNT-wrap accumulator */
    }
    for (int k = 0; k < ICB_K; k++) {
        if (reset) reset();
        r[k] = icb_time(rep);
        (void)micros();   /* outside the window: services cycles64()'s 4.3 s CYCCNT-wrap accumulator */
    }
    h.wit = witness ? witness() : icb_sink;
    icb_sort(r, ICB_K);
    h.min = r[0];
    h.med = (r[ICB_K / 2 - 1] + r[ICB_K / 2]) / 2u;
    return h;
}
static IcbCell icb_cell(uint32_t (*rep)(void), void (*reset)(void), uint32_t (*witness)(void))
{
    IcbCell c;
    arm_icache_disable();
    c.off = icb_half(rep, reset, witness, false);
    arm_icache_enable();                                       /* invalidates first: the next rep is cold */
    c.on = icb_half(rep, reset, witness, true);
    return c;
}
/* One row.  Two printf calls on purpose: Print::printf clamps at 128 bytes. */
static void icb_row(const char *wl, const void *fn, const IcbHalf &h, const char *unit, uint32_t units)
{
    CONSOLE.printf("icache_bench wl=%s place=%s addr=0x%08lx icache=%s ",
                   wl, icb_place(fn), (unsigned long)((uintptr_t)fn & ~(uintptr_t)1u), h.ic ? "on" : "off");
    if (h.cold) CONSOLE.printf("cold_us/rep=%lu ", (unsigned long)icb_us(h.cold));
    uint32_t mt = (uint32_t)(((uint64_t)h.min * 10u) / units), dt = (uint32_t)(((uint64_t)h.med * 10u) / units);
    CONSOLE.printf("min_cyc/%s=%lu.%lu med_cyc/%s=%lu.%lu us/rep=%lu wit=0x%08lx\n",
                   unit, (unsigned long)(mt / 10u), (unsigned long)(mt % 10u),
                   unit, (unsigned long)(dt / 10u), (unsigned long)(dt % 10u),
                   (unsigned long)icb_us(h.min), (unsigned long)h.wit);
}
static void icb_pair(const char *wl, const void *fn, const IcbCell &c, const char *unit, uint32_t units)
{
    icb_row(wl, fn, c.off, unit, units);
    icb_row(wl, fn, c.on, unit, units);
    CONSOLE.printf("icache_bench wl=%s place=%s wit_match=%d off/on=%lu.%lux\n", wl, icb_place(fn),
                   c.off.wit == c.on.wit ? 1 : 0,
                   (unsigned long)(icb_ratio10(c.off.min, c.on.min) / 10u), (unsigned long)(icb_ratio10(c.off.min, c.on.min) % 10u));
}

static void icb_header(void)
{
    uint32_t clidr = SCB_ID_CLIDR;
    SCB_ID_CSSELR = 1u;                                        /* InD=1: instruction cache, level 1 */
    __asm__ volatile("dsb" ::: "memory"); __asm__ volatile("isb" ::: "memory");
    uint32_t cc = SCB_ID_CCSIDR;
    SCB_ID_CSSELR = 0u; __asm__ volatile("dsb" ::: "memory"); __asm__ volatile("isb" ::: "memory");   /* leave the selector as we found it */
    uint32_t line_b = 1u << ((cc & 7u) + 4u);                  /* LineSize = log2(words) - 2 */
    uint32_t ways = ((cc >> 3) & 0x3FFu) + 1u;
    uint32_t sets = ((cc >> 13) & 0x7FFFu) + 1u;
    CONSOLE.printf("icache_bench ccr_reset=0x%08lx ccr_now=0x%08lx clidr=0x%08lx ccsidr_i=0x%08lx ",
                   (unsigned long)startup_ccr_at_reset, (unsigned long)SCB_CCR, (unsigned long)clidr, (unsigned long)cc);
    CONSOLE.printf("isize_kb=%lu ways=%lu line_b=%lu hz=%lu\n",
                   (unsigned long)(sets * ways * line_b / 1024u), (unsigned long)ways, (unsigned long)line_b, (unsigned long)F_CPU_ACTUAL);
}

void setup()
{
    CONSOLE.begin(115200);
    delay(50);
    CONSOLE.println("ICACHE-BENCH v1");
    icb_header();

    /* CRC table + buffer (DTCM), deterministic LCG fill. */
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        icb_crc_table[i] = c;
    }
    uint32_t lcg = 0x2545F491u;
    for (uint32_t i = 0; i < sizeof icb_crc_buf; i++) { lcg = lcg * 1664525u + 1013904223u; icb_crc_buf[i] = (uint8_t)(lcg >> 24); }

    /* SBC input: 1 kHz at about -3 dBFS plus a 3-bit LCG dither so the bit
     * allocation is not degenerate; the right channel 90 degrees behind. */
    for (int i = 0; i < 128; i++) {
        lcg = lcg * 1664525u + 1013904223u;
        float ph = 2.0f * 3.14159265f * 1000.0f * (float)i / 44100.0f;
        icb_pcm_l[i] = (int16_t)(23000.0f * sinf(ph)) + (int16_t)((lcg >> 28) & 7u);
        icb_pcm_r[i] = (int16_t)(23000.0f * cosf(ph)) + (int16_t)((lcg >> 24) & 7u);
    }

    /* calls: ITCM twin (control: off == on, TCM is never cached) then the flash twin */
    IcbCell ci = icb_cell(icb_itcm_calls_rep, 0, 0);
    icb_pair("calls", (const void *)icb_itcm_calls_rep, ci, "call", ICB_N_CALLS * 64u);
    IcbCell cf = icb_cell(icb_flash_calls_rep, 0, 0);
    icb_pair("calls", (const void *)icb_flash_calls_rep, cf, "call", ICB_N_CALLS * 64u);

    /* crc */
    IcbCell ri = icb_cell(icb_itcm_crc_rep, 0, 0);
    icb_pair("crc", (const void *)icb_itcm_crc_rep, ri, "byte", ICB_N_CRC * (uint32_t)sizeof icb_crc_buf);
    IcbCell rf = icb_cell(icb_flash_crc_rep, 0, 0);
    icb_pair("crc", (const void *)icb_flash_crc_rep, rf, "byte", ICB_N_CRC * (uint32_t)sizeof icb_crc_buf);

    /* sbc: placement is per build (see CMakeLists); the address decides the label */
    const void *enc = icb_sbc_encode_addr();
    IcbCell s = icb_cell(icb_sbc_rep, icb_sbc_reset, icb_sbc_witness);
    icb_pair("sbc", enc, s, "frame", ICB_N_SBC);
    uint32_t us_off = icb_us(s.off.min) / ICB_N_SBC, us_on = icb_us(s.on.min) / ICB_N_SBC;
    CONSOLE.printf("icache_bench wl=sbc place=%s flen=%u us/frame_off=%lu us/frame_on=%lu ",
                   icb_place(enc), (unsigned)icb_sbc_flen, (unsigned long)us_off, (unsigned long)us_on);
    int sbc_crc_match = (s.off.wit == s.on.wit) && (icb_sbc_flen != 0);
    CONSOLE.printf("realtime_x_off=%lu.%lu realtime_x_on=%lu.%lu sbc_crc=0x%08lx sbc_crc_match=%d\n",
                   (unsigned long)(icb_ratio10(2902u, us_off) / 10u), (unsigned long)(icb_ratio10(2902u, us_off) % 10u),
                   (unsigned long)(icb_ratio10(2902u, us_on) / 10u),  (unsigned long)(icb_ratio10(2902u, us_on) % 10u),
                   (unsigned long)s.on.wit, sbc_crc_match);

    /* summary: flash off/on speedups, and how close cached flash gets to ITCM */
    CONSOLE.printf("icache_bench summary calls=%lu.%lux crc=%lu.%lux sbc=%lu.%lux ",
                   (unsigned long)(icb_ratio10(cf.off.min, cf.on.min) / 10u), (unsigned long)(icb_ratio10(cf.off.min, cf.on.min) % 10u),
                   (unsigned long)(icb_ratio10(rf.off.min, rf.on.min) / 10u), (unsigned long)(icb_ratio10(rf.off.min, rf.on.min) % 10u),
                   (unsigned long)(icb_ratio10(s.off.min, s.on.min) / 10u),   (unsigned long)(icb_ratio10(s.off.min, s.on.min) % 10u));
    CONSOLE.printf("flash_on_vs_itcm: calls=%lu.%lux crc=%lu.%lux\n",
                   (unsigned long)(icb_ratio10(cf.on.min, ci.on.min) / 10u), (unsigned long)(icb_ratio10(cf.on.min, ci.on.min) % 10u),
                   (unsigned long)(icb_ratio10(rf.on.min, ri.on.min) / 10u), (unsigned long)(icb_ratio10(rf.on.min, ri.on.min) % 10u));

    arm_icache_enable();                                       /* leave the core in its default state */
    CONSOLE.println("ICACHE-BENCH done");
}

void loop()
{
    static uint32_t n = 0;
    delay(1000);
    CONSOLE.printf("hb n=%lu icache=%s\n", (unsigned long)++n, icb_ic() ? "on" : "off");
}
