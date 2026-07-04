# RT1176 attachInterrupt Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Implement Arduino `attachInterrupt(pin, fn, mode)` / `detachInterrupt(pin)` for GPIO pin interrupts on all Arduino-header pins, all 5 modes.

**Architecture:** New `interrupt_attach.c` with a per-pin callback table and two shared GPIO ISRs (combined IRQ 99 for GPIO8/9/11; 61/62 for GPIO12), riding on the existing `digital_pin_to_info` table. QEMU-gated (the GPIO model implements interrupt logic; the machine wires D13→D9 output→input) and HW-verified.

**Tech Stack:** C (Arduino core), ARM GCC 10.2.1 (`ARMGCC_DIR=/Applications/ARM_10`), CMake+Ninja, QEMU (`mimxrt1170-evk`), LinkServer.

**Confirmed constants**
- Modes (`core_pins.h`): `LOW=0, HIGH=1, FALLING=2, RISING=3, CHANGE=4`.
- GPIO IRQs: GPIO8/9/11 → **99** (`GPIO7_8_9_10_11`); GPIO12 → **61**/**62**.
- GPIO reg offsets from base: `ICR1 +0x0C`, `ICR2 +0x10`, `IMR +0x14`, `ISR +0x18` (W1C), `EDGE_SEL +0x1C`. ICR field per pin (2 bits): 0=low,1=high,2=rising,3=falling.
- GPIO bases already in `imxrt1176.h`: GPIO8/9/11/12_BASE. Struct `digital_pin_info_struct{gpio,bit,mux_mode,mux,pad}` + `extern digital_pin_to_info[]` in `pins_arduino.h`. `attachInterruptVector` in `core_pins.h`; `NVIC_ENABLE_IRQ`/`NVIC_SET_PRIORITY` in `imxrt1176.h`.
- QEMU: GPIO9 = `FSL_IMXRT1170(dev)->gpio[8]` (base 0x40C64000). D13=gpio9 bit 27, D9=gpio9 bit 0.

---

### Task 1: Add GPIO IRQ numbers to IRQ_NUMBER_t

**Files:** Modify `cores/imxrt1176/core_pins.h`.

- [ ] **Step 1:** In the `IRQ_NUMBER_t` enum (has `IRQ_LPSPI1 = 38, ...` before `} IRQ_NUMBER_t;`), add on a new line before the closing brace:

```c
	IRQ_GPIO12_0_15 = 61, IRQ_GPIO12_16_31 = 62,
	IRQ_GPIO7_8_9_10_11 = 99,
```

- [ ] **Step 2:** Verify: `grep -nE "IRQ_GPIO7_8_9_10_11 = 99|IRQ_GPIO12_0_15 = 61" cores/imxrt1176/core_pins.h` → two matches, inside the enum.
- [ ] **Step 3:** Commit: `cd cores && git add imxrt1176/core_pins.h && git commit -m "imxrt1176: add GPIO combined IRQ numbers (99, 61/62) to IRQ_NUMBER_t"`

### Task 2: interrupt_attach.c — the engine

**Files:** Create `cores/imxrt1176/interrupt_attach.c`.

- [ ] **Step 1:** Write the file:

```c
#include "core_pins.h"     // digital_pin_to_info, mode macros, attachInterruptVector, IRQ_* enum
#include "imxrt1176.h"     // GPIO*_BASE, NVIC_ENABLE_IRQ

#define GPIO_ICR1(base)     (*(volatile uint32_t *)((base) + 0x0Cu))
#define GPIO_ICR2(base)     (*(volatile uint32_t *)((base) + 0x10u))
#define GPIO_IMR(base)      (*(volatile uint32_t *)((base) + 0x14u))
#define GPIO_ISR(base)      (*(volatile uint32_t *)((base) + 0x18u))
#define GPIO_EDGE_SEL(base) (*(volatile uint32_t *)((base) + 0x1Cu))

static void (*isr_callback[CORE_NUM_DIGITAL])(void);

// Service every pending+enabled interrupt bit on one GPIO port.
static void dispatch_port(uint32_t base) {
	uint32_t pending = GPIO_ISR(base) & GPIO_IMR(base);
	if (!pending) return;
	GPIO_ISR(base) = pending;                       // W1C before callbacks (don't lose a re-trigger)
	for (uint8_t pin = 0; pin < CORE_NUM_DIGITAL; pin++) {
		const struct digital_pin_info_struct *p = &digital_pin_to_info[pin];
		if (p->gpio == base && (pending & (1u << p->bit)) && isr_callback[pin]) {
			isr_callback[pin]();
		}
	}
}

static void gpio_isr_7_11(void) {                   // IRQ 99: GPIO7..11 (header pins on 8/9/11)
	dispatch_port(GPIO8_BASE);
	dispatch_port(GPIO9_BASE);
	dispatch_port(GPIO11_BASE);
}
static void gpio_isr_12(void) { dispatch_port(GPIO12_BASE); }   // IRQ 61/62: GPIO12 (D14/D15)

static void enable_irq_for(uint32_t base) {
	if (base == GPIO12_BASE) {
		attachInterruptVector(IRQ_GPIO12_0_15, gpio_isr_12);
		attachInterruptVector(IRQ_GPIO12_16_31, gpio_isr_12);
		NVIC_ENABLE_IRQ(IRQ_GPIO12_0_15);
		NVIC_ENABLE_IRQ(IRQ_GPIO12_16_31);
	} else {
		attachInterruptVector(IRQ_GPIO7_8_9_10_11, gpio_isr_7_11);
		NVIC_ENABLE_IRQ(IRQ_GPIO7_8_9_10_11);
	}
}

void attachInterrupt(uint8_t pin, void (*fn)(void), int mode) {
	if (pin >= CORE_NUM_DIGITAL || fn == 0) return;
	const struct digital_pin_info_struct *p = &digital_pin_to_info[pin];
	if (p->gpio == 0) return;                        // unmapped pin
	uint32_t base = p->gpio; uint8_t bit = p->bit;
	isr_callback[pin] = fn;
	if (mode == CHANGE) {
		GPIO_EDGE_SEL(base) |= (1u << bit);          // any edge
	} else {
		GPIO_EDGE_SEL(base) &= ~(1u << bit);
		uint32_t icr;                                // 0=low,1=high,2=rising,3=falling
		switch (mode) {
			case LOW:     icr = 0; break;
			case HIGH:    icr = 1; break;
			case RISING:  icr = 2; break;
			case FALLING: icr = 3; break;
			default:      icr = 2; break;
		}
		if (bit < 16) {
			GPIO_ICR1(base) = (GPIO_ICR1(base) & ~(3u << (2 * bit))) | (icr << (2 * bit));
		} else {
			uint8_t s = 2 * (bit - 16);
			GPIO_ICR2(base) = (GPIO_ICR2(base) & ~(3u << s)) | (icr << s);
		}
	}
	GPIO_ISR(base) = (1u << bit);                    // clear stale status
	GPIO_IMR(base) |= (1u << bit);                   // unmask
	enable_irq_for(base);
}

void detachInterrupt(uint8_t pin) {
	if (pin >= CORE_NUM_DIGITAL) return;
	const struct digital_pin_info_struct *p = &digital_pin_to_info[pin];
	if (p->gpio == 0) return;
	GPIO_IMR(p->gpio) &= ~(1u << p->bit);
	GPIO_EDGE_SEL(p->gpio) &= ~(1u << p->bit);
	isr_callback[pin] = 0;
}
```

- [ ] **Step 2:** Confirm it compiles into the core (built in Task 4). If `core_pins.h` doesn't pull in the struct/`digital_pin_to_info`, add `#include "pins_arduino.h"` (it's where the struct + extern live).
- [ ] **Step 3:** Commit: `cd cores && git add imxrt1176/interrupt_attach.c && git commit -m "imxrt1176: attachInterrupt/detachInterrupt (GPIO pin interrupts, shared-ISR dispatch)"`

### Task 3: QEMU machine — wire D13→D9 for the gate

**Files:** Modify `~/Development/qemu2/hw/arm/mimxrt1170-evk.c`.

- [ ] **Step 1:** Confirm the SoC exposes the GPIO array: `grep -nE "IMXRTGPIOState gpio\[|gpio\[FSL" ~/Development/qemu2/include/hw/arm/fsl-imxrt1170.h` → a `gpio[]` member. (Accessed as `FSL_IMXRT1170(dev)->gpio[8]` = GPIO9.)
- [ ] **Step 2:** After the `ssi-loopback` block, add:

```c
    /* Test fixture: loop GPIO9 header pin D13 (bit 27) output -> D9 (bit 0)
     * input, so the attachInterrupt QEMU gate can self-stimulate an edge by
     * toggling D13 and see the interrupt fire on D9. */
    qdev_connect_gpio_out(DEVICE(&FSL_IMXRT1170(dev)->gpio[8]), 27,
                          qdev_get_gpio_in(DEVICE(&FSL_IMXRT1170(dev)->gpio[8]), 0));
