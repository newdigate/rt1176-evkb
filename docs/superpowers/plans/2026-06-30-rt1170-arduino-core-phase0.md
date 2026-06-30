# RT1170 EVKB Arduino Core — Phase 0 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring up a teensy4-derived Arduino/Teensyduino core on the NXP MIMXRT1170-EVKB (RT1176, CM7) that boots and runs LED blink with `delay()`/`millis()`, verified in QEMU then on hardware.

**Architecture:** A new `cores/imxrt1176` core (branch of `newdigate/teensy-cores`) with a standalone Teensy-idiom `imxrt1176.h` generated from NXP's RT1176 register data; a `flexspi_nor` boot image (FCB/IVT/boot_data at `0x30000000`); an RT1176 `startup.c` (CCM clock tree, RAM init, SysTick); IOMUXC/GPIO digital I/O; and an RT1176 build profile in a branch of `newdigate/teensy-cmake-macros`. Verified against the custom `mimxrt1170-evk` QEMU machine (`~/Development/qemu2`), then the physical board.

**Tech Stack:** C/C++ bare-metal, ARM GCC 10.2.1 (`/Applications/ARM_10`), CMake + teensy-cmake-macros, QEMU `qemu-system-arm -M mimxrt1170-evk`, LinkServer for hardware flash, Python (QMP) for verification.

**References (read before starting):**
- Template core: `~/Development/rt1060/evkb/cores/teensy4/` (startup.c, bootdata.c, core_pins.h, delay.c, pins_arduino.h, main.cpp, yield.cpp, EventResponder.*, imxrt1060_evkb.ld)
- RT1176 register data: `~/Development/nxp/mcux-devices-rt/RT1170/MIMXRT1176/MIMXRT1176_cm7.h`
- RT1176 EVKB clock setup (authoritative): `~/Development/mcuxsdk-examples/_boards/evkbmimxrt1170/demo_apps/mc_pmsm/pmsm_enc/cm7/clock_config.c` (function `BOARD_BootClockRUN`)
- Boot-header format (validated): `~/Development/qemu2/docs/system/arm/mimxrt1170-flexspi.rst`
- QEMU SoC GPIO bases: `~/Development/qemu2/hw/arm/fsl-imxrt1170.c` (`gpio_base[]`; GPIO9 = `0x40C64000`)
- EVKB LED: GPIO9_IO03, pad `GPIO_AD_04`, "User LED D6", active-high (`~/Development/zephyr/.../mimxrt1170_evk.dtsi`)

**Conventions:**
- Build QEMU if needed: `ninja -C ~/Development/qemu2/build qemu-system-arm`; `QEMU=~/Development/qemu2/build/qemu-system-arm`.
- `gtimeout` (coreutils) bounds QEMU runs; macOS has no `/usr/bin/timeout`.
- Run QEMU output to a file then grep — piping `mon:stdio` through `grep` can hang.
- The verification harness (`tools/qemu_check_blink.py`, Task 7) is the shared test vehicle for Tasks 8–13.

---

## File Structure

```
~/Development/rt1170/evkb/
  cores/                              # git: newdigate/teensy-cores, branch imxrt1176
    imxrt1176/
      imxrt1176.h                     # generated standalone register header
      tools/gen_imxrt1176_h.py        # generator (SDK header -> Teensy idiom)
      imxrt1176.ld                    # linker / memory map
      bootdata.c                      # FCB + IVT + boot_data (flexspi_nor)
      startup.c                       # ResetHandler, CCM clocks, RAM init, SysTick, faults
      core_pins.h, delay.c            # timing macros + millis/micros/delay
      digital.c                       # pinMode/digitalWrite/digitalRead + IOMUXC
      pins_arduino.h                  # EVKB Arduino pin table + LED_BUILTIN
      main.cpp, yield.cpp, ...        # reused from teensy4
  teensy-cmake-macros/                # git: newdigate/teensy-cmake-macros, branch imxrt1176
  blink/{CMakeLists.txt,blink.cpp}
  tools/qemu_check_blink.py           # QEMU verification harness
  docs/superpowers/{specs,plans}/
```

