# RT1176 SerialUSB Phase 1 (USB CDC enumeration) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Bring up USB_OTG1 as a USB CDC-ACM device that **enumerates** (reaches `usb_configuration != 0`), by porting the Teensy 4 `usb.c` enumeration core with an RT1176 PLL/PHY adaptation.

**Architecture:** Port `cores/teensy4/usb.c`'s controller + EP0 engine verbatim (identical ChipIdea IP; registers at base+0x140), trimmed to the CDC/standard cases; replace only the imxrt1062 clock/PLL/PHY-power block of `usb_init` with the RT1176 480 MHz USB-PHY-PLL sequence. Add the CDC descriptors and the USB register/IRQ definitions. Gate in QEMU (the ChipIdea CDC virtual host enumerates the guest; a machine tweak wires its chardev to USB_OTG1) and on hardware (the Mac enumerates a `/dev/cu.usbmodem`).

**Tech Stack:** C (Arduino core), ARM GCC 10.2.1 (`ARMGCC_DIR=/Applications/ARM_10`), CMake+Ninja, QEMU (`mimxrt1170-evk`), LinkServer.

## Confirmed constants (extracted — do not re-derive)

- **USB_OTG1** base `0x40430000`; **USBNC_OTG1** `0x40430200`; **USBPHY1** `0x40434000`; **IRQ_USB_OTG1 = 136**. ChipIdea/EHCI device IP identical to imxrt1062 → controller code ports verbatim (only the base changes: 1062 `0x402E0000` → `0x40430000`).
- **Controller register offsets from base** (verbatim from Teensy, device window): `USBCMD 0x140`, `USBSTS 0x144`, `USBINTR 0x148`, `DEVICEADDR 0x154`, `ENDPTLISTADDR 0x158`, `BURSTSIZE 0x160`, `PORTSC1 0x184`, `USBMODE 0x1A8`, `ENDPTSETUPSTAT 0x1AC`, `ENDPTPRIME 0x1B0`, `ENDPTFLUSH 0x1B4`, `ENDPTSTATUS 0x1B8`, `ENDPTCOMPLETE 0x1BC`, `ENDPTCTRL0..7 0x1C0..0x1DC`. QEMU's `chipidea_udc.c` models this same 0x140+ window.
- **USBPHY1 offsets:** `PWD 0x00`, `CTRL 0x30` (SET `0x34`/CLR `0x38`), `PLL_SIC` (per SDK `fsl_clock.c` — resolve exact offset from the SDK USBPHY header in Task 3). Bits: `CTRL.SFTRST=1<<31`, `CTRL.CLKGATE=1<<30`.
- **RT1176 USB PLL (480 MHz) — the one genuinely new sequence** (SDK `fsl_clock.c:1766-1849`, `CLOCK_EnableUsbhs0PhyPllClock`): enable USB LPCG; `USBPHY1.CTRL_CLR=SFTRST`; `PLL_SIC_SET=PLL_REG_ENABLE`; delay ≥15 µs; `PLL_SIC_SET=PLL_POWER`; set `PLL_DIV_SEL` (÷ for 24 MHz→480 MHz); `PLL_SIC_CLR=PLL_BYPASS`; `PLL_SIC_SET=PLL_EN_USB_CLKS`; `CTRL_CLR=CLKGATE`; **poll `PLL_SIC.PLL_LOCK`**. QEMU models the PLL loosely → **HW is the judge.**
- **CDC descriptors (from Teensy, CDC-only subset):** device 18 B; config **75 B** = 9 (config) + 8 (IAD) + 9 (comm iface) + 5+5+4+5 (CDC func) + 7 (interrupt EP `0x82`, 16 B) + 9 (data iface) + 7 (bulk-OUT `0x03`) + 7 (bulk-IN `0x84`); **bNumInterfaces=2**. Endpoints: EP2 interrupt-IN (16), EP3 bulk-OUT (512/64), EP4 bulk-IN (512/64). **VID/PID = `0x1209`/`0x0001`** (generic placeholder, not PJRC).
- **EP0** dQH config: RX `(64<<16)|(1<<15)`, TX `(64<<16)`. dQH array `endpoint_queue_head[(NUM_ENDPOINTS+1)*2]`, **4 KB-aligned**, in `.dmabuffers`/OCRAM via `DMAMEM`.
- **QEMU gate wiring:** `-global usb-chipidea.chardev=` does NOT work (two instances). The machine must set `usb[0].chardev` from a second serial: `qdev_prop_set_chr(DEVICE(&s->usb[0]),"chardev", serial_hd(1))`. Enumeration logs `CI-CDC: …` under `-d guest_errors`; success line `CI-CDC: DTR asserted -> bridging USB serial`.
- **D-cache is off** in this core (no `SCB_EnableDCache`) → no cache maintenance needed; DMAMEM/OCRAM placement is belt-and-suspenders. Verify on HW.