```

(`qdev_connect_gpio_out`/`qdev_get_gpio_in` are in `hw/qdev-core.h`, already available.)

- [ ] **Step 3:** Rebuild QEMU: `cd ~/Development/qemu2/build && ninja qemu-system-arm 2>&1 | tail -3` → links, no error.
- [ ] **Step 4:** Commit: `cd ~/Development/qemu2 && git add hw/arm/mimxrt1170-evk.c && git commit -m "hw/arm/mimxrt1170-evk: loop GPIO9 D13->D9 for the attachInterrupt gate"`

### Task 4: QEMU gate — irq_attach_test

**Files:** Create `evkb/irq_attach_test/irq_attach_test.cpp` + `CMakeLists.txt` + `toolchain/` (copy from `evkb/spi_loopback_test`) + `run_qemu_irq.sh`.

- [ ] **Step 1:** Copy scaffolding: `cd evkb && rm -rf irq_attach_test && mkdir irq_attach_test && cp -r spi_loopback_test/toolchain irq_attach_test/ && sed 's/spi_loopback_test/irq_attach_test/g' spi_loopback_test/CMakeLists.txt > irq_attach_test/CMakeLists.txt`

- [ ] **Step 2:** Write the sketch `irq_attach_test/irq_attach_test.cpp`:

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"

volatile int g_count = 0;
static void onIrq() { g_count++; }

static void pulse(int n) {                 // n high/low toggles on D13 (wired to D9)
	for (int i = 0; i < n; i++) {
		digitalWrite(13, HIGH); delayMicroseconds(50);
		digitalWrite(13, LOW);  delayMicroseconds(50);
	}
}

void setup() {
	Serial1.begin(115200); while (!Serial1) {}
	pinMode(13, OUTPUT); pinMode(9, INPUT); digitalWrite(13, LOW);
	bool ok = true;

	attachInterrupt(9, onIrq, RISING);     // 5 rising edges
	g_count = 0; pulse(5); int rising = g_count; if (rising != 5) ok = false;

	attachInterrupt(9, onIrq, CHANGE);     // 5 toggles = 10 edges
	g_count = 0; pulse(5); int change = g_count; if (change != 10) ok = false;

	detachInterrupt(9);                    // no more interrupts
	g_count = 0; pulse(5); int detached = g_count; if (detached != 0) ok = false;

	Serial1.print("rising="); Serial1.print(rising);
	Serial1.print(" change="); Serial1.print(change);
	Serial1.print(" detached="); Serial1.println(detached);
	Serial1.println(ok ? "IRQ=PASS" : "IRQ=FAIL");
}
void loop() {}
```