---

## Task 1: Scaffold project and branch the repos

**Files:** Create `~/Development/rt1170/evkb/.gitignore`; clone `cores/` and `teensy-cmake-macros/` on branch `imxrt1176`.

- [ ] **Step 1: Clone the two upstreams and create branches**

```bash
cd ~/Development/rt1170/evkb
git clone git@github.com:newdigate/teensy-cores.git cores
git -C cores checkout -b imxrt1176
git clone git@github.com:newdigate/teensy-cmake-macros.git teensy-cmake-macros
git -C teensy-cmake-macros checkout -b imxrt1176
```
Expected: two repos cloned, each on a new `imxrt1176` branch.

- [ ] **Step 2: Add a .gitignore for the project repo**

```
cmake-build-*/
build/
*.elf
*.hex
*.bin
*.map
.DS_Store
```

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb
git add .gitignore && git commit -m "chore: project scaffold for RT1170 EVKB Arduino core"
```

---

## Task 2: Create the `imxrt1176` core from teensy4

**Files:** Create `cores/imxrt1176/` (subset of `cores/teensy4/`).

- [ ] **Step 1: Copy the Phase-0 file subset**

```bash
cd ~/Development/rt1170/evkb/cores
mkdir -p imxrt1176/tools
cp teensy4/{core_pins.h,delay.c,main.cpp,yield.cpp,EventResponder.cpp,EventResponder.h,pins_arduino.h,bootdata.c,startup.c} imxrt1176/
cp teensy4/{Arduino.h,WProgram.h,wiring.h,avr_emulation.h} imxrt1176/ 2>/dev/null || true
```
Expected: `imxrt1176/` holds the starting files (still RT1062-targeted; chip-specific ones are replaced in later tasks).

- [ ] **Step 2: Repoint umbrella includes to imxrt1176.h**

Run: `grep -rl '"imxrt.h"' ~/Development/rt1170/evkb/cores/imxrt1176/`
Edit each hit to `#include "imxrt1176.h"`.

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb/cores
git add imxrt1176 && git commit -m "imxrt1176: seed core from teensy4 (Phase 0 subset)"
```

---

## Task 3: RT1176 build profile in teensy-cmake-macros

**Files:** Modify `teensy-cmake-macros/CMakeLists.include.txt` (the `TEENSY_VERSION`-keyed block, ~line 74).

- [ ] **Step 1: Add an RT1176 / EVKB profile**

After the existing `TEENSY_VERSION EQUAL 42` (rt1060 evkb) block, add (selector `117` → RT1170):

```cmake
        elseif(TEENSY_VERSION EQUAL 117)
            set(CPU_DEFINE __IMXRT1176__)
            set(LINKER_FILE ${COREPATH}imxrt1176.ld)
            set(build_board MIMXRT1170_EVKB)
            set(build_fcpu 996000000)
            set(build_flags_cpu "-mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16 ")
            set(build_flags_defs "-D${CPU_DEFINE} -DTEENSYDUINO=159 ")
```

Copy the full body of the `42` block and change only `CPU_DEFINE`, `LINKER_FILE`, `build_board`, `build_fcpu`. Keep `build_usbtype`/`build_keylayout` defaults (USB is Phase 3).

- [ ] **Step 2: Commit**

```bash
cd ~/Development/rt1170/evkb/teensy-cmake-macros
git add CMakeLists.include.txt
git commit -m "imxrt1176: add RT1170 EVKB build profile (TEENSY_VERSION 117)"
```

---

## Task 4: Generate the standalone `imxrt1176.h`

**Files:** Create `cores/imxrt1176/tools/gen_imxrt1176_h.py`; generate `cores/imxrt1176/imxrt1176.h`.

- [ ] **Step 1: Write the generator**

Create `cores/imxrt1176/tools/gen_imxrt1176_h.py`:

```python
#!/usr/bin/env python3
"""Generate a standalone Teensy-idiom imxrt1176.h from the NXP CMSIS header.

Emits `#define <PREFIX>_<REG> (*(volatile uint32_t *)0x...)` for the peripherals
Phase 0 needs.  Reads peripheral base addresses from the NXP MIMXRT1176_cm7.h
`#define <INST>_BASE (...)` lines.  Output has no SDK dependency.
"""
import re, sys, pathlib