---

### Task 1: USB register block + IRQ number

**Files:** Modify `cores/imxrt1176/imxrt1176.h`, `cores/imxrt1176/core_pins.h`.

- [ ] **Step 1:** Append to `imxrt1176.h` a USB_OTG1 + USBPHY1 register block (accessors from base) and the bit masks used by the port. Include: `USB1_USBCMD/USBSTS/USBINTR/DEVICEADDR/ENDPOINTLISTADDR/USBMODE/ENDPTSETUPSTAT/ENDPTPRIME/ENDPTFLUSH/ENDPTSTATUS/ENDPTCOMPLETE/ENDPTCTRL0..7/PORTSC1/BURSTSIZE` at `0x40430000+offset`; `USBPHY1_CTRL/CTRL_SET/CTRL_CLR/PWD/PLL_SIC/PLL_SIC_SET/PLL_SIC_CLR` at `0x40434000+offset`; and the `USB_USBCMD_*`/`USB_USBSTS_*`/`USB_USBINTR_*`/`USB_USBMODE_*`/`USB_DEVICEADDR_*`/`USB_ENDPTCTRL_*`/`USBPHY_CTRL_*`/`USBPHY_PWD_*`/`USBPHY_PLL_SIC_*` bit macros (copy the values from `cores/teensy4/imxrt.h`; they are IP-identical). Add the USB LPCG clock-gate define (resolve the LPCG number from the SDK `fsl_clock.h` USB clock entry — Task 3 uses it).
- [ ] **Step 2:** In `core_pins.h`, add to `IRQ_NUMBER_t`: `IRQ_USB_OTG1 = 136`.
- [ ] **Step 3:** If `DMAMEM` is not defined, add to `imxrt1176.h`: `#define DMAMEM __attribute__((section(".dmabuffers"), used))`.
- [ ] **Step 4:** Verify: `grep -nE "USB1_USBCMD|USBPHY1_PLL_SIC|IRQ_USB_OTG1 = 136|define DMAMEM" cores/imxrt1176/imxrt1176.h cores/imxrt1176/core_pins.h`.
- [ ] **Step 5:** Commit: `cd ~/Development/rt1170/evkb/cores && git add imxrt1176/imxrt1176.h imxrt1176/core_pins.h && git commit -m "imxrt1176: USB_OTG1/USBPHY1 register block + IRQ_USB_OTG1 (136) + DMAMEM"`

### Task 2: CDC descriptors

**Files:** Create `cores/imxrt1176/usb_desc.c`, `cores/imxrt1176/usb_desc.h`.

- [ ] **Step 1:** Write `usb_desc.h` with the CDC #defines: `VENDOR_ID 0x1209`, `PRODUCT_ID 0x0001`, `EP0_SIZE 64`, `NUM_ENDPOINTS 4`, `NUM_INTERFACE 2`, `CDC_STATUS_INTERFACE 0`, `CDC_DATA_INTERFACE 1`, `CDC_ACM_ENDPOINT 2`, `CDC_RX_ENDPOINT 3`, `CDC_TX_ENDPOINT 4`, `CDC_ACM_SIZE 16`, `CDC_RX_SIZE_480 512`, `CDC_TX_SIZE_480 512`, `CDC_RX_SIZE_12 64`, `CDC_TX_SIZE_12 64`, `CONFIG_DESC_SIZE 75`; the `ENDPOINT_RECEIVE_*`/`ENDPOINT_TRANSMIT_*` config macros + `ENDPOINT2_CONFIG/3/4` (copy from teensy4 `usb_desc.h`); the `usb_descriptor_list_t` struct; and externs.
- [ ] **Step 2:** Write `usb_desc.c` with: `device_descriptor[18]` (bcdUSB 0x0200, class/sub/proto 0, EP0 64, VID/PID `0x1209`/`0x0001`, `iManufacturer=1,iProduct=2,iSerial=0`, 1 config), `config_descriptor[75]` (the exact CDC byte layout from the Confirmed-constants block: config header **bNumInterfaces=2**, IAD, comm iface + CDC header/callmgmt/ACM/union + interrupt EP `0x82`, data iface + bulk `0x03`/`0x84`), `string0` (langid 0x0409), manufacturer/product string descriptors, and `usb_descriptor_list[]` (`{0x0100,0,device_descriptor,18}`, `{0x0200,0,config_descriptor,75}`, `{0x0300,0,&string0,0}`, `{0x0301,0x0409,&string1,0}`, `{0x0302,0x0409,&string2,0}`, terminator). (Single-speed FS/HS: use the 512-byte bulk config; QEMU + HS HW use it.)
- [ ] **Step 3:** Commit: `cd ~/Development/rt1170/evkb/cores && git add imxrt1176/usb_desc.c imxrt1176/usb_desc.h && git commit -m "imxrt1176: USB CDC-ACM descriptors (device/config/strings, VID 0x1209)"`

