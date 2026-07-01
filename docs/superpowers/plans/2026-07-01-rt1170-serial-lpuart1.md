# Interrupt-driven `Serial1` on LPUART1 (RT1176 Phase 1) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring up a full-parity Teensyduino `HardwareSerial` (`Serial1`) on the MIMXRT1170-EVKB's LPUART1 VCOM console — TX+RX ring buffers, an LPUART1 interrupt handler, and the `Print`/`Stream` API — verified in QEMU then on hardware at 115200 baud.

**Architecture:** Four bottom-up layers, each independently testable: (1) interrupt infrastructure — a RAM vector table + NVIC macros + `attachInterruptVector`, reusable by every future ISR-driven peripheral; (2) LPUART register/bitfield defs generated into `imxrt1176.h`; (3) the hardware-agnostic `Print`/`Stream` base classes, host-unit-tested; (4) the `HardwareSerialIMXRT` driver + a single `Serial1` instance bound to LPUART1, with a hardware table rewritten for RT1176 (LPCG clock gate, fixed console pins, runtime baud). Ported from the teensy4 core (`~/Development/rt1170/evkb/cores/teensy4/`); RTS/CTS flow control, half-duplex, 9-bit, DMA, and XBAR triggering are dropped (spec YAGNI).

**Tech Stack:** C/C++ bare-metal, ARM GCC 10.2.1 (`/Applications/ARM_10`), CMake + teensy-cmake-macros, custom QEMU `qemu-system-arm -M mimxrt1170-evk` (`~/Development/qemu2/build/qemu-system-arm`), LinkServer for hardware flash, host `clang++`/`g++` for the Print/ring-buffer unit tests, Python + pyserial for hardware serial capture.

---

## Reference facts (verified against sources — use these exact values)

**Core / NVIC (Cortex-M7):**
- `SCB_VTOR` @ `0xE000ED08` (already in `imxrt1176.h`).
- NVIC register bases: `ISER` @ `0xE000E100`, `ICER` @ `0xE000E180`, `ISPR` @ `0xE000E200`, `ICPR` @ `0xE000E280`, `IPR` (byte-addressed) @ `0xE000E400`.
- RT1176 CM7 external IRQ count = **217** → RAM vector table has `16 + 217 = 233` entries (932 B) → align **1024** (next power of two ≥ table size, per ARMv7-M VTOR requirement).
- LPUART1 IRQ number = **20** (`IRQ_LPUART1`); LPUART1..12 = 20..31.

**LPUART1 register block** (base `0x4007C000`, offsets from `PERI_LPUART.h`):
`VERID +0x0, PARAM +0x4, GLOBAL +0x8, PINCFG +0xC, BAUD +0x10, STAT +0x14, CTRL +0x18, DATA +0x1C, MATCH +0x20, MODIR +0x24, FIFO +0x28, WATER +0x2C`.

**LPUART bitfields:**
- `STAT`: `RDRF` `1<<21`, `TDRE` `1<<23`, `TC` `1<<22`, `IDLE` `1<<20`, `OR` `1<<19`.
- `CTRL`: `RE` `1<<18`, `TE` `1<<19`, `ILIE` `1<<20`, `RIE` `1<<21`, `TCIE` `1<<22`, `TIE` `1<<23`.
- `BAUD`: `SBR` = bits 0..12 (mask `0x1FFF`), `OSR` = bits 24..28 (`((n)&0x1F)<<24`), `BOTHEDGE` `1<<17`.
- `WATER`: RX count = `(WATER>>24)&0x7`, TX count = `(WATER>>8)&0x7`.
- (all confirmed against the QEMU model `~/Development/qemu2/hw/char/imxrt_lpuart.c` and `PERI_LPUART.h`.)

**LPUART1 console clock + pins (EVKB, from NXP SDK):**
- Clock root: `kCLOCK_Root_Lpuart1 = 25` → `CCM->CLOCK_ROOT[25].CONTROL` @ **`0x40CC0C80`** (CCM_BASE `0x40CC0000` + 25×`0x80`). Source mux `OscRC48MDiv2 = 0` (24 MHz), div field `0` (÷1) → **24 MHz** (`UART_CLOCK = 24000000`).
- LPCG gate: `kCLOCK_Lpuart1 = 86` → `CCM->LPCG[86].DIRECT` @ **`0x40CC6AC0`** (CCM_BASE + `0x6000` + 86×`0x20`); enable by writing `0x1`.
- TXD pad **GPIO_AD_24**: mux reg `0x400E816C` (off `0x16C`) mux val `0` (ALT0=LPUART1_TXD); pad-ctl reg `0x400E83B0` (off `0x3B0`); `LPUART1_TXD input-select` reg `0x400E8620`, daisy `0`.
- RXD pad **GPIO_AD_25**: mux reg `0x400E8170` (off `0x170`) mux val `0` (ALT0=LPUART1_RXD); pad-ctl reg `0x400E83B4` (off `0x3B4`); `LPUART1_RXD input-select` reg `0x400E861C`, daisy `0`.
- Baud (computed at runtime by `begin()`): 115200 @ 24 MHz → OSR/SBR product 208 (e.g. OSR=16, SBR=13) → 115384 baud, 0.16% error. QEMU ignores baud functionally; hardware requires it.

**Build / run:**
- Sketch build dir pattern: see `~/Development/rt1170/evkb/blink/` (CMake + `toolchain/rt1170-evkb.toolchain.cmake`, `TEENSY_VERSION=117`).
- Build QEMU if stale: `ninja -C ~/Development/qemu2/build qemu-system-arm`; `QEMU=~/Development/qemu2/build/qemu-system-arm`.
- Run a sketch ELF: `$QEMU -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel <elf> -display none -serial file:<out.uart> -d guest_errors -D <dbg>` (run backgrounded, `sleep N`, `kill`). LPUART1 is the **first** `-serial` chardev.
- Hardware flash: `/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load <img.hex>`.
- Hardware serial capture: pyserial @115200 (NOT `cat` — resets baud to 9600); `gtimeout` (no `/usr/bin/timeout` on macOS).

