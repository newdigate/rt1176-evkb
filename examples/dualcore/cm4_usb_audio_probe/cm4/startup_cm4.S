/* Cortex-M4 startup for the cm4_usb_audio_probe image (Phase 7.3).
 *
 * Byte-for-byte the cm4_usb_enum_probe (7.2) startup: the isochronous stream
 * this image drives needs no interrupt of its own.  The OUT ring is armed
 * ioc=false (usb_audio.cpp beginStreaming) precisely so it does not generate
 * ~1000 IRQ/s into a handler with no siTD work to do -- service() polls the
 * descriptors' status words instead.  Vector slot 151 is still load-bearing:
 * ENUMERATION is interrupt-driven, and streaming cannot start without it.
 *
 * Same reset handler as cm4_usb_irq_probe (relocate VTOR to 0x1FFE0000, reload
 * MSP, enable FPU, copy .data, zero .bss and .bss.dma), with one difference
 * that is the whole point of this image: vector index 151 (external IRQ 135,
 * USB OTG2, RM Table 4-2) names `usbhost_isr_entry` -- the real USBHost_t36
 * ISR -- instead of a local counting stub.
 *
 * ★ usbhost_isr_entry lives in USBHost_t36/usbhost_isr_entry.cpp and is
 * `extern "C"`, because USBHost::isr() is private and C++-mangled and this
 * table is a flat array of C function pointers.  The CM7 never needs it: it
 * installs isr() at run time with attachInterruptVector(), which is a
 * documented no-op in the CM4 shim (the table is static, linked at build time).
 *
 * It is NOT weak-aliased to Default_Handler here.  cm4_usb_irq_probe did that
 * so the image would link before the handler existed; doing it now would mean
 * a missing usbhost_isr_entry.cpp in SOURCES links CLEANLY and silently routes
 * every USB interrupt into a spin loop -- enumeration would simply never
 * progress, with nothing to read but a hang.  A hard reference makes that a
 * link error instead.
 *
 * The table is 152 words (608 B); VTOR must cover it, so it sits at
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
    .rept 76                    /* IRQ 0..75 */
    .word Default_Handler
    .endr
    .word SAI1_IRQHandler       /* index 92: external IRQ 76 (SAI1) */
    .rept 41                    /* IRQ 77..117 */
    .word Default_Handler
    .endr
    .word MU_IRQHandler         /* 134: external IRQ 118 (MU) */
    .rept 16                    /* IRQ 119..134 */
    .word Default_Handler
    .endr
    .word usbhost_isr_entry     /* 151: external IRQ 135 (USB OTG2) */

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

    /* Zero .bss.dma (DMAMEM, OCRAM2 @ 0x202C0000) -- same idiom, r3 still 0.
     * Separate loop because the section is NOLOAD in a different MEMORY region,
     * so it is not contiguous with .bss.  ★ This loop is why the window is
     * OCRAM2 and not OCRAM1: it runs at CM4 reset, i.e. after the CM7 has
     * booted and staged this image, so pointing it at OCRAM1 would zero the
     * front of the CM7's live DMAMEM.  See cm4.ld for the full note, and for
     * why TCM cannot be used at all.
     *
     * Unlike in cm4_usb_irq_probe this range is NOT empty: USBHost_t36's
     * memory.cpp puts its Device_t/Pipe_t/Transfer_t pools here, and every one
     * of them is a linked-list node the library expects zeroed at boot.  In
     * THIS image it is ~190 KB, because the whole USBAudioOut object (two
     * payload rings, two sample FIFOs) is DMAMEM as well -- so this loop is
     * also the longest-running thing in the reset path. */
    ldr     r0, =_sbss_dma
    ldr     r1, =_ebss_dma
4:  cmp     r0, r1
    itt     lo
    strlo   r3, [r0], #4
    blo     4b

    bl      main
3:  b       3b

    .thumb_func
    .align 2
    .global Default_Handler
Default_Handler:
    b       Default_Handler