SDK = pathlib.Path.home() / "Development/nxp/mcux-devices-rt/RT1170/MIMXRT1176/MIMXRT1176_cm7.h"
OUT = pathlib.Path(__file__).resolve().parent.parent / "imxrt1176.h"

WANTED = {"CCM": "CCM", "IOMUXC": "IOMUXC", "IOMUXC_GPR": "IOMUXC_GPR",
          "FLEXSPI1": "FLEXSPI", "FLEXSPI2": "FLEXSPI2",
          **{f"GPIO{i}": f"GPIO{i}" for i in range(1, 14)}}

def parse_bases(txt):
    bases = {}
    for m in re.finditer(r'#define\s+(\w+)_BASE\s+\(([0-9a-fA-Fxu]+)\)', txt):
        try: bases[m.group(1)] = int(m.group(2).rstrip("uU"), 0)
        except ValueError: pass
    return bases

def main():
    bases = parse_bases(SDK.read_text())
    L = ["/* AUTO-GENERATED by tools/gen_imxrt1176_h.py -- do not edit by hand.",
         " * Standalone Teensy-idiom register defs for NXP i.MX RT1176 (CM7). */",
         "#ifndef _imxrt1176_h_", "#define _imxrt1176_h_", "#include <stdint.h>", ""]
    for inst, pfx in sorted(WANTED.items()):
        b = bases.get(inst)
        if b is None: print(f"warn: no base for {inst}", file=sys.stderr); continue
        L.append(f"#define {pfx}_BASE 0x{b:08X}u")
    for i in range(1, 14):
        b = bases.get(f"GPIO{i}")
        if b is None: continue
        L += [f"#define GPIO{i}_DR        (*(volatile uint32_t *)0x{b+0x00:08X}u)",
              f"#define GPIO{i}_GDIR      (*(volatile uint32_t *)0x{b+0x04:08X}u)",
              f"#define GPIO{i}_PSR       (*(volatile uint32_t *)0x{b+0x08:08X}u)",
              f"#define GPIO{i}_DR_SET    (*(volatile uint32_t *)0x{b+0x84:08X}u)",
              f"#define GPIO{i}_DR_CLEAR  (*(volatile uint32_t *)0x{b+0x88:08X}u)",
              f"#define GPIO{i}_DR_TOGGLE (*(volatile uint32_t *)0x{b+0x8C:08X}u)"]
    L += ["#define SYST_CSR   (*(volatile uint32_t *)0xE000E010u)",
          "#define SYST_RVR   (*(volatile uint32_t *)0xE000E014u)",
          "#define SYST_CVR   (*(volatile uint32_t *)0xE000E018u)",
          "#define SCB_VTOR   (*(volatile uint32_t *)0xE000ED08u)",
          "#define ARM_DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)",
          "#define ARM_DWT_CTRL   (*(volatile uint32_t *)0xE0001000u)",
          "#define ARM_DEMCR      (*(volatile uint32_t *)0xE000EDFCu)",
          "#define ARM_DEMCR_TRCENA (1u << 24)", "", "#endif"]
    OUT.write_text("\n".join(L) + "\n")
    print(f"wrote {OUT}")

if __name__ == "__main__": main()
```

- [ ] **Step 2: Run the generator**

Run: `python3 ~/Development/rt1170/evkb/cores/imxrt1176/tools/gen_imxrt1176_h.py`
Expected: `wrote .../imxrt1176.h`, no `warn` for GPIO9/CCM/IOMUXC.

- [ ] **Step 3: Verify key addresses (the test)**

Run: `grep -E 'GPIO9_DR |IOMUXC_BASE|CCM_BASE|FLEXSPI_BASE' ~/Development/rt1170/evkb/cores/imxrt1176/imxrt1176.h`
Expected: `GPIO9_DR` = `0x40C64000`, `CCM_BASE` = `0x40CC0000`, `IOMUXC_BASE` = `0x400E8000`, `FLEXSPI_BASE` = `0x400CC000`. Cross-check against `MIMXRT1176_cm7.h`; if a base is missing, extend `WANTED` and regenerate.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb/cores
git add imxrt1176/tools/gen_imxrt1176_h.py imxrt1176/imxrt1176.h
git commit -m "imxrt1176: generate standalone Teensy-idiom register header"
```

