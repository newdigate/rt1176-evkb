/* Cortex-M4 startup for the cm4_audio_test capstone image.
 *
 * Same minimal reset handler as cm4_image_test (relocate VTOR to 0x1FFE0000,
 * reload MSP, enable FPU, copy .data, zero .bss, call main), with a full
 * vector table carrying BOTH interrupt lines the CM4-owned audio pipeline
 * needs:
 *   - the audio-graph software interrupt (external IRQ 44 -- the repurposed
 *     CAN1 IRQ_SOFTWARE slot, same convention as the CM7 world) at vector
 *     index 16+44 = 60 (Software_IRQHandler, from cm4_audiostream_test);
 *   - the SAI1 FIFO-request interrupt (external IRQ 76, RM Table 4-2 --
 *     HW-proven to reach the CM4 NVIC by cm4_sai_irq_probe) at vector index
 *     16+76 = 92 (SAI1_IRQHandler);
 *   - the MU interrupt (external IRQ 118) at vector index 16+118 = 134.
 * Unused vectors spin in Default_Handler. Software_IRQHandler and
 * SAI1_IRQHandler are extern "C" wrappers in main_cm4.cpp (around
 * AudioStream.cpp's C++-linkage software_isr and sai1176.c's
 * sai1176_isr_dispatch respectively -- a static table cannot name a mangled
 * or indirectly-hooked symbol portably); SysTick_Handler and MU_IRQHandler
 * are stubs in runtime_stubs.c.
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
    .rept 44                    /* IRQ 0..43 */
    .word Default_Handler
    .endr
    .word Software_IRQHandler   /* index 60: external IRQ 44 (IRQ_SOFTWARE) */
    .rept 31                    /* IRQ 45..75 */
    .word Default_Handler
    .endr
    .word SAI1_IRQHandler       /* index 92: external IRQ 76 (SAI1) */
    .rept 41                    /* IRQ 77..117 */
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
