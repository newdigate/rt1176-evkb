/* Minimal CM4 heartbeat firmware for the Phase-2A gate.
 *
 * Proves the real-image startup machinery by reporting three canaries over the
 * MU B side (MUB @ 0x40C4C000, the CM4's side), each of which only reads
 * correct if a specific piece of startup worked on silicon:
 *   TR0 = data_canary (0xDA7A0001)  -> .data was copied ITCM-LMA -> DTCM-VMA
 *   TR1 = bss_canary + 0xB55        -> .bss was zero-initialised
 *   TR2 = sum of a DTCM stack array -> the stack works (0+1+4+9+..+49 = 0x8C)
 * then echoes RR3+1 forever so the CM7 can confirm the image keeps running and
 * the MU still round-trips.
 *
 * n=0 is the HIGH-order bit of each MU 4-bit field (RM ch.35), same as the A
 * side.  Public domain (author: Nicholas Newdigate).
 */
#include <stdint.h>

#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_RR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x10u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define SR_RF(n)   (1u << (27 - (n)))   /* receive register n full */
#define SR_TE(n)   (1u << (23 - (n)))   /* transmit register n empty */

volatile uint32_t data_canary = 0xDA7A0001u;   /* .data: proves LMA->VMA copy */
volatile uint32_t bss_canary;                   /* .bss: proves zero-init     */

static void mu_send(unsigned ch, uint32_t v)
{
    while (!(MUB_SR & SR_TE(ch))) {
    }
    MUB_TR(ch) = v;
}

int main(void)
{
    volatile uint32_t stack_arr[8];
    uint32_t sum = 0;

    for (unsigned i = 0; i < 8; i++) {
        stack_arr[i] = i * i;
    }
    for (unsigned i = 0; i < 8; i++) {
        sum += stack_arr[i];        /* 0+1+4+9+16+25+36+49 = 140 = 0x8C */
    }

    bss_canary += 0xB55u;           /* 0 (if zeroed) + 0xB55 = 0xB55 */

    mu_send(0, data_canary);        /* 0xDA7A0001 */
    mu_send(1, bss_canary);         /* 0x00000B55 */
    mu_send(2, sum);                /* 0x0000008C */

    for (;;) {
        if (MUB_SR & SR_RF(3)) {
            uint32_t v = MUB_RR(3);
            mu_send(3, v + 1);
        }
    }
}