> The generator is minimal for Phase 0; each later phase extends `WANTED`/offsets (LPUART, LPADC, LPI2C, USB...) and regenerates.

---

## Task 5: Linker script (memory map)

**Files:** Create `cores/imxrt1176/imxrt1176.ld` (from `imxrt1060_evkb.ld`).

- [ ] **Step 1: Copy the rt1060 EVKB linker as a base**

```bash
cp ~/Development/rt1170/evkb/cores/teensy4/imxrt1060_evkb.ld \
   ~/Development/rt1170/evkb/cores/imxrt1176/imxrt1176.ld
```

- [ ] **Step 2: Set the RT1176 memory map**

Edit the `MEMORY { ... }` block in `cores/imxrt1176/imxrt1176.ld` to:

```ld
MEMORY
{
	ITCM (rx):  ORIGIN = 0x00000000, LENGTH = 256K
	DTCM (rw):  ORIGIN = 0x20000000, LENGTH = 256K
	OCRAM (rw): ORIGIN = 0x20240000, LENGTH = 512K
	FLASH (rx): ORIGIN = 0x30000000, LENGTH = 16384K
	ERAM (rw):  ORIGIN = 0x80000000, LENGTH = 65536K
}
```
(FLASH moves from RT1062's `0x60000000` to RT1176's FlexSPI1 `0x30000000`; ERAM is the SEMC SDRAM window, declared but unused until Phase 4.)

- [ ] **Step 3: Confirm boot-header placement**

The boot-header sections must keep the teensy ordering so FCB lands at `0x30000400`, IVT at `0x30001000`, vector table at `0x30002000`.
Run: `grep -nE '0x400|0x1000|0x2000|flashconfig|ivt|FLASH' ~/Development/rt1170/evkb/cores/imxrt1176/imxrt1176.ld | head`
Expected: `.flashconfig`/`.ivt`/vector sections placed at those FLASH offsets (the rt1060 EVKB script already does; keep it).

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb/cores
git add imxrt1176/imxrt1176.ld
git commit -m "imxrt1176: linker memory map (FlexSPI XIP at 0x30000000)"
```

---

## Task 6: Boot data (FCB / IVT / boot_data)

**Files:** Modify `cores/imxrt1176/bootdata.c`.

- [ ] **Step 1: Set the FCB for the EVKB IS25WP128**

Replace the RT1062 FCB in `bootdata.c` with the EVKB IS25WP128 FlexSPI NOR config. Reference the proven bytes from our working Zephyr image:
`xxd -s 0x400 -l 0x200 ~/Development/zephyr/projects/zepherproject/build-hello/zephyr/zephyr.bin`.
Key fields: tag `0x42464346` ('FCFB'), version `0x56010400`, `readSampleClkSrc`, `deviceType=1` (serial NOR), `sflashPadType=4` (quad), `serialClkFreq`, `sflashA1Size=0x01000000` (16 MB), and the quad-read LUT. Set `boot_data.start=0x30000000`, `boot_data.length=0x01000000`, IVT `entry=&ResetHandler`, `self=0x30001000`. Match the layout the QEMU boot-ROM validates (`mimxrt1170-flexspi.rst`).

- [ ] **Step 2: Confirm section attributes**

Ensure the FCB/IVT/boot_data structs carry the section attributes that the Task 5 linker places at the right offsets (`__attribute__((section(".flashconfig")))`, `.ivt`, `.bootdata`). End-to-end verification happens in Task 8.

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb/cores
git add imxrt1176/bootdata.c
git commit -m "imxrt1176: FCB/IVT/boot_data for EVKB IS25WP128 (flexspi_nor)"
```

---

## Task 7: QEMU verification harness

