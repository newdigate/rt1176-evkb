# qemu_dcd_boot_test

Minimal flexspi_nor boot image carrying NXP's real evkbmimxrt1170 DCD
(nxp_dcd.bin, extracted from mcuxsdk-examples _boards/evkbmimxrt1170/dcd.c).
Boots only if the QEMU bootrom stub executes the DCD and the SEMC SDRAM
gate opens: stack + data live in SDRAM; prints DCD-SDRAM-OK on LPUART1.

Build:  arm-none-eabi-gcc -nostdlib -mcpu=cortex-m7 -mthumb -Wl,-Ttext=0x30000000 -o dcd_test.elf dcd_test.s
Run:    qemu-system-arm -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel dcd_test.elf -serial stdio -display none