---

## File structure

**`cores/imxrt1176/` (repo `newdigate/teensy-cores`, work on a new branch):**
- `startup.c` — MODIFY: add `_VectorsRam[233]` (`.vectorsram`, aligned 1024); in `ResetHandler` copy `_VectorsFlash`→`_VectorsRam` and repoint `SCB_VTOR`; define `attachInterruptVector()`.
- `imxrt1176.ld` — MODIFY: dedicated `.vectorsram (NOLOAD)` region in DTCM, `ALIGN(1024)`; drop the `KEEP(*(.vectorsram))` line from `.data`.
- `core_pins.h` — MODIFY: `IRQ_NUMBER_t` enum, `NVIC_NUM_INTERRUPTS`, `attachInterruptVector`/`_VectorsRam` externs.
- `tools/gen_imxrt1176_h.py` — MODIFY: emit NVIC macros, the LPUART1 register block + bitfield macros, and the two console pads' mux/pad/select registers.
- `imxrt1176.h` — REGENERATED artifact (do not hand-edit).
- `Print.cpp`, `Print.h`, `Printable.h`, `Stream.h`, `Stream.cpp` — CREATE (port from `teensy4/`).
- `HardwareSerial.h`, `HardwareSerial.cpp`, `HardwareSerial1.cpp` — CREATE (port + RT1176 adaptation from `teensy4/`).
- `yield.cpp` — MODIFY: restore a minimal dispatcher that pumps `Serial1` events.
- `tools/test_print.cpp`, `tools/test_ringbuffer.cpp` — CREATE (host unit tests).

**`~/Development/rt1170/evkb/serial_test/` — CREATE:** a sketch (`serial_test.cpp` + `CMakeLists.txt` + `toolchain/`, cloned from `blink/`) that exercises `Serial1` for QEMU + hardware verification.

---

## Task 1: Interrupt infrastructure (RAM vector table + NVIC + `attachInterruptVector`)

**Files:**
- Modify: `cores/imxrt1176/tools/gen_imxrt1176_h.py` (NVIC macros) → regenerates `cores/imxrt1176/imxrt1176.h`
- Modify: `cores/imxrt1176/core_pins.h` (IRQ enum + externs)
- Modify: `cores/imxrt1176/imxrt1176.ld` (`.vectorsram` region)
- Modify: `cores/imxrt1176/startup.c` (`_VectorsRam`, copy, VTOR, `attachInterruptVector`)
- Test: `cores/imxrt1176/tools/test_irq_sketch.cpp` (a throwaway QEMU sketch that software-triggers an IRQ)

- [ ] **Step 1: Write the failing test — a sketch that proves an IRQ dispatches through the RAM vector table**

Create `cores/imxrt1176/tools/test_irq_sketch.cpp`. It installs a handler on a spare IRQ (use `IRQ_LPUART12 = 31`, unused by this phase), software-pends it, and on entry writes a sentinel to a fixed DTCM address the QEMU monitor can read.

```cpp
#include "core_pins.h"
#define SENTINEL_ADDR 0x20001000u          // fixed DTCM slot for monitor xp read
static volatile uint32_t *const sentinel = (uint32_t *)SENTINEL_ADDR;
static void test_isr(void) { *sentinel = 0xC0DE0001u; NVIC_CLEAR_PENDING(IRQ_LPUART12); }
extern "C" int main(void) {
    *sentinel = 0;
    attachInterruptVector(IRQ_LPUART12, &test_isr);
    NVIC_SET_PRIORITY(IRQ_LPUART12, 128);
    NVIC_ENABLE_IRQ(IRQ_LPUART12);
    NVIC_SET_PENDING(IRQ_LPUART12);        // software-trigger; ISR must run
    while (1) { __asm__ volatile("wfi"); }
}
```

- [ ] **Step 2: Build it against the *current* core and run in QEMU — verify it FAILS**

Copy `blink/` to a scratch build using `test_irq_sketch.cpp` as the source (reuse `blink/CMakeLists.txt`, swap the source file), build, then:

```bash
QEMU=~/Development/qemu2/build/qemu-system-arm
( sleep 1; echo 'xp /1xw 0x20001000'; echo quit ) \
  | $QEMU -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on \
      -kernel build/test_irq.elf -display none -monitor stdio 2>&1 | grep 0x20001000
```
Expected: compile FAILS — `NVIC_SET_PENDING`, `attachInterruptVector`, `IRQ_LPUART12` are undefined. (If you stub them out, the sentinel reads `0x00000000` because the flash table has only 16 NVIC entries and IRQ 31 never reaches a handler.)

- [ ] **Step 3: Add NVIC macros + LPUART IRQ numbers to the generator**

In `cores/imxrt1176/tools/gen_imxrt1176_h.py`, extend the Cortex-M core block (currently emitted around the `SYST_*`/`SCB_*` lines) to also append:

```python
    L += ["",
          "/* --- Cortex-M7 NVIC (Task: interrupt infrastructure) --- */",
          "#define NVIC_ISER(n) (*(volatile uint32_t *)(0xE000E100u + ((n) << 2)))",
          "#define NVIC_ICER(n) (*(volatile uint32_t *)(0xE000E180u + ((n) << 2)))",
          "#define NVIC_ISPR(n) (*(volatile uint32_t *)(0xE000E200u + ((n) << 2)))",
          "#define NVIC_ICPR(n) (*(volatile uint32_t *)(0xE000E280u + ((n) << 2)))",
          "#define NVIC_IP(n)   (*(volatile uint8_t  *)(0xE000E400u + (n)))",
          "#define NVIC_ENABLE_IRQ(n)   (NVIC_ISER((n) >> 5) = (1u << ((n) & 31)))",
          "#define NVIC_DISABLE_IRQ(n)  (NVIC_ICER((n) >> 5) = (1u << ((n) & 31)))",
          "#define NVIC_SET_PENDING(n)  (NVIC_ISPR((n) >> 5) = (1u << ((n) & 31)))",
          "#define NVIC_CLEAR_PENDING(n)(NVIC_ICPR((n) >> 5) = (1u << ((n) & 31)))",
          "#define NVIC_SET_PRIORITY(n, p) (NVIC_IP(n) = (uint8_t)(p))"]
```

Regenerate:
```bash
python3 cores/imxrt1176/tools/gen_imxrt1176_h.py
grep -n "NVIC_ENABLE_IRQ" cores/imxrt1176/imxrt1176.h   # confirm emitted
```

- [ ] **Step 4: Add the IRQ enum + externs to `core_pins.h`**

Near the top of `cores/imxrt1176/core_pins.h` (after `#include "imxrt1176.h"`), add:

```c
#define NVIC_NUM_INTERRUPTS 217
typedef enum IRQ_NUMBER_t {
    IRQ_LPUART1 = 20, IRQ_LPUART2, IRQ_LPUART3, IRQ_LPUART4,
    IRQ_LPUART5, IRQ_LPUART6, IRQ_LPUART7, IRQ_LPUART8,
    IRQ_LPUART9, IRQ_LPUART10, IRQ_LPUART11, IRQ_LPUART12   /* = 31 */
} IRQ_NUMBER_t;

#ifdef __cplusplus
extern "C" {
#endif
extern void (* volatile _VectorsRam[NVIC_NUM_INTERRUPTS + 16])(void);
void attachInterruptVector(IRQ_NUMBER_t irq, void (*function)(void));
#ifdef __cplusplus
}
#endif
```

- [ ] **Step 5: Add the RAM vector-table region to the linker script**

In `cores/imxrt1176/imxrt1176.ld`: remove `KEEP(*(.vectorsram))` from the `.data` section (line ~65), and add a dedicated NOLOAD region before `.bss`:

```ld
	.vectorsram (NOLOAD) : {
		. = ALIGN(1024);
		KEEP(*(.vectorsram))
		. = ALIGN(4);
	} > DTCM
```

- [ ] **Step 6: Define `_VectorsRam`, copy it, repoint VTOR, and implement `attachInterruptVector` in `startup.c`**

Add the RAM table (after `_VectorsFlash`):
```c
__attribute__((section(".vectorsram"), used, aligned(1024)))
void (* volatile _VectorsRam[16 + NVIC_NUM_INTERRUPTS])(void);
```
In `ResetHandler`, replace the `SCB_VTOR = (uint32_t)_VectorsFlash;` line with a copy-then-repoint (place it *after* `memory_clear(&_sbss, &_ebss)` so `.bss` zeroing does not clobber the table — `.vectorsram` is its own NOLOAD region, but keep ordering explicit):
```c
	for (unsigned i = 0; i < 16 + NVIC_NUM_INTERRUPTS; i++) {
		_VectorsRam[i] = (i < 16 + 16) ? _VectorsFlash[i] : unused_isr;
	}
	__asm__ volatile("dsb":::"memory");
	SCB_VTOR = (uint32_t)_VectorsRam;
	__asm__ volatile("dsb":::"memory"); __asm__ volatile("isb":::"memory");
```
Add the function (near `unused_interrupt_vector`):
```c
void attachInterruptVector(IRQ_NUMBER_t irq, void (*function)(void)) {
	_VectorsRam[16 + (int)irq] = function;
	__asm__ volatile("dsb":::"memory"); __asm__ volatile("isb":::"memory");
}
```
Note: `_VectorsFlash` currently declares `[16 + 16]`; the copy loop above reads only indices `< 32` from it and fills the rest with `unused_isr`, so no out-of-bounds read.

- [ ] **Step 7: Rebuild the Task-1 test sketch and run in QEMU — verify it PASSES**

```bash
QEMU=~/Development/qemu2/build/qemu-system-arm
( sleep 1; echo 'xp /1xw 0x20001000'; echo quit ) \
  | $QEMU -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on \
      -kernel build/test_irq.elf -display none -monitor stdio 2>&1 | grep 0x20001000
```
Expected: `0000000020001000: 0xc0de0001` — the ISR ran through the RAM vector table.

- [ ] **Step 8: Regression — the blink sketch still boots and toggles GPIO9**

```bash
QEMU=~/Development/qemu2/build/qemu-system-arm
( sleep 2; for i in 1 2 3; do echo 'xp /1xw 0x40c64000'; sleep 0.6; done; echo quit ) \
  | $QEMU -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on \
      -kernel ~/Development/rt1170/evkb/blink/build/blinky.elf -display none -monitor stdio 2>&1 | grep 0x40c64000
```
Expected: GPIO9 `DR` bit 3 changes across samples (blink still works with VTOR now in RAM).

- [ ] **Step 9: Commit**

```bash
cd ~/Development/rt1170/evkb/cores && git add -A && \
git commit -m "feat(imxrt1176): RAM vector table + NVIC macros + attachInterruptVector"
```

---

## Task 2: LPUART register block + bitfield macros