**Files:** Create `tools/qemu_check_blink.py` (shared test vehicle for Tasks 8–13).

- [ ] **Step 1: Write the harness**

Create `~/Development/rt1170/evkb/tools/qemu_check_blink.py`:

```python
#!/usr/bin/env python3
"""Boot an ELF in the mimxrt1170-evk QEMU machine and check memory via QMP.

Modes:
  --symbol S --expect V   : read symbol S (from ELF) once, assert == V
  --symbol S --advances   : assert symbol S's value increases over --seconds
  --addr A --bit B --toggles : assert the bit at A goes both 0 and 1 over --seconds
"""
import argparse, subprocess, json, socket, time, os, re, sys
QEMU = os.path.expanduser("~/Development/qemu2/build/qemu-system-arm")
NM = "/Applications/ARM_10/bin/arm-none-eabi-nm"

def sym_addr(elf, sym):
    for line in subprocess.check_output([NM, elf]).decode().splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == sym:
            return int(p[0], 16)
    raise SystemExit(f"symbol {sym} not found in {elf}")

def qmp(sock, cmd):
    sock.sendall((json.dumps(cmd) + "\n").encode()); time.sleep(0.05)
    return sock.recv(65536).decode()

def read_word(sock, addr):
    r = qmp(sock, {"execute": "human-monitor-command",
                   "arguments": {"command-line": f"xp/1wx 0x{addr:08x}"}})
    m = re.search(r":\s*0x([0-9a-fA-F]+)", r)
    return int(m.group(1), 16) if m else None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--symbol"); ap.add_argument("--expect")
    ap.add_argument("--advances", action="store_true")
    ap.add_argument("--toggles", action="store_true")
    ap.add_argument("--addr"); ap.add_argument("--bit", type=int)
    ap.add_argument("--seconds", type=float, default=2.0)
    a = ap.parse_args()
    port = 55123
    proc = subprocess.Popen([QEMU, "-M", "mimxrt1170-evk", "-global",
        "fsl-imxrt1170.boot-xip=on", "-nographic", "-serial", "null",
        "-kernel", a.elf, "-qmp", f"tcp:127.0.0.1:{port},server,nowait"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = socket.create_connection(("127.0.0.1", port)); s.recv(65536)
        qmp(s, {"execute": "qmp_capabilities"})
        addr = int(a.addr, 0) if a.addr else sym_addr(a.elf, a.symbol)
        seen = set(); first = last = None; end = time.time() + a.seconds
        while time.time() < end:
            v = read_word(s, addr)
            if v is not None:
                if a.toggles: seen.add((v >> a.bit) & 1)
                if first is None: first = v
                last = v
            time.sleep(0.05)
        if a.toggles:
            ok = 0 in seen and 1 in seen; print(f"bits seen: {sorted(seen)}")
        elif a.advances:
            ok = first is not None and last > first; print(f"first={first} last={last}")
        else:
            ok = last == int(a.expect, 0); print(f"value=0x{last:x} expect={a.expect}")
        print("PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)
    finally:
        proc.terminate()

if __name__ == "__main__": main()
```

- [ ] **Step 2: Smoke-test the harness (the test)**

