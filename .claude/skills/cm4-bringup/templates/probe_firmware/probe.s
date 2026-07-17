/*
 * RT1176 register-probe template (CM7, flexspi_nor boot-header image).
 *
 * Prints deterministic `token=HEXVALUE` lines on LPUART1 so a QEMU
 * transcript and an EVKB transcript diff directly.  Copy, rename, and
 * replace the EXAMPLE PROBES section.  Same ELF runs on both sides:
 *   QEMU:  -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel …
 *   EVKB:  LinkServer flash … load probe.elf   (+ clean_boot.scp for an
 *          uncontaminated run; see references/silicon-truth-loop.md)
 *
 * Build: arm-none-eabi-gcc -nostdlib -mcpu=cortex-m7 -mthumb \
 *            -Wl,-Ttext=0x30000000 -o probe.elf probe.s
 */
    .syntax unified
    .cpu cortex-m7
    .thumb

    .section .text
    .global _start

    /* FCB at +0x400: the boot-ROM stub validates only the tag. */
    .org 0x400
    .ascii "FCFB"

    /* IVT at +0x1000. */
    .org 0x1000
    .word 0x412000D1            /* header: tag 0xD1, len 0x20, ver 0x41 */
    .word _start + 1            /* entry (Thumb) */
    .word 0                     /* reserved1 */
    .word 0                     /* dcd pointer (none) */
    .word 0x30001020            /* boot_data pointer */
    .word 0x30001000            /* self */
    .word 0                     /* csf (unsigned) */
    .word 0                     /* reserved2 */
    .word 0x30000000            /* boot_data.start */
    .word 0x3000                /* boot_data.size */
    .word 0                     /* boot_data.plugin */
    /* No XMCD: +0x1040 stays zero. */

    /* Application vector table at +0x2000 (stub sets MSP/VTOR here). */
    .org 0x2000
vectors:
    .word 0x20040000            /* MSP: top of the 256K fuse-default DTCM */
    .word _start + 1

    .thumb_func
_start:
    ldr     r7, =0x4007C000     /* LPUART1 (QEMU TX needs no init) */

    adr     r0, msg_start
    bl      puts

    /* ---- EXAMPLE PROBES: replace from here ---- */
    ldr     r4, =0x40C48000     /* MUA */
    adr     r0, tok_sr0
    ldr     r1, [r4, #0x20]     /* MUA.SR */
    bl      phex                /* expect 00F00200 (bit9 quirk, TEs set) */

    ldr     r4, =0x40C04000     /* SRC */
    adr     r0, tok_stat
    ldr     r1, [r4, #0x290]    /* STAT_M4CORE */
    bl      phex                /* expect 00000001 while the CM4 is held */
    /* ---- to here ---- */

    adr     r0, msg_done
    bl      puts
park:
    wfi
    b       park

    /* puts: r0 = NUL-terminated string (clobbers r0,r1) */
    .thumb_func
puts:
    ldrb    r1, [r0], #1
    cbz     r1, 9f
    str     r1, [r7, #0x1C]     /* LPUART DATA */
    b       puts
9:  bx      lr

    /* phex: r0 = token string, r1 = 32-bit value  ->  "token=XXXXXXXX\n"
       (clobbers r0-r3, r5, r6, lr-safe via r6) */
    .thumb_func
phex:
    mov     r5, r1
    mov     r6, lr
    bl      puts
    movs    r0, #61             /* '=' */
    str     r0, [r7, #0x1C]
    movs    r2, #8
1:  lsrs    r3, r5, #28
    cmp     r3, #10
    ite     lt
    addlt   r3, #48             /* '0' */
    addge   r3, #55             /* 'A' - 10 */
    str     r3, [r7, #0x1C]
    lsls    r5, r5, #4
    subs    r2, #1
    bne     1b
    movs    r0, #13             /* '\r' */
    str     r0, [r7, #0x1C]
    movs    r0, #10             /* '\n' */
    str     r0, [r7, #0x1C]
    bx      r6

    .ltorg
    .align 2
msg_start: .asciz "PROBE-START\r\n"
msg_done:  .asciz "PROBE-DONE\r\n"
tok_sr0:   .asciz "sr0"
tok_stat:  .asciz "stat_m4"
