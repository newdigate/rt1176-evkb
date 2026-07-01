# RT1176 FlexRAM bank configuration (ITCM boot-fault fix) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Configure RT1176 FlexRAM at boot so ITCM-resident code is backed by real ITCM, fixing an early-boot `INVSTATE` fault that appears on hardware once a sketch grows past the BootROM-default ITCM size.

**Architecture:** Add the `IOMUXC_GPR_GPR14/16/17` register macros; in the `naked` `ResetHandler`, write the FlexRAM bank config (GPR17/16/14) as the very first action — before setting SP — so DTCM-top is backed before any push; move `_estack` to DTCM-top; and **correct the `_flexram_bank_config` formula** (the inherited teensy4/RT1062 value is wrong for the RT1176 bank encoding). Validated by every existing QEMU gate plus an on-silicon regression sweep.

**Tech Stack:** C bare-metal, ARM GCC 10.2.1 (`/Applications/ARM_10`), CMake + teensy-cmake-macros, custom QEMU `mimxrt1170-evk` (Phase-1 FlexRAM GPR model), LinkServer + pyOCD + pyserial for hardware.

---

## Reference facts

- Core repo `~/Development/rt1170/evkb/cores` (github `newdigate/teensy-cores`), branch off `master` (`c2a5480`). Core dir `cores/imxrt1176/`.
- `IOMUXC_GPR_BASE = 0x400E4000` (already in `imxrt1176.h`). GPR14 @ `+0x38` (`0x400E4038`), GPR16 @ `+0x40` (`0x400E4040`), GPR17 @ `+0x44` (`0x400E4044`).
- **RT1176 FLEXRAM_BANK_CFG (GPR17) encoding**, per the RT1176 RM and the Phase-1 QEMU model (`~/Development/qemu2/hw/arm/fsl-imxrt1170.c` ~line 474): 2 bits/bank × 16 banks; `0b01 = ITCM`, `0b10 = DTCM`, `0b00 = OCRAM`, `0b11 = reserved`. Enabled by `GPR16.FLEXRAM_BANK_CFG_SEL` (bit 2).
- teensy4 (RT1062) `ResetHandler` sequence: `GPR17 = &_flexram_bank_config; GPR16 = 0x00200007; GPR14 = 0x00AA0000;` then `mov sp, &_estack`.
- The linker already emits `_itcm_block_count = (SIZEOF(.text.itcm)+SIZEOF(.ARM.exidx)+0x7FFF) >> 15` (32K banks). It ALSO emits `_flexram_bank_config` but with the WRONG (RT1062) formula — Task 2 corrects it.
- Current `startup.c` `ResetHandler` (`section(".startup"), naked`) first line is `__asm__ volatile("mov sp, %0" :: "r"((uint32_t)&_estack) : "memory");`. `_flexram_bank_config` is a linker ABSOLUTE symbol — declare `extern uint32_t _flexram_bank_config;` and use `(uint32_t)&_flexram_bank_config` (its address IS the value).
- Current `_estack = ORIGIN(OCRAM) + LENGTH(OCRAM)` (0x202C0000). Target: `_estack = ORIGIN(DTCM) + ((16 - _itcm_block_count) << 15)` (DTCM-top).
- **The bug:** without the GPR config the FlexRAM stays at the BootROM default; when `.text.itcm` grows past the default ITCM, `memory_copy` writes the top of it into unbacked/DTCM banks → INVSTATE fault before first serial byte. Confirmed via pyOCD (CFSR=0x00020000). Small sketches fit → work.
- ARM flags for compile checks: `-mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16 -Os -I. -D__IMXRT1176__ -DTEENSYDUINO=159 -DARDUINO=10607 -DARDUINO_MIMXRT1170_EVKB -DF_CPU=996000000 -DUSB_SERIAL -DLAYOUT_US_ENGLISH` (C: `-std=gnu11`; C++: add `-std=gnu++17 -fno-exceptions -fpermissive -fno-rtti -fno-threadsafe-statics -felide-constructors -Wno-error=narrowing`).
- `imxrt1176.h` is GENERATED — edit `tools/gen_imxrt1176_h.py`, then `python3 tools/gen_imxrt1176_h.py`.
- QEMU: `~/Development/qemu2/build/qemu-system-arm -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel <elf> ...`. Existing gates: `blink/` (GPIO9 toggle via monitor), `serial_test/run_qemu.sh` (TX), `serial_test_rx/run_qemu_rx.sh` (RX), `analog_test/run_qemu_adc.sh` (LPADC). Rebuild QEMU if stale: `ninja -C ~/Development/qemu2/build qemu-system-arm`.
- Hardware: LinkServer flash leaves the core halted (`VC_CORERESET`); power-cycle the target to run (memory `rt1170-evkb-flashing`). `serial_test_rx` is a known-good small sketch. Capture VCOM with pyserial @115200 (not `cat`).