- [ ] **Step 3:** Write `irq_attach_test/run_qemu_irq.sh`:

```sh
#!/bin/sh
set -e
QEMU=~/Development/qemu2/build/qemu-system-arm
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/irq_attach_test.elf"; OUT="$DIR/irq.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/irq.dbg" &
P=$!; sleep 3; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "IRQ=PASS" "$OUT" || { echo "FAIL: attachInterrupt"; exit 1; }
echo "PASS: attachInterrupt verified (RISING/CHANGE/detach)"
```

- [ ] **Step 4:** Build + run: `cd evkb/irq_attach_test && chmod +x run_qemu_irq.sh && export ARMGCC_DIR=/Applications/ARM_10 && cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build && ./run_qemu_irq.sh 2>&1 | tail -6`. Expected: `rising=5 change=10 detached=0` and `IRQ=PASS`.
  (Diagnostic value: if run BEFORE Task 3's machine wire, `rising=0` → FAIL, confirming the gate actually exercises the interrupt path.)
- [ ] **Step 5:** Commit: `cd evkb && git add irq_attach_test && git commit -m "irq: attachInterrupt QEMU gate (self-stimulated via D13->D9 loop)"`

### Task 5: Hardware verification

**Files:** none (reuse `evkb/irq_attach_test`).

- [ ] **Step 1:** Jumper header **D13 → D9** on the EVKB (output → interrupt input).
- [ ] **Step 2:** Flash + capture: `cd evkb/irq_attach_test && cmake --build build && pkill -f LinkServer; sleep 2; /Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/irq_attach_test.elf` (background); read VCOM `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200 during boot. Expect `rising=5 change=10 detached=0` + `IRQ=PASS`.
- [ ] **Step 3:** Record result (observational, no commit).

### Task 6: Finish

- [ ] **Step 1:** Regression — all QEMU gates: `evkb/irq_attach_test/run_qemu_irq.sh`, `evkb/wire_master_test/run_qemu_wire.sh`, `evkb/wire_slave_test/run_qemu_wire_slave.sh`, `evkb/spi_loopback_test/run_qemu_spi.sh` → all PASS.
- [ ] **Step 2:** Push: `cd cores && git push`; `cd ~/Development/qemu2 && git push`.
- [ ] **Step 3:** Memory `rt1176-attachinterrupt.md`: combined IRQs (99 GPIO8/9/11, 61/62 GPIO12), shared-ISR dispatch reading ISR&IMR, ICR mapping (0/1/2/3 = low/high/rising/falling) + EDGE_SEL for CHANGE, QEMU gate wires D13->D9 in the machine. One-line pointer in MEMORY.md.
- [ ] **Step 4:** superpowers:finishing-a-development-branch.

---

## Self-review (author checklist — done)
- **Spec coverage:** IRQ numbers (T1), attach/detach + all 5 modes + shared ISR (T2), QEMU wire (T3), gate RISING/CHANGE/detach (T4), HW jumper (T5), regression/push/memory (T6). Covered.
- **Type consistency:** `isr_callback`, `dispatch_port`, `gpio_isr_7_11`/`gpio_isr_12`, `enable_irq_for` consistent in T2; IRQ enum names (T1) match those used in T2's `enable_irq_for`; `digital_pin_info_struct` fields (`gpio`,`bit`) match `pins_arduino.h`.
- **Deferrals:** T3 Step 1 verifies the `gpio[]` field name before use.
