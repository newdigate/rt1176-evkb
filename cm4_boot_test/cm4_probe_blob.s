/*
 * CM4 probe blob for the RT1176 dual-core hardware-vs-QEMU test.
 * Position independent apart from the two vector words (CM4 view: ITCM
 * 0x1FFE0000).  Behavior:
 *   - sends 0xC0FFEE42 on MUB.TR0 at boot ("hello", resent after SW reset)
 *   - forever polls MUB.SR:
 *       GIP0 set  -> ack it (W1C) and send 0xD00DFEED on TR1
 *       RF3 set   -> read RR3, send value+1 on TR3 (echo)
 */
    .syntax unified
    .cpu cortex-m4
    .thumb

    .word 0x20020000            /* MSP: top of CM4 DTCM */
    .word 0x1FFE0009            /* reset: +8, thumb */
start:
    ldr r0, =0x40C4C000         /* MUB */
    ldr r1, =0xC0FFEE42
    str r1, [r0, #0x00]         /* TR0: hello */
poll:
    ldr r2, [r0, #0x20]         /* BSR */
    tst r2, #0x80000000         /* GIP0? */
    beq 1f
    mov r3, #0x80000000
    str r3, [r0, #0x20]         /* ack GIP0 (W1C) */
    ldr r3, =0xD00DFEED
    str r3, [r0, #0x04]         /* TR1: doorbell ack */
1:
    tst r2, #(1 << 24)          /* RF3? */
    beq poll
    ldr r3, [r0, #0x1C]         /* RR3 */
    adds r3, #1
    str r3, [r0, #0x0C]         /* TR3: echo + 1 */
    b poll
    .ltorg