**Files:**
- Modify: `cores/imxrt1176/tools/gen_imxrt1176_h.py`
- Regenerate: `cores/imxrt1176/imxrt1176.h`
- Test: inline compile check + `grep`

- [ ] **Step 1: Write the failing check**

```bash
cat > /tmp/lpuart_check.c <<'EOF'
#include "imxrt1176.h"
int main(void){ return (int)(LPUART1_BAUD | LPUART_CTRL_TE | LPUART_STAT_TDRE | LPUART_BAUD_OSR(15) | LPUART_BAUD_SBR(13)); }
EOF
/Applications/ARM_10/bin/arm-none-eabi-gcc -mcpu=cortex-m7 -I cores/imxrt1176 -c /tmp/lpuart_check.c -o /tmp/lpuart_check.o
```
Expected: FAIL — `LPUART1_BAUD`, `LPUART_CTRL_TE`, etc. undefined.

- [ ] **Step 2: Emit the LPUART register block + bitfields from the generator**

In `gen_imxrt1176_h.py`, add (before the final `#endif` block):

```python
    # --- LPUART register blocks + bitfields (Task: Serial1) -----------------
    LPUART_BASES = {1:0x4007C000, 2:0x40080000, 3:0x40084000, 4:0x40088000,
                    5:0x4008C000, 6:0x40090000, 7:0x40094000, 8:0x40098000,
                    9:0x4009C000, 10:0x400A0000, 11:0x40C24000, 12:0x40C28000}
    LPUART_REGS = {"VERID":0x0,"PARAM":0x4,"GLOBAL":0x8,"PINCFG":0xC,"BAUD":0x10,
                   "STAT":0x14,"CTRL":0x18,"DATA":0x1C,"MATCH":0x20,"MODIR":0x24,
                   "FIFO":0x28,"WATER":0x2C}
    L += ["", "/* --- LPUART register blocks (Task: Serial1) --- */"]
    for n, base in sorted(LPUART_BASES.items()):
        for reg, off in LPUART_REGS.items():
            L.append(f"#define LPUART{n}_{reg} (*(volatile uint32_t *)0x{base+off:08X}u)")
    L += ["",
          "/* LPUART bitfields (RT1176 RM / PERI_LPUART.h) */",
          "#define LPUART_STAT_RDRF   (1u << 21)",
          "#define LPUART_STAT_TDRE   (1u << 23)",
          "#define LPUART_STAT_TC     (1u << 22)",
          "#define LPUART_STAT_IDLE   (1u << 20)",
          "#define LPUART_STAT_OR     (1u << 19)",
          "#define LPUART_CTRL_RE     (1u << 18)",
          "#define LPUART_CTRL_TE     (1u << 19)",
          "#define LPUART_CTRL_ILIE   (1u << 20)",
          "#define LPUART_CTRL_RIE    (1u << 21)",
          "#define LPUART_CTRL_TCIE   (1u << 22)",
          "#define LPUART_CTRL_TIE    (1u << 23)",
          "#define LPUART_BAUD_SBR(n)   ((uint32_t)(n) & 0x1FFFu)",
          "#define LPUART_BAUD_OSR(n)   (((uint32_t)(n) & 0x1Fu) << 24)",
          "#define LPUART_BAUD_BOTHEDGE (1u << 17)"]
```

- [ ] **Step 3: Regenerate and re-run the check — verify it PASSES**

```bash
python3 cores/imxrt1176/tools/gen_imxrt1176_h.py
/Applications/ARM_10/bin/arm-none-eabi-gcc -mcpu=cortex-m7 -I cores/imxrt1176 -c /tmp/lpuart_check.c -o /tmp/lpuart_check.o && echo OK
grep -n "LPUART1_BAUD " cores/imxrt1176/imxrt1176.h    # 0x4007C010
```
Expected: `OK`; `LPUART1_BAUD` resolves to `0x4007C010`.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb/cores && git add -A && \
git commit -m "feat(imxrt1176): LPUART register blocks + bitfield macros"
```

---

## Task 3: `Print` / `Stream` / `Printable` base classes (host-tested)

**Files:**
- Create: `cores/imxrt1176/Print.h`, `Print.cpp`, `Printable.h`, `Stream.h`, `Stream.cpp` (port from `cores/teensy4/`)
- Test: `cores/imxrt1176/tools/test_print.cpp` (host)

- [ ] **Step 1: Write the failing host test**

Create `cores/imxrt1176/tools/test_print.cpp` — a minimal `Print` subclass that appends to a `std::string`, asserting formatting:

```cpp
#include <string>
#include <cassert>
#include <cstdio>
#include "../Print.h"
struct StringPrint : public Print {
    std::string s;
    size_t write(uint8_t c) override { s.push_back((char)c); return 1; }
    using Print::write;
};
int main() {
    StringPrint p;
    p.print("hi"); assert(p.s == "hi"); p.s.clear();
    p.print(12345); assert(p.s == "12345"); p.s.clear();
    p.print(-42); assert(p.s == "-42"); p.s.clear();
    p.print(255, HEX); assert(p.s == "ff"); p.s.clear();
    p.println(1); assert(p.s == "1\r\n"); p.s.clear();
    p.print(3.14159, 2); assert(p.s == "3.14"); p.s.clear();
    printf("test_print OK\n");
    return 0;
}
```

- [ ] **Step 2: Run it — verify it FAILS (headers absent)**

```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
clang++ -std=c++17 tools/test_print.cpp -o /tmp/test_print 2>&1 | head
```
Expected: FAIL — `Print.h` not found.

- [ ] **Step 3: Port the base classes from teensy4**

```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
cp ../teensy4/Print.h ../teensy4/Print.cpp ../teensy4/Printable.h ../teensy4/Stream.h ../teensy4/Stream.cpp .
```
These are hardware-agnostic (they depend only on the pure-virtual `write()`), so they port unmodified. If `Print.h`/`Stream.h` `#include "core_pins.h"` or `"WString.h"` for `String`, either bring `WString.{h,cpp}` across too (check with `grep -n '#include' Print.h Stream.h`) or, if `String` support is not needed this phase, guard those includes — prefer bringing `WString` across for parity. Confirm the port compiles standalone next step.