### Task 3: usb.c — enumeration core (port teensy4 usb.c)

**Files:** Create `cores/imxrt1176/usb.c`, `cores/imxrt1176/usb_dev.h`.

- [ ] **Step 1:** Write `usb_dev.h`: the `transfer_t` and `endpoint_t` structs (verbatim from `teensy4/usb_dev.h:37-46` and `teensy4/usb.c:61-78`), and prototypes `usb_init`, `usb_config_rx/tx`, `usb_prepare_transfer`, `usb_transmit`, `usb_receive`, `usb_transfer_status`; externs `usb_configuration`, `usb_cdc_line_coding`, `usb_cdc_line_rtsdtr`.
- [ ] **Step 2:** Write `usb.c`. **Port verbatim** from `teensy4/usb.c` (register names already provided by Task 1's `imxrt1176.h`): `endpoint_queue_head[(NUM_ENDPOINTS+1)*2]` as `DMAMEM __attribute__((aligned(4096)))`; the EP0 buffers/`reply_buffer`/`usb_descriptor_buffer` as `DMAMEM`; `usb_isr()` (teensy4:260-399); `endpoint0_setup()` (teensy4:442-747) **trimmed to keep only** SET_ADDRESS(0x0500), SET_CONFIGURATION(0x0900), GET_CONFIGURATION(0x0880), GET_STATUS(0x0080/0x0082), SET/CLEAR_FEATURE(0x0302/0x0102), GET_DESCRIPTOR(0x0680/0x0681), and the CDC cases (SET_CONTROL_LINE_STATE 0x2221, SET_LINE_CODING 0x2021, SEND_BREAK 0x2321) — **drop** all AUDIO/MIDI/MTP/HID/MULTITOUCH/EXPERIMENTAL `#ifdef` blocks and the SOF `_reboot_Teensyduino_` logic; `endpoint0_transmit/receive/complete`, `usb_config_rx/tx`, `usb_endpoint_config`, `usb_prepare_transfer`, `schedule_transfer`, `usb_transmit/receive`, `run_callbacks` (verbatim). In GET_DESCRIPTOR, drop `arm_dcache_flush_delete` (no cache) and the 12/480 dual-config (single config). Add stubs `usb_serial_configure(){}` and `usb_serial_reset(){}` (Phase 2 fills these) so SET_CONFIGURATION links.
- [ ] **Step 3:** Replace the imxrt1062 clock/PLL/PHY block of `usb_init()` with the RT1176 sequence. Concretely, `usb_init()` becomes:

```c
void usb_init(void)
{
    usb_pll_phy_init();                 // RT1176 480 MHz USB PHY-PLL (Step 4)
    USB1_BURSTSIZE = 0x0404;
    USBPHY1_CTRL_CLR = USBPHY_CTRL_CLKGATE;
    USBPHY1_PWD = 0;
    USB1_USBMODE = USB_USBMODE_CM(2) | USB_USBMODE_SLOM;
    memset(endpoint_queue_head, 0, sizeof(endpoint_queue_head));
    endpoint_queue_head[0].config = (64 << 16) | (1 << 15);
    endpoint_queue_head[1].config = (64 << 16);
    USB1_ENDPOINTLISTADDR = (uint32_t)&endpoint_queue_head;
    USB1_USBINTR = USB_USBINTR_UE | USB_USBINTR_UEE | USB_USBINTR_URE | USB_USBINTR_SLE;
    attachInterruptVector(IRQ_USB_OTG1, &usb_isr);
    NVIC_ENABLE_IRQ(IRQ_USB_OTG1);
    USB1_USBCMD = USB_USBCMD_RS;
}
```

- [ ] **Step 4:** Implement `usb_pll_phy_init()` following SDK `CLOCK_EnableUsbhs0PhyPllClock` (`fsl_clock.c:1766-1849`) — enable the USB LPCG; `USBPHY1_CTRL_CLR=SFTRST`; `PLL_SIC_SET=PLL_REG_ENABLE`; `delayMicroseconds(20)`; `PLL_SIC_SET=PLL_POWER`; set `PLL_DIV_SEL` (24→480 MHz); `PLL_SIC_CLR=PLL_BYPASS`; `PLL_SIC_SET=PLL_EN_USB_CLKS`; `USBPHY1_CTRL_CLR=CLKGATE`; `while(!(USBPHY1_PLL_SIC & USBPHY_PLL_SIC_PLL_LOCK)) {}`. Confirm the exact `PLL_SIC` offset + bit positions against the SDK `MIMXRT1176` USBPHY header before writing (this is the one HW-critical unknown).
- [ ] **Step 5:** Build is exercised by Task 5. Commit: `cd ~/Development/rt1170/evkb/cores && git add imxrt1176/usb.c imxrt1176/usb_dev.h && git commit -m "imxrt1176: USB device controller + CDC enumeration core (ported from teensy4 usb.c; RT1176 PLL/PHY)"`

### Task 4: QEMU machine — wire the CDC host chardev to USB_OTG1

**Files:** Modify `~/Development/qemu2/hw/arm/fsl-imxrt1170.c`.

- [ ] **Step 1:** Confirm the usb child names + the `serial_hd` include: `grep -nE "usb\[|serial_hd|qdev_prop_set_chr|#include \"system/system.h\"" hw/arm/fsl-imxrt1170.c` (add `#include "system/system.h"` if `serial_hd` is undeclared).
- [ ] **Step 2:** After the USB realize loop (~`:889`), wire a second serial backend to USB_OTG1 so a chardev activates the CDC host:

```c
    /* Attach -serial #1 (if present) to USB_OTG1 to enable the ChipIdea CDC-ACM
     * host bridge for the SerialUSB enumeration gate. -serial #0 stays the LPUART console. */
    if (serial_hd(1)) {
        qdev_prop_set_chr(DEVICE(&s->usb[0]), "chardev", serial_hd(1));
    }
```

(If `qdev_prop_set_chr` after realize is rejected, set it before `sysbus_realize(&s->usb[0])` inside the loop for `i==0`.)

- [ ] **Step 3:** Rebuild QEMU: `cd ~/Development/qemu2/build && ninja qemu-system-arm 2>&1 | tail -3`.
- [ ] **Step 4:** Commit: `cd ~/Development/qemu2 && git add hw/arm/fsl-imxrt1170.c && git commit -m "hw/arm/fsl-imxrt1170: attach -serial #1 to USB_OTG1 (ChipIdea CDC host) for the SerialUSB gate"`

### Task 5: QEMU gate — usb_enum_test

**Files:** Create `evkb/usb_enum_test/{usb_enum_test.cpp,CMakeLists.txt,toolchain/,run_qemu_usb.sh}`.

- [ ] **Step 1:** Scaffold: `cd ~/Development/rt1170/evkb && rm -rf usb_enum_test && mkdir usb_enum_test && cp -r tone_test/toolchain usb_enum_test/ && sed 's/tone_test/usb_enum_test/g' tone_test/CMakeLists.txt > usb_enum_test/CMakeLists.txt`
- [ ] **Step 2:** Write `usb_enum_test/usb_enum_test.cpp`:

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "usb_dev.h"     // usb_init, usb_configuration

extern volatile uint8_t usb_configuration;

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    usb_init();
    uint32_t t0 = millis();
    while (usb_configuration == 0 && (millis() - t0) < 3000) { /* wait for host */ }
    if (usb_configuration) Serial1.println("USB=CONFIGURED");
    else                   Serial1.println("USB=TIMEOUT");
}
void loop() {}
```

- [ ] **Step 3:** Write `usb_enum_test/run_qemu_usb.sh` (serial #0 = LPUART gate output to a file; serial #1 = the USB CDC host backend):

```sh
#!/bin/sh
set -e
QEMU=~/Development/qemu2/build/qemu-system-arm
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/usb_enum_test.elf"; OUT="$DIR/usb.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none \
    -serial file:"$OUT" \
    -serial null \
    -d guest_errors -D "$DIR/usb.dbg" &
