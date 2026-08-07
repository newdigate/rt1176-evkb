/* Linker script for a TCM-resident CM4 image on the i.MX RT1176.
 *
 * The CM4's private view: code/vectors in ITCM at 0x1FFE0000, data in DTCM at
 * 0x20000000 (128 K each, RM Table 3-2 / SDK MIMXRT1176xxxxx_cm4_ram.ld).
 * The CM7 stages the objcopy'd image into the OCRAM-M4 backdoor at 0x20200000
 * (which aliases this ITCM) and boots with VTOR = 0x20200000; the CM4 then
 * relocates VTOR to its own 0x1FFE0000 table.  .data lives in DTCM but its
 * initialisers (LMA) sit in ITCM, appended after .text, so the whole loadable
 * image is contiguous from 0x1FFE0000 and objcopy -O binary yields exactly the
 * bytes to stage. Public domain (author: Nicholas Newdigate).
 *
 * OCRAM2 is added for DMAMEM (.bss.dma): the EHCI controller is its own bus
 * master and cannot reach either TCM, so USB host descriptors and payload rings
 * have to live in on-chip RAM that is on the system bus.  RM Table 3-2 (CM4
 * map) and Table 3-1 (CM7 map) agree on the addresses:
 *
 *     0x2020_0000 .. 0x2023_FFFF   256K   OCRAM M4  <-- NOT this; see below
 *     0x2024_0000 .. 0x202B_FFFF   512K   OCRAM1    <-- NOT this; the CM7's
 *     0x202C_0000 .. 0x2033_FFFF   512K   OCRAM2    <-- .bss.dma goes here
 *
 * OCRAM M4 at 0x20200000 is a DIFFERENT block: it is the backdoor alias of this
 * CM4's own TCMs, and it is where Multicore::begin() stages this very image
 * (default stageAddr = CM4_BOOT_ADDRESS = 0x20200000).  Putting DMA memory
 * there would have the image overwrite itself.  OCRAM1 is the CM7's -- see the
 * ★ note at the foot of this file. */

MEMORY
{
    ITCM   (rx) : ORIGIN = 0x1FFE0000, LENGTH = 128K
    DTCM   (rw) : ORIGIN = 0x20000000, LENGTH = 128K
    OCRAM2 (rw) : ORIGIN = 0x202C0000, LENGTH = 512K
}

__StackTop = ORIGIN(DTCM) + LENGTH(DTCM);   /* 0x20020000: top of CM4 DTCM */

ENTRY(Reset_Handler)

SECTIONS
{
    .isr_vector : {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
    } > ITCM

    .text : {
        . = ALIGN(4);
        *(.text*)
        *(.rodata*)
        /* C++ static constructors: walked by runtime_stubs.c cm4_run_ctors()
         * (freestanding -nostdlib world, no crt0).  USBHost_t36 has real ones
         * -- USBHost/USBHub/the enum driver are file-scope objects whose
         * constructors call driver_ready_for_device(), so an image that skips
         * this walk enumerates a device and then claims it with NO drivers
         * registered.  That failure is silent: the device is found, nothing
         * reports it. */
        . = ALIGN(4);
        __init_array_start = .;
        KEEP(*(.init_array*))
        __init_array_end = .;
        . = ALIGN(4);
    } > ITCM

    /* .data: VMA in DTCM, LMA appended after .text in ITCM. */
    .data : {
        . = ALIGN(4);
        __data_start__ = .;
        *(.data*)
        . = ALIGN(4);
        __data_end__ = .;
    } > DTCM AT> ITCM
    __data_load__ = LOADADDR(.data);

    /* Declared BEFORE .bss on purpose: the .bss rule below globs *(.bss*),
     * which also matches an input .bss.dma, and ld assigns each input section
     * to the FIRST output section whose pattern matches.  Reverse these two and
     * every DMAMEM object lands silently in DTCM -- exactly the unreachable
     * memory this section exists to avoid. */
    .bss.dma (NOLOAD) : {
        . = ALIGN(32);
        _sbss_dma = .;          /* DMAMEM zero-init range start (startup_cm4.S) */
        KEEP(*(.bss.dma*))
        . = ALIGN(32);
        _ebss_dma = .;          /* DMAMEM zero-init range end                   */
    } > OCRAM2

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > DTCM
}

/* Notes on .bss.dma above.
 *
 * NOLOAD, and outside the ITCM LMA chain, so `objcopy -O binary` does not try
 * to span the ~3 MB gap from 0x1FFE0000 to 0x202C0000 -- the staged .bin stays
 * just .isr_vector + .text + the .data initialisers.
 *
 * KEEP() because this image links with --gc-sections and GCC 10 does not
 * promote __attribute__((used)) into SHF_GNU_RETAIN.  __attribute__((used))
 * only stops the COMPILER discarding the object; the LINKER still collects
 * .bss.dma as an ordinary garbage-collectable section.  DMAMEM buffers are the
 * kind hardware touches more than software does, so a garbage-collected DMA
 * ring surfaces as inexplicable controller misbehaviour, not as a link error --
 * exactly the class of failure that is expensive to chase here.  Do not drop
 * the KEEP.
 *
 * ALIGN(32) at both ends: EHCI descriptors are 32-byte objects and the cache
 * line is 32 bytes.  ORIGIN(OCRAM2) is already 32-byte aligned, so _sbss_dma
 * lands exactly on 0x202C0000.
 *
 * ★ The CM4's DMA buffers live in OCRAM2, NOT OCRAM1, and that is not
 * arbitrary.  The CM7's own .bss.dma (DMAMEM) is linked at OCRAM1 0x20240000 by
 * imxrt1176.ld -- its whole OCRAM region is 0x20240000/512K, i.e. OCRAM1 only,
 * with _heap_start immediately after .bss.dma -- and that section is 195,392
 * bytes in a USB audio build (usb_audio_duplex_test), spanning to ~0x2026FB40.
 * Putting the CM4's window there too would do more than overlap: the zeroing
 * loop in startup_cm4.S runs at CM4 reset, which happens AFTER the CM7 has
 * booted and staged the image, so the CM4 would wipe the front of the CM7's
 * live DMA buffers on every boot -- and the two cores' descriptors would alias
 * the same physical bytes thereafter.  Harmless only while the section is
 * empty; the first DMAMEM object makes it real.
 *
 * OCRAM2 (0x202C_0000..0x2033_FFFF, 512K, RM Table 3-2) is unused by this tree.
 * It is ordinary system SRAM in the same address map, so the EHCI bus master
 * reaches it exactly as it reaches OCRAM1, and qemu2's EHCI DMA view holes out
 * only the two TCM windows (hw/arm/fsl-imxrt1170.c: usbhost-itcm-hole,
 * usbhost-dtcm-hole) -- no OCRAM is holed.
 *
 * Do NOT move this back to OCRAM1 without also moving the CM7's DMAMEM. */