- [ ] **Step 4: Run the host test — verify it PASSES**

```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
clang++ -std=c++17 -I. tools/test_print.cpp Print.cpp -o /tmp/test_print && /tmp/test_print
```
Expected: `test_print OK` (exit 0). If `Print.cpp` pulls in `WString.cpp`, add it to the compile line.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb/cores && git add -A && \
git commit -m "feat(imxrt1176): port Print/Stream/Printable base classes"
```

---

## Task 4: `HardwareSerialIMXRT` driver + `Serial1` instance

**Files:**
- Create: `cores/imxrt1176/HardwareSerial.h`, `HardwareSerial.cpp`, `HardwareSerial1.cpp` (port + adapt from `cores/teensy4/`)
- Test: `cores/imxrt1176/tools/test_ringbuffer.cpp` (host)

This task ports the ring-buffer + ISR core verbatim and rewrites the hardware-access layer. The board bring-up (clock/pins/baud) lands in Task 6; this task compiles and unit-tests the buffer logic, leaving `begin()`'s hardware config minimal (registers only, no clock/pin yet — those are added in Task 6).

- [ ] **Step 1: Write the failing host test for ring-buffer accounting**

Create `cores/imxrt1176/tools/test_ringbuffer.cpp`. It includes the ring-buffer sizing constants and exercises `available()`/`availableForWrite()` head/tail math with a fake register block (the pure arithmetic that must port correctly). Use the same formulas as `HardwareSerial.cpp`:

```cpp
#include <cassert>
#include <cstdint>
#include <cstdio>
// Mirror of the ring-buffer accounting in HardwareSerialIMXRT (head/tail wrap).
static const int SIZE = 64;
static int avail(int head, int tail){ return head >= tail ? head - tail : SIZE + head - tail; }
static int availForWrite(int head, int tail){ return head >= tail ? SIZE - 1 - head + tail : tail - head - 1; }
int main(){
    assert(avail(0,0) == 0);
    assert(avail(5,0) == 5);
    assert(avail(0,60) == 4);                 // wrapped
    assert(availForWrite(0,0) == SIZE - 1);
    assert(availForWrite(63,0) == 0);         // full
    printf("test_ringbuffer OK\n");
    return 0;
}
```

- [ ] **Step 2: Run it — verify it FAILS (or is a red guard until logic is in place)**

```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
clang++ -std=c++17 tools/test_ringbuffer.cpp -o /tmp/test_rb && /tmp/test_rb
```
Expected initially: this compiles/passes on its own (it is a self-contained mirror). Its purpose is to lock the wrap arithmetic you must reproduce in the driver; keep it green as a regression guard while porting. (If you prefer a failing gate, assert against `Serial1.availableForWrite()` after Task 6 integration instead.)

- [ ] **Step 3: Port `HardwareSerial.h`, dropping unsupported features**

`cp ../teensy4/HardwareSerial.h .` then edit the `hardware_t` struct down to the RT1176 console shape (single fixed TX/RX pin, LPCG gate, no XBAR/CTS/RTS):

```c
typedef struct {
    uint8_t serial_index;
    IRQ_NUMBER_t irq;
    void (*irq_handler)(void);
    void (*_serialEvent)(void);
    volatile uint32_t &lpcg_register;   // CCM->LPCG[n].DIRECT
    volatile uint32_t &clock_root_reg;  // CCM->CLOCK_ROOT[n].CONTROL
    uint32_t clock_root_val;            // mux|div for 24 MHz
    volatile uint32_t &tx_mux_reg;  uint32_t tx_mux_val;  volatile uint32_t &tx_pad_reg;
    volatile uint32_t &rx_mux_reg;  uint32_t rx_mux_val;  volatile uint32_t &rx_pad_reg;
    volatile uint32_t &rx_select_input_reg; uint32_t rx_select_input_val;
    uint16_t irq_priority;
} hardware_t;
```
Remove RTS/CTS, half-duplex, 9-bit, `transmitterEnable`, `addMemoryFor*`, `pin_to_xbar_info`, and the `IRQHandler_Serial2..12`/`serialEvent2..12` declarations (keep only `IRQHandler_Serial1`/`serialEvent1`). Keep the `HardwareSerial` abstract base, `HardwareSerialIMXRT` class, ring-buffer members, `begin/end/available/peek/read/flush/availableForWrite/write` and the `Print`/`Stream` inheritance. Add a private method declaration `void configure_hardware(void);` to the class (defined in Task 6). Also declare `extern void serialEvent1(void);`.

- [ ] **Step 4: Port `HardwareSerial.cpp`, rewriting register access for RT1176**

`cp ../teensy4/HardwareSerial.cpp .` then apply these adaptations (the ring-buffer bodies of `write`, `read`, `peek`, `available`, `availableForWrite`, `flush`, and the RX/TX halves of `IRQHandler` port **unchanged** — they operate on `port->STAT/DATA/CTRL/WATER` and the ring buffers):

1. Replace the port typedef access. teensy4 uses `IMXRT_LPUART_t *port = (IMXRT_LPUART_t *)port_addr;` and `port->BAUD` etc. Define a matching struct in `HardwareSerial.h`:
```c
typedef struct {
    volatile uint32_t VERID, PARAM, GLOBAL, PINCFG, BAUD, STAT, CTRL, DATA, MATCH, MODIR, FIFO, WATER;
} IMXRT_LPUART_t;
```
   `port_addr` is set to `0x4007C000` by the `Serial1` constructor (Task 4 Step 6). Field offsets match the register map exactly.
2. Delete the `setTX`/`setRX`/`transmitterEnable`/`clear`/`addMemoryFor*`/`write9bit`(9-bit path)/RTS helpers and their calls. Reduce `write(uint8_t c)` to the plain TX-buffer push (drop the `write9bit` indirection):
```c
size_t HardwareSerialIMXRT::write(uint8_t c) {
    IMXRT_LPUART_t *port = (IMXRT_LPUART_t *)port_addr;
    uint32_t head = tx_buffer_head_;
    if (++head >= tx_buffer_total_size_) head = 0;
    while (tx_buffer_tail_ == head) {           // buffer full: wait for ISR drain
        if ((port->STAT & LPUART_STAT_TDRE) && ((port->CTRL & LPUART_CTRL_TIE) == 0)) break;
        yield();
    }
    if (head < tx_buffer_size_) tx_buffer_[head] = c; else tx_buffer_storage_[head - tx_buffer_size_] = c;
    transmitting_ = 1;
    tx_buffer_head_ = head;
    port->CTRL |= (LPUART_CTRL_TIE | LPUART_CTRL_TCIE);
    return 1;
}
```
3. In `IRQHandler()`, keep the RX block (reads `WATER>>24` count, pushes to RX ring, clears `IDLE`) and the TX block (drains TX ring to `DATA`, clears `TIE`/sets `TCIE`, then `TC`→`transmitting_=0`, clears `TCIE`) **as-is**, but delete the RTS (`rts_pin_baseReg_`), half-duplex (`half_duplex_mode_`, `TXDIR`), and `transmit_pin_baseReg_` branches.
4. Keep `begin()`'s baud-search loop (OSR 4..32), the ring-buffer reset, the `attachInterruptVector`/`NVIC_SET_PRIORITY`/`NVIC_ENABLE_IRQ` lines, **and the FIFO/WATER writes** (teensy4 sets `port->FIFO |= (LPUART FIFO TXFE|RXFE enable)` and `port->WATER` watermarks — keep these so the RX ISR's `(WATER>>24)&7` count works and TX FIFO is used). Replace the pin-mux and clock-gate lines (`hardware->ccm_register |= hardware->ccm_value;` and the `portControlRegister`/`portConfigRegister` writes) with a call to a new `configure_hardware()` **stubbed empty for now** (filled in Task 6). Set `#define UART_CLOCK 24000000` at the top of the file (teensy4 defines it via `F_CPU`-derived macro; use the fixed LPUART root clock).

