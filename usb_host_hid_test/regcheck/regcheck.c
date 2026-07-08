/* regcheck.c -- throwaway compile-check for the RT1176 USBHost_t36 register
 * wiring (Sub-project A, Task 1).  It exercises the USBHS_* aliases that
 * USBHost_t36/utility/imxrt_usbhs.h maps onto the core's USB_OTG2 / USBPHY2
 * host-controller defines, so a clean compile proves every macro the library's
 * ehci.cpp touches resolves under __IMXRT1176__.  Not linked, not flashed.
 *
 * Build (bare macro-resolution check, mirrors the v117 gate flags):
 *   /Applications/ARM_10/bin/arm-none-eabi-gcc -c \
 *     -mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16 \
 *     -D__IMXRT1176__ \
 *     -I<evkb/cores/imxrt1176> -I<USBHost_t36> regcheck.c -o /dev/null
 *
 * NOTE: this core has no "imxrt.h" (the register header is imxrt1176.h and the
 * IRQ_NUMBER_t enum lives in core_pins.h); core_pins.h pulls in both plus
 * NVIC_ENABLE_IRQ, so it is the correct entry point here.
 */
#include "core_pins.h"
#include "utility/imxrt_usbhs.h"

volatile uint32_t sink;

void check(void) {
    USBHS_USBCMD = USBHS_USBCMD_RS | USBHS_USBCMD_RST;
    USBHS_USBMODE = USBHS_USBMODE_CM(3);
    USBHS_PORTSC1 |= USBHS_PORTSC_PP;
    sink = USBHS_USBSTS;
    USBHS_USBINTR = USBHS_USBINTR_PCE;
    NVIC_ENABLE_IRQ(IRQ_USBHS);
    USBPHY2_PWD = 0;
}