## File structure

- `cores/imxrt1176/tools/gen_imxrt1176_h.py` — MODIFY: emit `IOMUXC_GPR_GPR14/16/17`. → `imxrt1176.h` regenerated.
- `cores/imxrt1176/imxrt1176.ld` — MODIFY: correct `_flexram_bank_config` formula; set `_estack` to DTCM-top.
- `cores/imxrt1176/startup.c` — MODIFY: `extern _flexram_bank_config`; write GPR17/16/14 first in `ResetHandler`, before `mov sp`.

---

## Task 1: Emit IOMUXC_GPR_GPR14/16/17 macros

**Files:** Modify `tools/gen_imxrt1176_h.py`; regenerate `imxrt1176.h`.

- [ ] **Step 1: Failing check**
```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
printf '#include "imxrt1176.h"\nint main(void){ IOMUXC_GPR_GPR17=0; IOMUXC_GPR_GPR16=0; IOMUXC_GPR_GPR14=0; return 0; }\n' > /tmp/gpr.c
/Applications/ARM_10/bin/arm-none-eabi-gcc -mcpu=cortex-m7 -I. -c /tmp/gpr.c -o /tmp/gpr.o
```
Expected: FAIL (undefined). Record it.

- [ ] **Step 2: Emit the GPR macros**
In `tools/gen_imxrt1176_h.py`, before the final `L += ["", "#endif"]`, append:
```python
    L += ["",
          "/* IOMUXC_GPR FlexRAM bank-config registers (base 0x400E4000) */",
          "#define IOMUXC_GPR_GPR14 (*(volatile uint32_t *)0x400E4038u)",
          "#define IOMUXC_GPR_GPR16 (*(volatile uint32_t *)0x400E4040u)",
          "#define IOMUXC_GPR_GPR17 (*(volatile uint32_t *)0x400E4044u)"]
```
Regenerate + confirm:
```bash
python3 tools/gen_imxrt1176_h.py
grep -nE "IOMUXC_GPR_GPR1[467]" imxrt1176.h
```
Expected: GPR14=`0x400E4038`, GPR16=`0x400E4040`, GPR17=`0x400E4044`.

- [ ] **Step 3: Re-run the check — PASS**
```bash
/Applications/ARM_10/bin/arm-none-eabi-gcc -mcpu=cortex-m7 -I. -c /tmp/gpr.c -o /tmp/gpr.o && echo OK
```
Expected: `OK`.

- [ ] **Step 4: Regenerate-idempotence + commit**
```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176 && python3 tools/gen_imxrt1176_h.py
cd ~/Development/rt1170/evkb/cores && git diff --stat imxrt1176/imxrt1176.h   # only the 3 new lines; re-run generator => no further diff
git add -A && git commit -m "feat(imxrt1176): IOMUXC_GPR_GPR14/16/17 (FlexRAM bank config)"
```

---

## Task 2: FlexRAM config + corrected formula + DTCM-top stack

**Files:** Modify `imxrt1176.ld`, `startup.c`. Verified by ALL QEMU gates (this is where the fix is validated in emulation).