- [ ] **Step 5: Port `HardwareSerial1.cpp` — the single instance**

`cp ../teensy4/HardwareSerial1.cpp .` then rewrite the hardware table and instance for LPUART1:

```c
#include "HardwareSerial.h"
#include "core_pins.h"
#define IRQ_PRIORITY 64
static void IRQHandler_Serial1();
static uint8_t tx_buffer1[64];
static uint8_t rx_buffer1[64];
const HardwareSerialIMXRT::hardware_t UART1_Hardware = {
    0, IRQ_LPUART1, &IRQHandler_Serial1, &serialEvent1,
    CCM_LPCG86_DIRECT,                       // lpcg_register  (0x40CC6AC0)
    CCM_CLOCK_ROOT25_CONTROL,                // clock_root_reg (0x40CC0C80)
    (0u /*mux OscRC48MDiv2*/ | 0u /*div=1*/), // clock_root_val
    IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_24, 0u, IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_24, // TX
    IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_25, 0u, IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_25, // RX
    IOMUXC_LPUART1_RXD_SELECT_INPUT, 0u,     // rx daisy = GPIO_AD_25
    IRQ_PRIORITY,
};
HardwareSerialIMXRT Serial1(0x4007C000, &UART1_Hardware, tx_buffer1, sizeof(tx_buffer1),
                            rx_buffer1, sizeof(rx_buffer1));
static void IRQHandler_Serial1() { Serial1.IRQHandler(); }
void serialEvent1() __attribute__((weak)); void serialEvent1() {}
```
(The `CCM_LPCG86_DIRECT`, `CCM_CLOCK_ROOT25_CONTROL`, `IOMUXC_LPUART1_RXD_SELECT_INPUT`, and the two pad mux/pad macros are added to the generator in Task 6, Step 3 — this file will not link until then, which is expected; it compiles now only if those are present. Order Task 6 before final link, or temporarily hardcode the addresses and replace with the macros in Task 6.)

- [ ] **Step 6: Verify the ring-buffer host test still passes and the target compiles**

