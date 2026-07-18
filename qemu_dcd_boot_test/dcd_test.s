/*
 * DCD boot test for the QEMU mimxrt1170-evk bootrom stub (boot-xip=on).
 *
 * A minimal flexspi_nor boot image that carries NXP's real evkbmimxrt1170
 * DCD (1208 bytes of IOMUXC/CCM/SEMC writes ending in the SDRAM Mode-Set
 * IP command).  The application's initial stack and test data live in
 * SDRAM, so it only runs if the ROM stub executed the DCD and the SEMC
 * gate opened:
 *   - vector[0] MSP = 0x80100000 (SDRAM)
 *   - fills/checks a pattern at 0x80000000 and uses push/pop on the stack
 *   - prints "DCD-SDRAM-OK" on LPUART1, or nothing if SDRAM is dead.
 *
 * Assemble: arm-none-eabi-gcc -nostdlib -mcpu=cortex-m7 -mthumb \
 *              -Wl,-Ttext=0x30000000 -o dcd_test.elf dcd_test.s
 */
    .syntax unified
    .cpu cortex-m7
    .thumb

    .section .text
    .global _start

    /* FCB at +0x400: only the 'FCFB' tag is validated. */
    .org 0x400
    .ascii "FCFB"

    /* IVT at +0x1000. */
    .org 0x1000
    .word 0x412000D1            /* header: tag 0xD1, len 0x20, ver 0x41 */
    .word _start + 1            /* entry (Thumb) */
    .word 0                     /* reserved1 */
    .word 0x30001100            /* dcd pointer */
    .word 0x30001020            /* boot_data pointer */
    .word 0x30001000            /* self */
    .word 0                     /* csf (unsigned) */
    .word 0                     /* reserved2 */
    /* boot_data at +0x1020 */
    .word 0x30000000            /* image start */
    .word 0x3000                /* image size */
    .word 0                     /* plugin */

    /* No XMCD: +0x1040 stays zero. */

    /* NXP's real DCD at +0x1100. */
    .org 0x1100
    .incbin "nxp_dcd.bin"

    /* Application vector table at +0x2000 (the stub sets MSP/VTOR here). */
    .org 0x2000
vectors:
    .word 0x80100000            /* initial MSP: 1 MiB into SDRAM */
    .word _start + 1

    .thumb_func
_start:
    ldr     r7, =0x4007C000     /* LPUART1 */

    /* SDRAM data test: pattern fill + readback at 0x80000000. */
    ldr     r0, =0x80000000
    ldr     r1, =0xDCD5EED5
    movs    r2, #16
1:
    str     r1, [r0], #4
    adds    r1, #1
    subs    r2, #1
    bne     1b
    ldr     r0, =0x80000000
    ldr     r1, =0xDCD5EED5
    movs    r2, #16
2:
    ldr     r3, [r0], #4
    cmp     r3, r1
    bne     fail
    adds    r1, #1
    subs    r2, #1
    bne     2b

    /* SDRAM stack test: push/pop through the SDRAM MSP. */
    ldr     r4, =0xCAFEF00D
    push    {r4}
    pop     {r5}
    cmp     r4, r5
    bne     fail

    adr     r0, msg_ok
    bl      puts
park:
    wfi
    b       park
fail:
    adr     r0, msg_fail
    bl      puts
    b       park

    .thumb_func
puts:
    ldrb    r1, [r0], #1
    cbz     r1, 9f
    str     r1, [r7, #0x1C]     /* LPUART DATA */
    b       puts
9:  bx      lr

    .ltorg
    .align 2
msg_ok:   .asciz "DCD-SDRAM-OK\n"
msg_fail: .asciz "DCD-SDRAM-FAIL\n"