- [ ] **Step 1: Correct the `_flexram_bank_config` formula in imxrt1176.ld**
Find:
```
_flexram_bank_config = 0xAAAAAAAA | ((1 << (_itcm_block_count * 2)) - 1);
```
Replace with (RT1176 encoding: ITCM banks = `0b01`, DTCM banks = `0b10`):
```
/* RT1176 GPR17 FLEXRAM_BANK_CFG: 2 bits/bank, 01=ITCM 10=DTCM 00=OCRAM.
 * Low _itcm_block_count banks -> ITCM (01); the rest -> DTCM (10).
 * (The teensy4/RT1062 form `0xAAAAAAAA | ((1<<(n*2))-1)` yields 0b11=reserved
 * for the ITCM banks and does NOT work on RT1176.) */
_flexram_bank_config = (0xAAAAAAAA & ~((1 << (_itcm_block_count * 2)) - 1)) | (0x55555555 & ((1 << (_itcm_block_count * 2)) - 1));
```

- [ ] **Step 2: Move `_estack` to DTCM-top in imxrt1176.ld**
Find:
```
	_estack = ORIGIN(OCRAM) + LENGTH(OCRAM);
```
Replace with:
```
	/* Stack at the top of DTCM.  Valid because ResetHandler configures FlexRAM
	 * (GPR17/16/14) BEFORE setting SP, so DTCM-top is backed before any push. */
	_estack = ORIGIN(DTCM) + ((16 - _itcm_block_count) << 15);
```
(Leave the `_heap_end` OCRAM reservation as-is; out of scope.)

- [ ] **Step 3: Add the FlexRAM config to ResetHandler (startup.c)**
Add near the other externs (by `extern uint32_t _estack;`, ~line 68):
```c
extern uint32_t _flexram_bank_config;   /* linker absolute symbol; its address IS the value */
```
In `ResetHandler`, insert these three writes as the **first statements**, BEFORE the existing `__asm__ volatile("mov sp, ...")`:
```c
	/* Configure FlexRAM bank split (ITCM/DTCM) BEFORE using the stack: the
	 * BootROM-default split does not back DTCM-top, so this must run first.
	 * ResetHandler is naked and these are plain register stores (no stack use). */
	IOMUXC_GPR_GPR17 = (uint32_t)&_flexram_bank_config;   /* per-bank ITCM/DTCM map */
	IOMUXC_GPR_GPR16 = 0x00200007u;                       /* FLEXRAM_BANK_CFG_SEL + INIT_ITCM/DTCM enable */
	IOMUXC_GPR_GPR14 = 0x00AA0000u;                       /* TCM size fields */
	__asm__ volatile("dsb":::"memory"); __asm__ volatile("isb":::"memory");
```
The existing `mov sp, &_estack` (now DTCM-top) immediately follows and is now valid. Do not otherwise reorder ResetHandler. Confirm `startup.c` includes `imxrt1176.h` (it does — it uses SCB_VTOR etc.) so the `IOMUXC_GPR_GPR*` macros resolve.

- [ ] **Step 4: Build the blink sketch (smallest) — compile/link succeeds**
```bash
cd ~/Development/rt1170/evkb/blink && rm -rf build
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . >/dev/null 2>&1
cmake --build build 2>&1 | tail -3
/Applications/ARM_10/bin/arm-none-eabi-nm build/blinky.elf | grep -E " _estack| _flexram_bank_config"
```
Expected: `blinky.elf` links; `_estack` now resolves to a `0x2000xxxx` (DTCM) address, not `0x202C0000`.