```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
clang++ -std=c++17 tools/test_ringbuffer.cpp -o /tmp/test_rb && /tmp/test_rb   # test_ringbuffer OK
```
Target compile is exercised by the sketch build in Task 6/7. If building the core library now, expect unresolved `configure_hardware()` body / clock+pin macros until Task 6 — that is the task boundary.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/rt1170/evkb/cores && git add -A && \
git commit -m "feat(imxrt1176): HardwareSerialIMXRT driver + Serial1 (buffer/ISR core)"
```

---

## Task 5: `serialEvent` dispatch via `yield()`

**Files:**
- Modify: `cores/imxrt1176/yield.cpp`

- [ ] **Step 1: Restore a minimal yield() dispatcher**

Replace the empty Phase-0 `yield.cpp` body with a dispatcher that pumps `Serial1`'s event (mirrors teensy4's `yield()` but only for the ported `Serial1`):

```cpp
#include "HardwareSerial.h"
extern void serialEvent1(void) __attribute__((weak));
extern "C" void yield(void) {
    static uint8_t running = 0;
    if (running) return;            // guard against re-entrancy from within serialEvent
    running = 1;
    if (&serialEvent1 && Serial1.available()) serialEvent1();
    running = 0;
}
```
Note: `delay()` in `delay.c` provides a `__attribute__((weak)) yield()`; this strong definition overrides it. Confirm there is exactly one non-weak `yield` after this change (`nm build/*.elf | grep yield`).

- [ ] **Step 2: Compile-check via the Task-7 sketch build (deferred functional test)**

`serialEvent1` firing is verified end-to-end in Task 7 (QEMU RX) and Task 8 (hardware). Here, confirm the core still links with the new `yield`.

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb/cores && git add -A && \
git commit -m "feat(imxrt1176): yield() dispatches serialEvent1"
```

---

## Task 6: Board bring-up — LPUART1 clock root, LPCG gate, pin mux, baud

**Files:**
- Modify: `cores/imxrt1176/tools/gen_imxrt1176_h.py` (console pads + LPCG + clock-root + select-input macros) → regenerate `imxrt1176.h`
- Modify: `cores/imxrt1176/HardwareSerial.cpp` (`configure_hardware()` body)

- [ ] **Step 1: Write the failing gate — a sketch that prints over LPUART1**

Create `~/Development/rt1170/evkb/serial_test/serial_test.cpp` (build harness cloned from `blink/` in Task 7; this step just authors the sketch):

```cpp
#include "core_pins.h"
#include "HardwareSerial.h"
extern "C" int main(void) {
    Serial1.begin(115200);
    Serial1.println("RT1176 Serial1 up");
    uint32_t n = 0;
    while (1) { Serial1.print("count="); Serial1.println(n++); delay(200); }
}
```

- [ ] **Step 2: Build + run in QEMU — verify it FAILS (no clock/pin config yet)**

With the Task-7 harness built, run and capture (see Task 7 Step 3 for the exact command). Expected before this task: the generator lacks the console-pad / LPCG / clock-root macros, so `HardwareSerial1.cpp` fails to compile; if stubbed, the UART file is empty because `configure_hardware()` is a no-op and TX never leaves the peripheral.

- [ ] **Step 3: Emit the console pads, LPCG, clock-root, and select-input macros**

In `gen_imxrt1176_h.py`, extend the pad tables and add the LPUART1 clock/gate registers:

```python
IOMUXC_MUX_CTL_REGS.update({"GPIO_AD_24": 0x16C, "GPIO_AD_25": 0x170})
IOMUXC_PAD_CTL_REGS.update({"GPIO_AD_24": 0x3B0, "GPIO_AD_25": 0x3B4})
```
and append to the emitted list (before final `#endif`):
```python
    L += ["",
          "/* LPUART1 console clock + input select (EVKB VCOM) */",
          "#define CCM_CLOCK_ROOT25_CONTROL (*(volatile uint32_t *)0x40CC0C80u)",
          "#define CCM_LPCG86_DIRECT        (*(volatile uint32_t *)0x40CC6AC0u)",
          "#define IOMUXC_LPUART1_TXD_SELECT_INPUT (*(volatile uint32_t *)0x400E8620u)",
          "#define IOMUXC_LPUART1_RXD_SELECT_INPUT (*(volatile uint32_t *)0x400E861Cu)"]
