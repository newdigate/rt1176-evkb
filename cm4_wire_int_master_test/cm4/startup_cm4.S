/* Cortex-M4 startup for the Phase-2C interrupt/timing CM4 image.
 *
 * Same minimal reset handler as cm4_image_test (relocate VTOR to 0x1FFE0000,
 * reload MSP, enable FPU, copy .data, zero .bss, call main), but with a full
 * vector table: SysTick at exception 15, the LPI2C5 interrupt (external IRQ 36,
 * Phase 4.1) at vector index 16+36 = 52, and the MU interrupt (external IRQ 118)
 * at vector index 16+118 = 134. Unused vectors spin in Default_Handler.
 * SysTick_Handler, LPI2C5_IRQHandler, and MU_IRQHandler are C functions in
 * main_cm4.c; LPI2C5_IRQHandler is an empty placeholder as of Phase 4.1 Task 4
 * (the real ISR body + NVIC-enable land in Task 5).
 *
 * The table is 135 words (540 B); VTOR must cover it, so it sits at
 * ORIGIN(ITCM)=0x1FFE0000, which is 1 KB aligned. Public domain (N. Newdigate).
 */
    .syntax unified
    .cpu cortex-m4
    .thumb

    .section .isr_vector, "a"
    .align 2
    .global __isr_vector
__isr_vector:
    .word __StackTop            /* 0: initial MSP */
    .word Reset_Handler         /* 1: reset */
    .word Default_Handler       /* 2: NMI */
    .word Default_Handler       /* 3: HardFault */
    .word Default_Handler       /* 4: MemManage */
    .word Default_Handler       /* 5: BusFault */
    .word Default_Handler       /* 6: UsageFault */
    .word 0                     /* 7-10: reserved */
    .word 0
    .word 0
    .word 0
    .word Default_Handler       /* 11: SVCall */
    .word Default_Handler       /* 12: DebugMon */
    .word 0                     /* 13: reserved */
    .word Default_Handler       /* 14: PendSV */
    .word SysTick_Handler       /* 15: SysTick */
    /* external IRQs 0..117 -> vector index 16..133 */
    .rept 36                    /* IRQ 0..35 */
    .word Default_Handler
    .endr
    .word LPI2C5_IRQHandler     /* index 52: external IRQ 36 (LPI2C5) */
    .rept 81                    /* IRQ 37..117 */
    .word Default_Handler
    .endr
    .word MU_IRQHandler         /* 134: external IRQ 118 (MU) */

    .text
    .thumb_func
    .align 2
    .global Reset_Handler
Reset_Handler:
    cpsid   i

    ldr     r0, =0xE000ED08         /* SCB->VTOR = our ITCM table */
    ldr     r1, =__isr_vector
    str     r1, [r0]
    ldr     r2, [r1]
    msr     msp, r2

    ldr     r0, =0xE000ED88         /* SCB->CPACR: enable M4F FPU (CP10/CP11) */
    ldr     r1, [r0]
    orr     r1, r1, #(0xF << 20)
    str     r1, [r0]
    dsb
    isb

    ldr     r0, =__data_start__     /* copy .data ITCM-LMA -> DTCM-VMA */
    ldr     r1, =__data_end__
    ldr     r2, =__data_load__
1:  cmp     r0, r1
    ittt    lo
    ldrlo   r3, [r2], #4
    strlo   r3, [r0], #4
    blo     1b

    ldr     r0, =__bss_start__      /* zero .bss */
    ldr     r1, =__bss_end__
    movs    r3, #0
2:  cmp     r0, r1
    itt     lo
    strlo   r3, [r0], #4
    blo     2b

    bl      main
3:  b       3b

    .thumb_func
    .align 2
    .global Default_Handler
Default_Handler:
    b       Default_Handler
