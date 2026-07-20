/* Minimal Cortex-M4 startup for a TCM-resident RT1176 CM4 image.
 *
 * Clean-room against the ARMv7-M architecture + the CM4 boot facts (RM ch.3):
 * the CM7 boots us with VTOR = the system backdoor 0x20200000; we relocate
 * VTOR to our own ITCM table (0x1FFE0000), reload MSP, enable the M4F FPU,
 * copy .data (ITCM LMA -> DTCM VMA), zero .bss, and call main().  No SystemInit
 * (the CM7/boot ROM already brought up clocks, power and the CM4 TCM).
 *
 * Public domain (author: Nicholas Newdigate).
 */
    .syntax unified
    .cpu cortex-m4
    .thumb

/* ---- vector table (ITCM 0x1FFE0000) ---- */
    .section .isr_vector, "a"
    .align 2
    .global __isr_vector
__isr_vector:
    .word __StackTop            /* 0x00: initial MSP */
    .word Reset_Handler         /* 0x04: reset */
    .word Default_Handler       /* NMI */
    .word Default_Handler       /* HardFault */
    .word Default_Handler       /* MemManage */
    .word Default_Handler       /* BusFault */
    .word Default_Handler       /* UsageFault */
    .word 0
    .word 0
    .word 0
    .word 0
    .word Default_Handler       /* SVCall */
    .word Default_Handler       /* DebugMon */
    .word 0
    .word Default_Handler       /* PendSV */
    .word Default_Handler       /* SysTick */
    /* External IRQs default to Default_Handler via the NVIC reset state; this
     * minimal image takes no interrupts, so no further entries are needed. */

/* ---- reset handler ---- */
    .text
    .thumb_func
    .align 2
    .global Reset_Handler
Reset_Handler:
    cpsid   i                       /* mask interrupts during bring-up */

    /* Relocate VTOR to our ITCM vector table + reload MSP from vector[0]. */
    ldr     r0, =0xE000ED08         /* SCB->VTOR */
    ldr     r1, =__isr_vector
    str     r1, [r0]
    ldr     r2, [r1]
    msr     msp, r2

    /* Enable the M4F FPU: CPACR CP10/CP11 full access (compiled hard-float). */
    ldr     r0, =0xE000ED88         /* SCB->CPACR */
    ldr     r1, [r0]
    orr     r1, r1, #(0xF << 20)
    str     r1, [r0]
    dsb
    isb

    /* Copy .data: [__data_start__, __data_end__) <- __data_load__ (ITCM). */
    ldr     r0, =__data_start__
    ldr     r1, =__data_end__
    ldr     r2, =__data_load__
1:  cmp     r0, r1
    ittt    lo
    ldrlo   r3, [r2], #4
    strlo   r3, [r0], #4
    blo     1b

    /* Zero .bss: [__bss_start__, __bss_end__). */
    ldr     r0, =__bss_start__
    ldr     r1, =__bss_end__
    movs    r3, #0
2:  cmp     r0, r1
    itt     lo
    strlo   r3, [r0], #4
    blo     2b

    bl      main
3:  b       3b                      /* main() should not return */

    .thumb_func
    .align 2
    .global Default_Handler
Default_Handler:
    b       Default_Handler