P=$!; sleep 6; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== VCOM ===="; cat "$OUT"
echo "==== CI-CDC (enumeration) ===="; grep "CI-CDC" "$DIR/usb.dbg" | head
grep -q "USB=CONFIGURED" "$OUT" || { echo "FAIL: USB enumeration"; exit 1; }
echo "PASS: USB CDC enumeration verified"
```

- [ ] **Step 4:** Build + run: `cd ~/Development/rt1170/evkb/usb_enum_test && chmod +x run_qemu_usb.sh && export ARMGCC_DIR=/Applications/ARM_10 && cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build && ./run_qemu_usb.sh 2>&1 | tail -12`. Expected: `USB=CONFIGURED`, `CI-CDC: … config parsed …`, `PASS`. **If it fails, this is where the model/firmware gap shows** — check the `CI-CDC:` lines to see how far enumeration got (device desc? config? DTR?), and whether `usb_pll_phy_init` / device-mode setup is the issue. Debug with systematic-debugging.
- [ ] **Step 5:** Commit: `cd ~/Development/rt1170/evkb && git add usb_enum_test && git commit -m "usb: CDC enumeration QEMU gate (ChipIdea CDC host; greps USB=CONFIGURED)"`

### Task 6: Hardware verification (the real arbiter)

**Files:** reuse `usb_enum_test` (or a tiny `usb_enum_hw` that loops printing `usb_configuration`).

- [ ] **Step 1:** Flash: `cd ~/Development/rt1170/evkb/usb_enum_test && cmake --build build && pkill -f LinkServer; sleep 2; /Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/usb_enum_test.elf` (background). Read debug VCOM `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200 with pyserial — expect `USB=CONFIGURED`.
- [ ] **Step 2:** Connect the EVKB **native USB** port (J-something, the OTG1 connector — not the MCU-Link debug USB) to the Mac. Confirm a new CDC device appears: `ls /dev/cu.usbmodem*` (a new node besides the debug one) and `ioreg -p IOUSB -l | grep -iE "0x1209|USB Serial"`. **This — the Mac enumerating — is the real pass.**
- [ ] **Step 3:** If it enumerates on HW: record VID 0x1209 device present. If QEMU passed but HW didn't: debug the PLL lock / PHY / cache (systematic-debugging) — the classic QEMU-vs-silicon gap. Record result (observational).