```
Regenerate and confirm:
```bash
python3 cores/imxrt1176/tools/gen_imxrt1176_h.py
grep -nE "GPIO_AD_24|GPIO_AD_25|CCM_CLOCK_ROOT25|CCM_LPCG86|LPUART1_RXD_SELECT" cores/imxrt1176/imxrt1176.h
```
Expected: `IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_24` = `0x400E816C`, `..._GPIO_AD_25` = `0x400E8170`, clock-root `0x40CC0C80`, LPCG `0x40CC6AC0`.

- [ ] **Step 4: Implement `configure_hardware()` in `HardwareSerial.cpp`**

First, get the exact pad-control value the SDK uses for these console pads (do not guess the RT1176 pad-register bit layout — it differs from RT106x). Read it from the EVKB board file:
```bash
grep -n "GPIO_AD_24\|GPIO_AD_25" ~/Development/mcuxsdk-examples/_boards/evkbmimxrt1170/common/pin_mux/pin_mux.c
# use the literal IOMUXC_SetPinConfig(..., 0x????U) value for GPIO_AD_24/25 (≈ line 75-80)
```
Then implement, substituting that hex for `PAD_CFG` (a single shared value is fine — both pads use the same keeper/slew config in the SDK):
```c
void HardwareSerialIMXRT::configure_hardware(void) {
    // 1. LPUART1 clock root = OscRC48MDiv2 (24 MHz), div 1
    hardware->clock_root_reg = hardware->clock_root_val;
    // 2. enable the LPUART1 LPCG clock gate
    hardware->lpcg_register = 0x1u;
    // 3. pad mux (ALT0) + pad control (PAD_CFG = exact hex from EVKB pin_mux.c)
    const uint32_t PAD_CFG = 0x02U;   // REPLACE with the IOMUXC_SetPinConfig value read above
    hardware->tx_mux_reg = hardware->tx_mux_val;  hardware->tx_pad_reg = PAD_CFG;
    hardware->rx_mux_reg = hardware->rx_mux_val;  hardware->rx_pad_reg = PAD_CFG;
    hardware->rx_select_input_reg = hardware->rx_select_input_val;  // daisy -> GPIO_AD_25
}
```
(QEMU ignores pad config, so Task 6 Step 5 passes regardless; the exact value matters only on hardware in Task 8.) Call `configure_hardware();` at the top of `begin()` (before writing `BAUD`/`CTRL`). The `BAUD` write stays as the existing `port->BAUD = LPUART_BAUD_OSR(bestosr-1) | LPUART_BAUD_SBR(bestdiv) | (bestosr<=8 ? LPUART_BAUD_BOTHEDGE : 0);`. After BAUD (and the FIFO/WATER writes kept from Task 4), enable the peripheral: `port->CTRL = LPUART_CTRL_TE | LPUART_CTRL_RE | LPUART_CTRL_RIE;`.

- [ ] **Step 5: Rebuild + run in QEMU — verify the banner appears (PASSES)**

Run the Task-7 capture command. Expected: `out.uart` contains `RT1176 Serial1 up` and several `count=N` lines.

- [ ] **Step 6: Commit**

```bash
cd ~/Development/rt1170/evkb/cores && git add -A && \
git commit -m "feat(imxrt1176): LPUART1 clock root + LPCG + console pins + baud"
```

---

## Task 7: QEMU integration test (banner + counter, `-serial` capture)

**Files:**
- Create: `~/Development/rt1170/evkb/serial_test/` (`serial_test.cpp` from Task 6, `CMakeLists.txt`, `toolchain/` cloned from `blink/`)
- Create: `~/Development/rt1170/evkb/serial_test/run_qemu.sh`

- [ ] **Step 1: Clone the blink harness for the serial sketch**

```bash
cd ~/Development/rt1170/evkb
cp -r blink serial_test
rm -rf serial_test/build
mv serial_test/blink.cpp serial_test/serial_test.cpp   # then paste the Task-6 sketch body
# edit serial_test/CMakeLists.txt: project(serial_test); teensy_add_executable(serial_test serial_test.cpp)
```

- [ ] **Step 2: Write the QEMU run+assert script**

Create `serial_test/run_qemu.sh`:
```bash
#!/bin/sh
set -e
QEMU=~/Development/qemu2/build/qemu-system-arm
ELF=$(dirname "$0")/build/serial_test.elf
OUT=$(dirname "$0")/serial.uart
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$(dirname "$0")/serial.dbg" &
P=$!; sleep 3; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 Serial1 up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "count=3" "$OUT" || { echo "FAIL: counter missing"; exit 1; }
echo "PASS: QEMU serial output verified"
```

- [ ] **Step 3: Build and run — verify PASS**

```bash
cd ~/Development/rt1170/evkb/serial_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake .
cmake --build build
sh run_qemu.sh
```
Expected: captured UART shows `RT1176 Serial1 up`, `count=0..`, and `PASS: QEMU serial output verified`.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb && git add serial_test && \
git commit -m "test(serial): QEMU integration harness for Serial1 (banner+counter)"
```

---

## Task 8: Hardware verification (flash + pyserial capture @115200)

**Files:**
- Create: `~/Development/rt1170/evkb/serial_test/capture_hw.py` (pyserial reader)

- [ ] **Step 1: Write the hardware capture helper**

Create `serial_test/capture_hw.py`:
```python
import sys, time, serial   # pip install pyserial
port = sys.argv[1] if len(sys.argv) > 1 else "/dev/tty.usbmodem*"
import glob
if "*" in port: port = sorted(glob.glob(port))[0]
s = serial.Serial(port, 115200, timeout=1)
end = time.time() + 5
buf = b""
while time.time() < end:
    buf += s.read(256)
sys.stdout.write(buf.decode(errors="replace"))
assert b"RT1176 Serial1 up" in buf, "FAIL: banner not seen on hardware"
assert b"count=" in buf, "FAIL: counter not seen on hardware"
print("\nPASS: hardware serial output verified")
```

- [ ] **Step 2: Flash the sketch with LinkServer**

```bash
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB \
    load ~/Development/rt1170/evkb/serial_test/build/serial_test.hex
```
Expected: `Flash Written`/success. Press SW4 (reset) if the board does not auto-run.

- [ ] **Step 3: Capture the VCOM output — verify PASS**

```bash
ls /dev/tty.usbmodem*                       # find the MCU-Link VCOM
python3 ~/Development/rt1170/evkb/serial_test/capture_hw.py /dev/tty.usbmodemXXXX
```
Expected: `RT1176 Serial1 up`, `count=N` lines, `PASS: hardware serial output verified`. This is the same VCOM path Zephyr `hello_world` used — confirms clock tree, baud divisor, pin mux, and the ISR-driven TX path on silicon.

- [ ] **Step 4: Secondary check — `millis()` cadence via serial**

Temporarily change the sketch loop to `Serial1.print("t="); Serial1.println(millis());` with `delay(1000)`; reflash; confirm successive `t=` values increase by ~1000 (±2%), validating the DWT-derived time base against the real serial clock.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb && git add serial_test/capture_hw.py && \
git commit -m "test(serial): hardware pyserial capture for Serial1 @115200"
```

---

## Final review

After all tasks: dispatch a final code review over the `cores/imxrt1176` serial additions + `serial_test/`, then use `superpowers:finishing-a-development-branch` to merge the `cores` feature branch and (if remoted) push.

**Verification summary:** Task 1 (IRQ infra) — QEMU software-interrupt sentinel; Task 2 (regs) — compile + grep; Task 3 (Print) — host unit test; Task 4 (driver) — host ring-buffer test + target compile; Task 5 (yield) — link check; Task 6 (bring-up) — QEMU banner; Task 7 — QEMU integration assert; Task 8 — hardware pyserial capture. Primary regression gate is the QEMU banner/counter assert (Task 7); hardware (Task 8) is the acceptance gate.