Verify it launches QEMU and reads a known-constant address (the FlexSPI window base holds the boot image after load). Use the working Zephyr ELF as a stand-in target that boots:
```bash
cd ~/Development/rt1170/evkb
python3 tools/qemu_check_blink.py \
  ~/Development/zephyr/projects/zepherproject/build-hello/zephyr/zephyr.elf \
  --addr 0x30000400 --bit 1 --seconds 1 || true
```
Expected: it launches QEMU, prints a `bits seen:` line and a PASS/FAIL (value content is irrelevant here — we're confirming the harness reads memory without error).

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/qemu_check_blink.py
git commit -m "tools: QEMU verification harness (symbol/advances/toggles via QMP)"
```

---

## Task 8: Minimal startup — boot to `main()` (BootROM clocks)

**Files:** Modify `cores/imxrt1176/startup.c`.

Strategy: reach `main()` relying on the clocks the BootROM already set; add CCM in Task 9.

- [ ] **Step 1: Reduce startup.c to a minimal RT1176 ResetHandler**

Replace the RT1062 ResetHandler body with: set `SCB_VTOR`; enable the FPU (`*(volatile uint32_t*)0xE000ED88 |= (0xF<<20)`); copy `.data` (LMA→VMA) and fast `.text` to ITCM per linker symbols (`_stext/_etext/_stextload`); zero `.bss`; set a provisional `SystemCoreClock`; call `__libc_init_array()`; call `main()`. Keep the teensy4 vector-table array but point the reset entry at `ResetHandler`; route unused/fault vectors to a `fault_isr` infinite loop. Leave CCM untouched (Task 9). Add minimal weak stubs for any teensy symbols the link needs (`yield`, `startup_early_hook`).

Linker-symbol externs (from Task 5): `extern unsigned long _stext, _etext, _sdata, _edata, _sbss, _ebss;`

- [ ] **Step 2: Build a boot probe**

Create `cores/imxrt1176/tools/probe_main.c` (temporary, not committed to core):
```c
#include "../imxrt1176.h"
volatile uint32_t boot_marker __attribute__((used));
int main(void) { boot_marker = 0xB00710AD; for(;;){} }
```

- [ ] **Step 3: Build the probe image**

```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
TC=/Applications/ARM_10/bin
$TC/arm-none-eabi-gcc -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 \
  -ffreestanding -nostdlib -O2 -D__IMXRT1176__ -T imxrt1176.ld \
  startup.c bootdata.c tools/probe_main.c -o /tmp/probe.elf -lgcc
```
Expected: links cleanly. Add minimal weak stubs for any undefined symbol the linker reports.

- [ ] **Step 4: Verify boot-to-main in QEMU (the test)**

```bash
cd ~/Development/rt1170/evkb
python3 tools/qemu_check_blink.py /tmp/probe.elf --symbol boot_marker --expect 0xB00710AD --seconds 3
```
Expected: `value=0xb00710ad ... PASS` — the CPU booted through the ROM (FCB/IVT valid) and reached `main()`.

- [ ] **Step 5: Commit (startup only; probe stays untracked)**

```bash
cd ~/Development/rt1170/evkb/cores
git add imxrt1176/startup.c
git commit -m "imxrt1176: minimal ResetHandler reaches main (BootROM clocks)"
```

---

## Task 9: CCM clock tree + SysTick

**Files:** Modify `cores/imxrt1176/startup.c`; extend/regenerate `imxrt1176.h`.

- [ ] **Step 1: Port BOARD_BootClockRUN into startup**

Add `void set_arm_clock_rt1176(void)`, called early in `ResetHandler`, mirroring the SDK reference `clock_config.c` `BOARD_BootClockRUN` as direct register writes via `imxrt1176.h`: bring up OSC/RC, configure ARM PLL for 996 MHz, set `CCM_CLOCK_ROOT` for the M7 core, AHB/IPG bus, and the GPIO/IOMUXC roots Phase 0 uses; set `SystemCoreClock = 996000000`. Extend `tools/gen_imxrt1176_h.py` `WANTED`/offsets to emit the specific `CCM_CLOCK_ROOT*`/`CCM_PLL*` registers referenced, then regenerate `imxrt1176.h`.

- [ ] **Step 2: SysTick + DWT**

Add `void systick_init(void)`: `SYST_RVR = (996000000u/1000u)-1; SYST_CVR = 0; SYST_CSR = 7;` and `ARM_DEMCR |= ARM_DEMCR_TRCENA; ARM_DWT_CTRL |= 1;`. Add `systick_isr` incrementing `systick_millis_count` (defined in Task 10) — declare it `extern volatile uint32_t systick_millis_count;` here.

- [ ] **Step 3: Verify no fault + clock value (the test)**

Extend `probe_main.c` to also write `SystemCoreClock` to a second marker:
`volatile uint32_t clk_marker __attribute__((used)); ... clk_marker = SystemCoreClock;` and call `set_arm_clock_rt1176(); systick_init();` from `main`. Rebuild (Task 8 Step 3) and:
```bash
cd ~/Development/rt1170/evkb
python3 tools/qemu_check_blink.py /tmp/probe.elf --symbol clk_marker --expect 996000000 --seconds 3
```
Expected: `value=0x3b624c00 ... PASS` (996000000), and no HardFault (probe still alive, `boot_marker` set).

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb/cores
git add imxrt1176/startup.c imxrt1176/imxrt1176.h imxrt1176/tools/gen_imxrt1176_h.py
git commit -m "imxrt1176: CCM clock tree (996 MHz) + SysTick/DWT init"
```

---

## Task 10: Timing — millis / micros / delay

**Files:** Modify `cores/imxrt1176/delay.c`, `cores/imxrt1176/core_pins.h`.

- [ ] **Step 1: Tick counter + timing functions**

In `delay.c`: `volatile uint32_t systick_millis_count = 0;`. Implement Teensy-idiom `millis()` (returns the counter), `micros()` (compose SysTick `CVR` + millis with wrap handling, as teensy4 does), `delay(ms)` (spin on `millis()`), `delayMicroseconds(us)` (spin on DWT `CYCCNT`). Keep the teensy4 signatures in `core_pins.h` so sketches compile unchanged.

- [ ] **Step 2: millis probe**

Add to `probe_main.c`: `volatile uint32_t millis_marker __attribute__((used)); ... for(;;) millis_marker = millis();` (replace the spin loop).

- [ ] **Step 3: Verify millis advances (the test)**

```bash
cd ~/Development/rt1170/evkb
# rebuild probe per Task 8 Step 3, adding delay.c to the compile line
python3 tools/qemu_check_blink.py /tmp/probe.elf --symbol millis_marker --advances --seconds 2
```
Expected: `first=<n> last=<m>` with `m > n` then `PASS` — SysTick fires and the ISR increments the counter.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb/cores
git add imxrt1176/delay.c imxrt1176/core_pins.h
git commit -m "imxrt1176: millis/micros/delay via SysTick + DWT"
```

---

## Task 11: GPIO — pinMode / digitalWrite / digitalRead + pin table

**Files:** Create `cores/imxrt1176/digital.c`; modify `cores/imxrt1176/pins_arduino.h`, `core_pins.h`.

- [ ] **Step 1: EVKB pin table with LED_BUILTIN**

In `pins_arduino.h`, replace the teensy4 map with an EVKB starter table:
```c
#define LED_BUILTIN 13   /* GPIO9_IO03, pad GPIO_AD_04 (User LED D6) */
#define CORE_NUM_DIGITAL 14
```
Add a `digital_pin_to_info[]` table (Arduino pin → {GPIO base, bit, IOMUXC mux reg, IOMUXC pad reg}) with the LED entry filled from `imxrt1176.h` (GPIO9 base `0x40C64000`, bit 3; IOMUXC mux/pad regs for `GPIO_AD_04`). Other header pins best-effort (not a gate).

- [ ] **Step 2: Implement digital.c**

Phase 0 uses **direct GPIO9** (no fast-alias remap; QEMU models GPIO9). Implement:
- `pinMode(pin,mode)`: set the pad IOMUXC mux to ALT5 (GPIO) + pad control; set `GPIOn_GDIR` bit for OUTPUT / clear for INPUT.
- `digitalWrite(pin,val)`: `val ? (GPIOn_DR_SET=bit) : (GPIOn_DR_CLEAR=bit);`
- `digitalRead(pin)`: `return (GPIOn_PSR>>bit)&1;`
Resolve `GPIOn`/bit/IOMUXC from the pin table.

- [ ] **Step 3: GPIO probe**

`probe_gpio.c`: `pinMode(LED_BUILTIN,OUTPUT); for(;;){ digitalWrite(LED_BUILTIN,1); for(volatile int v=0;v<100000;v++); digitalWrite(LED_BUILTIN,0); for(volatile int v=0;v<100000;v++);} `

- [ ] **Step 4: Verify GPIO9 toggles in QEMU (the test)**

```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
TC=/Applications/ARM_10/bin
$TC/arm-none-eabi-gcc -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 \
  -ffreestanding -nostdlib -O2 -D__IMXRT1176__ -T imxrt1176.ld \
  startup.c bootdata.c delay.c digital.c tools/probe_gpio.c -o /tmp/probe.elf -lgcc
cd ~/Development/rt1170/evkb
python3 tools/qemu_check_blink.py /tmp/probe.elf --addr 0x40C64000 --bit 3 --toggles --seconds 2
```
Expected: `bits seen: [0, 1]` then `PASS` — GPIO9 bit 3 toggles.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb/cores
git add imxrt1176/digital.c imxrt1176/pins_arduino.h imxrt1176/core_pins.h
git commit -m "imxrt1176: pinMode/digitalWrite/digitalRead + EVKB pin table (LED)"
```

---

## Task 12: Blink sketch + full CMake build

**Files:** Create `blink/CMakeLists.txt`, `blink/blink.cpp`.

- [ ] **Step 1: Blink sketch**

`blink/blink.cpp`:
```cpp
#include "Arduino.h"
void setup() { pinMode(LED_BUILTIN, OUTPUT); }
void loop() {
    digitalWrite(LED_BUILTIN, HIGH); delay(500);
    digitalWrite(LED_BUILTIN, LOW);  delay(500);
}
```

- [ ] **Step 2: blink/CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.24)
project(blinky)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)
import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176 avr util)
teensy_add_executable(blinky blink.cpp)
teensy_target_link_libraries(blinky cores)
target_link_libraries(blinky.elf stdc++)
```
(Local `SOURCE_DIR` builds the workspace branches, not GitHub `main`.)

- [ ] **Step 3: Build**

```bash
cd ~/Development/rt1170/evkb/blink
cmake -B build .
cmake --build build
```
Expected: `blinky.elf` and `blinky.hex` produced. If the link reports a missing core source (e.g. `analog.c`, `nonstd.c`), add a minimal stub or copy the teensy4 file into `cores/imxrt1176/`, then rebuild.

- [ ] **Step 4: Verify the built blink toggles in QEMU (the test)**

```bash
cd ~/Development/rt1170/evkb
python3 tools/qemu_check_blink.py blink/build/blinky.elf --addr 0x40C64000 --bit 3 --toggles --seconds 3
```
Expected: `bits seen: [0, 1]` then `PASS`.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add blink && git commit -m "blink: Arduino blink sketch + CMake build for imxrt1176"
```