- [ ] **Step 5: QEMU regression — ALL gates pass (validates the fix in emulation)**
```bash
QEMU=~/Development/qemu2/build/qemu-system-arm
# blink: GPIO9 DR toggles
( sleep 2; for i in 1 2 3; do echo 'xp /1xw 0x40c64000'; sleep 0.6; done; echo quit ) \
  | $QEMU -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel ~/Development/rt1170/evkb/blink/build/blinky.elf -display none -monitor stdio 2>&1 | grep 0x40c64000
# TX
cd ~/Development/rt1170/evkb/serial_test && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . >/dev/null 2>&1 && cmake --build build >/dev/null 2>&1 && sh run_qemu.sh 2>&1 | grep -E "PASS|FAIL"
# RX
cd ~/Development/rt1170/evkb/serial_test_rx && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . >/dev/null 2>&1 && cmake --build build >/dev/null 2>&1 && sh run_qemu_rx.sh 2>&1 | grep -E "PASS|FAIL"
# LPADC
cd ~/Development/rt1170/evkb/analog_test && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . >/dev/null 2>&1 && cmake --build build >/dev/null 2>&1 && sh run_qemu_adc.sh 2>&1 | grep -E "PASS|FAIL"
```
Expected: blink GPIO9 DR bit 3 toggles across samples; `PASS: QEMU serial output verified`; `PASS: echo verified, rx_isr=1`; `PASS: LPADC blocking reads verified`. If any fails, the FlexRAM formula or GPR write is wrong (e.g. DTCM sized to 0 → stack faults in QEMU) — debug the formula/encoding against the QEMU model (`~/Development/qemu2/hw/arm/fsl-imxrt1170.c` ~line 474); do NOT proceed to hardware until QEMU is green.

- [ ] **Step 6: Commit**
```bash
cd ~/Development/rt1170/evkb/cores && git add -A && \
git commit -m "fix(imxrt1176): configure FlexRAM banks + DTCM-top stack (correct RT1176 bank encoding)"
```

---

## Task 3: Hardware acceptance + regression (the definitive proof)

**Files:** none (uses existing sketches + hardware helpers). Requires the physical EVKB, powered.

- [ ] **Step 1: Flash + boot the previously-failing LPADC sketch**
```bash
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load ~/Development/rt1170/evkb/analog_test/build/analog_test.hex
```
Power-cycle the target (SW5 off/on or POR SW4) so it runs — see memory `rt1170-evkb-flashing` (LinkServer leaves it halted).

- [ ] **Step 2: Verify analog_test now BOOTS + reads A0 on silicon**
```bash
python3 ~/Development/rt1170/evkb/analog_test/adc_hw.py /dev/tty.usbmodem*   # send-less A0 reader; prints A0= lines
```
Expected: the banner + `adc1_ch5=341`/`async_val=341`/`A0=<value>` lines appear (previously TOTALLY SILENT). Optionally tie `GPIO_AD_06` (A0) to GND→~0 / 3V3→~1023 to confirm tracking. **This is the fix acceptance** — the exact sketch that hard-faulted before now runs.

- [ ] **Step 3: Hardware regression — small sketches still run**
Flash + confirm each still works on silicon (proves the FlexRAM re-bank didn't break them):
```bash
# blink: LED still toggles (visual, or GPIO via debugger)
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load ~/Development/rt1170/evkb/blink/build/blinky.hex   # power-cycle; LED blinks
# serial RX echo
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load ~/Development/rt1170/evkb/serial_test_rx/build/serial_test_rx.hex   # power-cycle
python3 ~/Development/rt1170/evkb/serial_test_rx/capture_rx_hw.py /dev/tty.usbmodem*
```
Expected: blink LED toggles; `PASS: hardware RX echo verified (rx_isr=...)`. (The controller runs these steps directly, coordinating the power-cycle with the user, since they need the physical board.)

- [ ] **Step 4: No commit** (verification only; the fix landed in Task 2). Record the hardware results in the final summary.

---

## Final review

After all tasks: dispatch a final review of the whole change (GPR macros + startup + linker), confirm all four QEMU gates + the hardware acceptance/regression pass, then use `superpowers:finishing-a-development-branch` to merge to `master` and push.

**Verification summary:** Task 1 — compile + grep. Task 2 — build + all four QEMU gates (the corrected FlexRAM formula is validated here; a wrong split breaks the DTCM stack in QEMU). Task 3 — the previously-faulting `analog_test` boots + reads on silicon, and blink/serial-RX still run on silicon.