### Task 7: Finish

- [ ] **Step 1:** Regression — all QEMU gates PASS: `usb_enum_test`, `tone_test`, `interval_timer_test`, `irq_attach_test`, `wire_master_test`, `spi_loopback_test` runners.
- [ ] **Step 2:** Push: `cd ~/Development/rt1170/evkb/cores && git push` (core USB files → github teensy-cores); `cd ~/Development/qemu2 && git push` (machine chardev wiring → gitlab qemu-rt1170). `evkb` local.
- [ ] **Step 3:** Memory note `rt1176-serialusb.md` (Phase 1): USB_OTG1 @ 0x40430000 (regs at base+0x140, ChipIdea IP identical to imxrt1062 → teensy4 usb.c ports verbatim), USBPHY1 @ 0x40434000, IRQ 136; the ONE new bit = RT1176 USB-PHY-PLL 480 MHz (`CLOCK_EnableUsbhs0PhyPllClock`, poll PLL_LOCK); dQH 4K-aligned in `.dmabuffers`/OCRAM (D-cache off so no maintenance); QEMU has a full ChipIdea device model + CDC virtual host — attach via `usb[0].chardev = serial_hd(1)` (machine tweak; `-global` can't target one of 2 instances), enumeration logs `CI-CDC:`; **HW is the arbiter (Mac must enumerate /dev/cu.usbmodem)**. Note Phase 2 = bulk data + Serial API. One-line pointer in MEMORY.md.
- [ ] **Step 4:** superpowers:finishing-a-development-branch.

---

## Self-review (author checklist — done)
- **Spec coverage:** USB regs+IRQ (T1); CDC descriptors (T2); PLL/PHY + controller + EP0 enumeration engine (T3); QEMU CDC-host wiring (T4); enumeration gate (T5); HW enumerate (T6); regression/push/memory (T7). All spec sections mapped.
- **Placeholder scan:** the verbatim-port functions (usb_isr/endpoint0_setup/helpers) are specified by exact teensy4:line ranges + the trim list, not reproduced (a 500-line port); every NEW/adapted piece (register block, usb_init, usb_pll_phy_init, descriptors, gate, machine wiring) is concrete. The one flagged unknown (exact PLL_SIC offset/bits) is HW-critical and called out in T3 S4.
- **Type consistency:** `usb_init`/`usb_configuration`/`usb_pll_phy_init`/`endpoint_queue_head`/`IRQ_USB_OTG1`/`DMAMEM` consistent across T1–T5; `USB=CONFIGURED` grep matches the sketch + runner (T5); `serial_hd(1)`→`usb[0]` wiring (T4) matches the runner's two `-serial` args (T5).
- **Gate-first / HW-first caveat:** USB is the peripheral where QEMU-consistency is least trustworthy; T5 debugs from the `CI-CDC:` trace, and T6 makes the Mac the arbiter.
