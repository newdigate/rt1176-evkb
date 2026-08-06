/* Linker script for a TCM-resident CM4 image on the i.MX RT1176.
 *
 * The CM4's private view: code/vectors in ITCM at 0x1FFE0000, data in DTCM at
 * 0x20000000 (128 K each, RM Table 3-2 / SDK MIMXRT1176xxxxx_cm4_ram.ld).
 * The CM7 stages the objcopy'd image into the OCRAM-M4 backdoor at 0x20200000
 * (which aliases this ITCM) and boots with VTOR = 0x20200000; the CM4 then
 * relocates VTOR to its own 0x1FFE0000 table.  .data lives in DTCM but its
 * initialisers (LMA) sit in ITCM, appended after .text, so the whole loadable
 * image is contiguous from 0x1FFE0000 and objcopy -O binary yields exactly the
 * bytes to stage. Public domain (author: Nicholas Newdigate). */

MEMORY
{
    ITCM (rx) : ORIGIN = 0x1FFE0000, LENGTH = 128K
    DTCM (rw) : ORIGIN = 0x20000000, LENGTH = 128K
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

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > DTCM
}