---

## Task 13: Hardware verification on the EVKB

**Files:** none (verification only).

- [ ] **Step 1: Flash blink to the board**

```bash
~/Development/rt1170/rt1170-flash.sh ~/Development/rt1170/evkb/blink/build/blinky.hex
```
(If LinkServer needs a raw `.bin` at `0x30000000`, use `blinky.bin`.)
Expected: LinkServer programs the image, exit 0.

- [ ] **Step 2: Reset and observe (the test)**

Press SW4 (RESET) or power-cycle J11.
Expected: **User LED D6 blinks at ~1 Hz** (500 ms on / 500 ms off).

- [ ] **Step 3: Record the result + commit**

Update the spec verification section ("QEMU: PASS, EVKB: PASS (date)"):
```bash
cd ~/Development/rt1170/evkb
git add docs && git commit -m "docs: Phase 0 verified — blink in QEMU and on EVKB"
```

---

## Phase 0 Done Criteria

- [ ] `blinky.elf`/`.hex` builds via the CMake/teensy-macros flow (Task 12)
- [ ] QEMU: LED GPIO9 bit 3 toggles (Tasks 11–12) + `millis()` advances (Task 10)
- [ ] EVKB: User LED D6 blinks at ~1 Hz (Task 13)
- [ ] `cores` and `teensy-cmake-macros` committed on branch `imxrt1176`
- [ ] Generator (`gen_imxrt1176_h.py`) reproduces `imxrt1176.h`

## Out of scope (later phase specs)

Serial/LPUART, analog (LPADC/FlexPWM), SPI/Wire, USB device stack, SEMC SDRAM/EXTMEM, DMA, Audio, USBHost, fast-GPIO aliasing optimization, full Arduino-header pin map.
