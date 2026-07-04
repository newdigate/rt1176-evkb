# RT1176 IntervalTimer (PIT) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Implement Teensyduino-compatible `IntervalTimer` (periodic µs interrupts on PIT1's 4 shared channels), and make QEMU derive the PIT clock from `CCM_CLOCK_ROOT2` so the model mirrors silicon instead of hardcoding 24 MHz.

**Architecture:** New `IntervalTimer.h`/`.cpp` in the core: a static 4-channel PIT1 pool shared across instances, a single shared ISR that demuxes by polling each channel's `TFLG`, and a runtime `pit_clock_hz()` that reads `CLOCK_ROOT2`. The QEMU `imxrt_ccm` gains a `bus_root_clk` output (mirroring the sibling `imxrt1060_ccm.c` `perclk` pattern) that feeds PIT1; the PIT model already reads `clock_get_hz` at arm time. Gate-first: the gate's clock-faithfulness check runs **red against the unmodified CCM** (fixed clock) and turns green once the CCM tracks ROOT2.

**Tech Stack:** C/C++ (Arduino core), ARM GCC 10.2.1 (`ARMGCC_DIR=/Applications/ARM_10`), CMake+Ninja, QEMU (`mimxrt1170-evk`), LinkServer, Saleae Logic 2.

## Confirmed constants (all read from SDK cm7 header / Zephyr / QEMU — do not re-derive)

- **PIT1** base `0x400D8000` (PIT2 `0x40CB0000`, unused). 4 channels @ `+0x100` stride `0x10`: `LDVAL(+0x0)`, `CVAL(+0x4,RO)`, `TCTRL(+0x8)`, `TFLG(+0xC)`. `MCR(+0x0)`: `FRZ`=bit0, `MDIS`=bit1. `TCTRL`: `TEN`=bit0, `TIE`=bit1. `TFLG`: `TIF`=bit0 (**W1C**).
- **Counting:** down from `LDVAL` to 0, fires every `LDVAL+1` input clocks ⇒ **`LDVAL = ticks − 1`**.
- **IRQ:** `PIT1_IRQn = 155` (PIT2=156). **One shared line per instance** — poll all 4 `TFLG`s in the ISR.
- **PIT1 clock gate:** `CCM_LPCG62_DIRECT` @ `0x40CC67C0` (kCLOCK_Pit1=62; LPCG base `0x6000`+62·`0x20`). Enable = write bit0=1.
- **PIT clock = BUS root `CCM_CLOCK_ROOT2_CONTROL` @ `0x40CC0100`**: `MUX`=bits[10:8], `DIV`=bits[7:0]; effective = `src/(DIV+1)`. **mux 0 = OscRC48MDiv2 = 24 MHz** (kCLOCK_BUS_ClockRoot_MuxOscRc48MDiv2=0). Today ⇒ **24 cycles/µs**. (Already in `imxrt1176.h:108`: `CCM_CLOCK_ROOT2_CONTROL`; `..._MUX(x)`/`..._DIV(x)` at lines 110–111.)
- **NVIC/core:** `attachInterruptVector(irq, fn)` (in `core_pins.h`), `NVIC_ENABLE_IRQ`/`NVIC_SET_PRIORITY` (in `imxrt1176.h`). Default user priority **128** (lower = higher; serial 64, I2C 16).
- **Teensy API** (`cores/teensy4/IntervalTimer.h`): min 17 cycles (`LDVAL ≥ 17` ⇒ ticks ≥ 18); `operator IRQ_NUMBER_t` returns the PIT IRQ if allocated else `NVIC_NUM_INTERRUPTS`.
- **QEMU:** template `hw/misc/imxrt1060_ccm.c` (`perclk` = `qdev_init_clock_out` + `clock_set_hz`/`clock_propagate` on register write + at `reset_hold`, `VMSTATE_CLOCK`). PIT model `hw/timer/imxrt_pit.c:66` reads `clock_get_hz(s->clk)` when a channel arms. Machine wires PIT clk at `hw/arm/fsl-imxrt1170.c:937`; CCM child created at `:410`, realized at `:953`.

---

### Task 1: Core — PIT1 registers + IRQ numbers

**Files:**
- Modify: `cores/imxrt1176/imxrt1176.h` (append PIT block)
- Modify: `cores/imxrt1176/core_pins.h` (IRQ enum + alias)

- [ ] **Step 1:** In `cores/imxrt1176/imxrt1176.h`, append (near the other CCM_LPCG defines is fine):

```c
/* ---- PIT1 (Periodic Interrupt Timer): 4 channels, combined IRQ 155 ------ */
#define PIT1_BASE          0x400D8000u
#define PIT1_MCR           (*(volatile uint32_t *)(PIT1_BASE + 0x00u))
#define PIT1_MCR_MDIS      (1u << 1)         /* module disable (clear to enable) */
typedef struct {
    volatile uint32_t LDVAL;    /* +0x0 load value                */
    volatile uint32_t CVAL;     /* +0x4 current value (read-only) */
    volatile uint32_t TCTRL;    /* +0x8 timer control             */
    volatile uint32_t TFLG;     /* +0xC timer flag (W1C)          */
} pit_channel_t;
#define PIT1_CHANNEL       ((pit_channel_t *)(PIT1_BASE + 0x100u))
#define PIT_TCTRL_TEN      (1u << 0)         /* timer enable          */
#define PIT_TCTRL_TIE      (1u << 1)         /* timer interrupt enable*/
#define PIT_TFLG_TIF       (1u << 0)         /* timeout flag (W1C)    */
/* PIT1 clock gate: CCM LPCG62 DIRECT (kCLOCK_Pit1=62 -> 0x6000 + 62*0x20). */
#define CCM_LPCG62_DIRECT  (*(volatile uint32_t *)0x40CC67C0u)
```

- [ ] **Step 2:** In `cores/imxrt1176/core_pins.h`, inside `enum IRQ_NUMBER_t` (before the closing `} IRQ_NUMBER_t;`), add a line, then add the alias just after the enum:

```c
    IRQ_PIT1 = 155, IRQ_PIT2 = 156,
```

```c
#define IRQ_PIT IRQ_PIT1   /* Teensy source-compat alias */
```

- [ ] **Step 3:** Verify: `grep -nE "PIT1_CHANNEL|CCM_LPCG62_DIRECT" cores/imxrt1176/imxrt1176.h` and `grep -nE "IRQ_PIT1 = 155|define IRQ_PIT " cores/imxrt1176/core_pins.h` → matches present. (`0x40CC67C0` should sit on the same LPCG stride as the existing `CCM_LPCG79_DIRECT 0x40CC69E0`.)
- [ ] **Step 4:** Commit: `cd cores && git add imxrt1176/imxrt1176.h imxrt1176/core_pins.h && git commit -m "imxrt1176: PIT1 register block + IRQ_PIT1/2 (155/156) for IntervalTimer"`

### Task 2: Core — IntervalTimer class

**Files:**
- Create: `cores/imxrt1176/IntervalTimer.h`
- Create: `cores/imxrt1176/IntervalTimer.cpp`

- [ ] **Step 1:** Write `cores/imxrt1176/IntervalTimer.h`:

```cpp
#pragma once
#include <stdint.h>
#include "imxrt1176.h"    // PIT1_*, CCM_*, NVIC_* macros
#include "core_pins.h"    // IRQ_PIT1, attachInterruptVector, NVIC_NUM_INTERRUPTS

class IntervalTimer {
public:
    IntervalTimer() : channel(-1), nvic_priority(128) {}
    ~IntervalTimer() { end(); }

    bool begin(void (*funct)(void), unsigned int us) { return beginPeriod(funct, (double)us); }
    bool begin(void (*funct)(void), int us)          { return us < 0 ? false : beginPeriod(funct, (double)us); }
    bool begin(void (*funct)(void), float us)        { return beginPeriod(funct, (double)us); }
    bool begin(void (*funct)(void), double us)       { return beginPeriod(funct, us); }

    void update(unsigned int us) { updatePeriod((double)us); }
    void update(int us)          { if (us >= 0) updatePeriod((double)us); }
    void update(float us)        { updatePeriod((double)us); }
    void update(double us)       { updatePeriod(us); }

    void end();
    void priority(uint8_t n);
    operator IRQ_NUMBER_t() { return channel >= 0 ? IRQ_PIT1 : (IRQ_NUMBER_t)NVIC_NUM_INTERRUPTS; }

private:
    int channel;              // 0..3 when allocated, -1 otherwise
    uint8_t nvic_priority;
    bool beginPeriod(void (*funct)(void), double us);
    void updatePeriod(double us);
    static uint32_t ldvalFromMicros(double us);  // returns LDVAL, 0 == invalid
};
```

- [ ] **Step 2:** Write `cores/imxrt1176/IntervalTimer.cpp`:

```cpp
#include "IntervalTimer.h"

#define PIT_NUM_CH 4

static void (*volatile pit_callback[PIT_NUM_CH])(void);
static uint8_t pit_priority[PIT_NUM_CH];
static bool    pit_used[PIT_NUM_CH];
static bool    pit_isr_installed = false;

/* Effective PIT1 input clock = BUS root (CLOCK_ROOT2), derived at runtime so it
 * tracks whatever startup/clocks program. Mirror of imxrt_ccm's bus_root_clk. */
static uint32_t pit_clock_hz(void)
{
    uint32_t ctrl = CCM_CLOCK_ROOT2_CONTROL;
    uint32_t mux  = (ctrl >> 8) & 0x7u;
    uint32_t div  = (ctrl & 0xFFu) + 1u;
    uint32_t src;
    switch (mux) {
    case 0:  src = 24000000u; break;   /* OscRC48MDiv2 (BUS mux 0) */
    default: src = 24000000u; break;   /* only mux 0 modelled; extend when BUS is re-routed */
    }
    return src / div;
}

static void pit_isr(void)
{
    for (int ch = 0; ch < PIT_NUM_CH; ch++) {
        if (PIT1_CHANNEL[ch].TFLG & PIT_TFLG_TIF) {
            PIT1_CHANNEL[ch].TFLG = PIT_TFLG_TIF;      /* W1C before callback */
            if (pit_callback[ch]) pit_callback[ch]();
        }
    }
}

static void pit_apply_priority(void)
{
    uint8_t top = 255;
    for (int ch = 0; ch < PIT_NUM_CH; ch++)
        if (pit_used[ch] && pit_priority[ch] < top) top = pit_priority[ch];
    NVIC_SET_PRIORITY(IRQ_PIT1, top);
}

uint32_t IntervalTimer::ldvalFromMicros(double us)
{
    if (us <= 0.0) return 0;
    double count = us * (double)pit_clock_hz() / 1000000.0;   /* input clocks per period */
    if (count > 4294967295.0) return 0;                       /* must fit 32-bit LDVAL */
    uint32_t ticks = (uint32_t)(count + 0.5);                 /* round to nearest */
    if (ticks < 18u) return 0;                                /* Teensy min (LDVAL >= 17) */
    return ticks - 1u;
}

bool IntervalTimer::beginPeriod(void (*funct)(void), double us)
{
    if (funct == 0) return false;
    uint32_t ldval = ldvalFromMicros(us);
    if (ldval == 0) return false;

    if (channel < 0) {                                        /* allocate a free channel */
        for (int i = 0; i < PIT_NUM_CH; i++) {
            if (!pit_used[i]) { channel = i; pit_used[i] = true; break; }
        }
        if (channel < 0) return false;                        /* all 4 busy */
    }

    CCM_LPCG62_DIRECT = 1u;                                   /* gate PIT1 clock on */
    PIT1_MCR &= ~PIT1_MCR_MDIS;                               /* enable module */
    if (!pit_isr_installed) {
        attachInterruptVector(IRQ_PIT1, pit_isr);
        NVIC_ENABLE_IRQ(IRQ_PIT1);
        pit_isr_installed = true;
    }

    pit_callback[channel] = funct;
    pit_priority[channel] = nvic_priority;
    PIT1_CHANNEL[channel].TCTRL = 0;                          /* stop while configuring */
    PIT1_CHANNEL[channel].LDVAL = ldval;
    PIT1_CHANNEL[channel].TFLG  = PIT_TFLG_TIF;               /* clear stale flag */
    PIT1_CHANNEL[channel].TCTRL = PIT_TCTRL_TEN | PIT_TCTRL_TIE;
    pit_apply_priority();
    return true;
}

void IntervalTimer::updatePeriod(double us)
{
    if (channel < 0) return;
    uint32_t ldval = ldvalFromMicros(us);
    if (ldval == 0) return;
    PIT1_CHANNEL[channel].LDVAL = ldval;                     /* effective next reload */
}

void IntervalTimer::end()
{
    if (channel < 0) return;
    PIT1_CHANNEL[channel].TCTRL = 0;                         /* stop + disable IRQ */
    PIT1_CHANNEL[channel].TFLG  = PIT_TFLG_TIF;
    pit_callback[channel] = 0;
    pit_used[channel] = false;
    channel = -1;
    pit_apply_priority();
}

void IntervalTimer::priority(uint8_t n)
{
    nvic_priority = n;
    if (channel >= 0) { pit_priority[channel] = n; pit_apply_priority(); }
}
```

- [ ] **Step 3:** Confirm the core CMake picks up `IntervalTimer.cpp` (it globs `*.cpp` like `HardwareSerial.cpp`/`Wire.cpp`; if it uses an explicit source list, add `IntervalTimer.cpp`). Compilation is exercised by Task 3's build.
- [ ] **Step 4:** Commit: `cd cores && git add imxrt1176/IntervalTimer.h imxrt1176/IntervalTimer.cpp && git commit -m "imxrt1176: IntervalTimer (PIT1, 4 shared channels, runtime bus-clock derivation)"`

### Task 3: QEMU gate — interval_timer_test (RED: exposes the fixed-clock CCM gap)

**Files:**
- Create: `evkb/interval_timer_test/interval_timer_test.cpp` + `CMakeLists.txt` + `toolchain/` (copied) + `run_qemu_intervaltimer.sh`

- [ ] **Step 1:** Scaffold from the IRQ gate: `cd evkb && rm -rf interval_timer_test && mkdir interval_timer_test && cp -r irq_attach_test/toolchain interval_timer_test/ && sed 's/irq_attach_test/interval_timer_test/g' irq_attach_test/CMakeLists.txt > interval_timer_test/CMakeLists.txt`

- [ ] **Step 2:** Write `evkb/interval_timer_test/interval_timer_test.cpp`:

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "IntervalTimer.h"
#include "imxrt1176.h"     // CCM_CLOCK_ROOT2_CONTROL, CCM_CLOCK_ROOT_CONTROL_MUX/DIV

volatile uint32_t g_count = 0;
static void onTick() { g_count++; }

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    bool ok = true;

    // Check 1: fires (~100 ticks over 100 ms at 1000 us)
    IntervalTimer t1;
    bool b1 = t1.begin(onTick, 1000);
    g_count = 0; delay(100);
    uint32_t c1 = g_count;
    if (!b1 || c1 < 90 || c1 > 110) ok = false;

    // Check 4: frequency ratio (500 us ~ 2x)
    t1.update(500);
    g_count = 0; delay(100);
    uint32_t c2 = g_count;                     // expect ~200
    if (c2 < 180 || c2 > 220) ok = false;

    // Check 3: end() stops
    t1.end();
    g_count = 0; delay(50);
    uint32_t c3 = g_count;                     // expect 0
    if (c3 != 0) ok = false;

    // Check 2: channel exhaustion (4 succeed, 5th fails)
    IntervalTimer a, b, c, d, e;
    bool ba = a.begin(onTick, 1000), bb = b.begin(onTick, 1000);
    bool bc = c.begin(onTick, 1000), bd = d.begin(onTick, 1000);
    bool be = e.begin(onTick, 1000);           // no channel left
    if (!(ba && bb && bc && bd) || be) ok = false;
    a.end(); b.end(); c.end(); d.end();

    // Check 5: clock faithfulness — period preserved across a BUS-clock change.
    IntervalTimer t5;
    t5.begin(onTick, 1000);
    g_count = 0; delay(100);
    uint32_t cA = g_count; t5.end();           // ~100 at 24 MHz
    // Re-route BUS root (CLOCK_ROOT2) to DIV=1 (/2 -> 12 MHz), mux 0.
    CCM_CLOCK_ROOT2_CONTROL = CCM_CLOCK_ROOT_CONTROL_MUX(0) | CCM_CLOCK_ROOT_CONTROL_DIV(1);
    t5.begin(onTick, 1000);                    // re-arm: core reads 12 MHz; PIT should too
    g_count = 0; delay(100);
    uint32_t cB = g_count; t5.end();           // faithful: ~100; broken (fixed 24 MHz): ~200
    CCM_CLOCK_ROOT2_CONTROL = CCM_CLOCK_ROOT_CONTROL_MUX(0) | CCM_CLOCK_ROOT_CONTROL_DIV(0);
    if (cB < 80 || cB > 120) ok = false;       // period preserved

    Serial1.print("c1="); Serial1.print(c1);
    Serial1.print(" c2="); Serial1.print(c2);
    Serial1.print(" c3="); Serial1.print(c3);
    Serial1.print(" exhaust="); Serial1.print(be ? 1 : 0);
    Serial1.print(" cA="); Serial1.print(cA);
    Serial1.print(" cB="); Serial1.println(cB);
    Serial1.println(ok ? "IT=PASS" : "IT=FAIL");
}
void loop() {}
```

- [ ] **Step 3:** Write `evkb/interval_timer_test/run_qemu_intervaltimer.sh`:

```sh
#!/bin/sh
set -e
QEMU=~/Development/qemu2/build/qemu-system-arm
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/interval_timer_test.elf"; OUT="$DIR/it.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/it.dbg" &
P=$!; sleep 15; kill $P 2>/dev/null; wait $P 2>/dev/null || true   # guest delay() loops spin on DWT; 15s covers ~0.5s of guest delays
echo "==== captured ===="; cat "$OUT"
grep -q "IT=PASS" "$OUT" || { echo "FAIL: IntervalTimer"; exit 1; }
echo "PASS: IntervalTimer verified"
```

- [ ] **Step 4:** Build (core + gate) and run against the **unmodified** QEMU:

```
cd evkb/interval_timer_test && chmod +x run_qemu_intervaltimer.sh && export ARMGCC_DIR=/Applications/ARM_10 \
 && cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . \
 && cmake --build build && ./run_qemu_intervaltimer.sh 2>&1 | tail -6
```

Expected (RED): `c1≈100 c2≈200 c3=0 exhaust=0 cA≈100 cB≈200` → **`IT=FAIL`**. Checks 1–4 pass; **check 5 fails** because the unmodified `imxrt_ccm` ignores the ROOT2 DIV write (PIT stays 24 MHz while the core computes for 12 MHz) — this is the model gap Task 4 closes.
(If checks 1–4 also fail, investigate `qemu2/hw/timer/imxrt_pit.c` — the PIT model, not IntervalTimer.)

- [ ] **Step 5:** Commit the gate (red on the clock check): `cd evkb && git add interval_timer_test && git commit -m "interval-timer: QEMU gate (checks 1-4 pass; check 5 red — exposes fixed-clock CCM gap)"`

### Task 4: QEMU — derive PIT clock from CLOCK_ROOT2 (GREEN)

**Files:**
- Modify: `~/Development/qemu2/include/hw/misc/imxrt_ccm.h`
- Modify: `~/Development/qemu2/hw/misc/imxrt_ccm.c`
- Modify: `~/Development/qemu2/hw/arm/fsl-imxrt1170.c:936-937`

- [ ] **Step 1:** In `include/hw/misc/imxrt_ccm.h`, add the include (crib the exact spelling from `imxrt1060_ccm.h`) and the field:

```c
#include "hw/clock.h"
```

Add to `struct IMXRTCCMState` (after `obs_regs`):

```c
    Clock *bus_root_clk;      /* BUS root (CLOCK_ROOT2) output -> PIT1 "clk" */
```

- [ ] **Step 2:** In `hw/misc/imxrt_ccm.c`, add the clock include near the top includes:

```c
#include "hw/core/qdev-clock.h"
```

Add constants + the update helper (place above `imxrt_ccm_write`):

```c
/* BUS clock root: CLOCK_ROOT[2].CONTROL. MUX[10:8], DIV[7:0]; out = src/(DIV+1). */
#define CCM_CLOCK_ROOT2        0x100
#define CCM_ROOT_MUX_SHIFT     8
#define CCM_ROOT_MUX_MASK      0x7u
#define CCM_ROOT_DIV_MASK      0xFFu
#define CCM_OSC_RC48M_DIV2_HZ  24000000u   /* BUS mux 0 = OscRC48MDiv2 */

static void imxrt_ccm_update_bus_root(IMXRTCCMState *s)
{
    uint32_t ctrl = s->regs[CCM_CLOCK_ROOT2 / 4];
    uint32_t mux  = (ctrl >> CCM_ROOT_MUX_SHIFT) & CCM_ROOT_MUX_MASK;
    uint32_t div  = (ctrl & CCM_ROOT_DIV_MASK) + 1u;
    uint32_t src;

    switch (mux) {
    case 0:  src = CCM_OSC_RC48M_DIV2_HZ; break;   /* only mux 0 modelled today  */
    default: src = CCM_OSC_RC48M_DIV2_HZ; break;   /* extend when BUS is re-routed */
    }
    clock_set_hz(s->bus_root_clk, src / div);
    clock_propagate(s->bus_root_clk);
}
```

- [ ] **Step 3:** In `imxrt_ccm_write`, after the existing gate-reflect block, add:

```c
    if (offset == CCM_CLOCK_ROOT2) {
        imxrt_ccm_update_bus_root(s);
    }
```

In `imxrt_ccm_reset_hold`, after the two `memset`s, add:

```c
    imxrt_ccm_update_bus_root(s);   /* regs=0 -> BUS mux0/div0 -> 24 MHz */
```

In `imxrt_ccm_init`, after the OBS `sysbus_init_mmio`, add:

```c
    s->bus_root_clk = qdev_init_clock_out(DEVICE(obj), "bus_root_clk");
```

In `vmstate_imxrt_ccm.fields`, add before `VMSTATE_END_OF_LIST()`:

```c
        VMSTATE_CLOCK(bus_root_clk, IMXRTCCMState),
```

- [ ] **Step 4:** In `hw/arm/fsl-imxrt1170.c`, change the PIT clock wiring (the `for` loop at ~:936). Replace the `qdev_connect_clock_in(... "clk", s->gpt_clk);` line with:

```c
        /* PIT1 tracks the BUS clock root (CLOCK_ROOT2) via the CCM so its period
         * follows silicon; PIT2 stays on the fixed default (its root is unmodelled). */
        Clock *pit_clk = (i == 0) ? s->ccm.bus_root_clk : s->gpt_clk;
        qdev_connect_clock_in(DEVICE(&s->pit[i]), "clk", pit_clk);
```

- [ ] **Step 5:** Rebuild QEMU: `cd ~/Development/qemu2/build && ninja qemu-system-arm 2>&1 | tail -3` → links, no error.
- [ ] **Step 6:** Re-run the gate (no rebuild of guest needed): `cd ~/Development/rt1170/evkb/interval_timer_test && ./run_qemu_intervaltimer.sh 2>&1 | tail -6`. Expected (GREEN): `... cA≈100 cB≈100` → **`IT=PASS`**. (cB moved from ~200 to ~100: the PIT now tracks the /2 bus clock, so the requested 1000 µs period is preserved.)
- [ ] **Step 7:** Commit: `cd ~/Development/qemu2 && git add include/hw/misc/imxrt_ccm.h hw/misc/imxrt_ccm.c hw/arm/fsl-imxrt1170.c && git commit -m "imxrt_ccm: derive BUS root (CLOCK_ROOT2) clock; feed PIT1 so its period tracks silicon"`

### Task 5: Hardware verification (Saleae + board)

**Files:** add a HW variant of the sketch that toggles a header pin in the callback (reuse `interval_timer_test`, or a small `interval_timer_hw` sketch).

- [ ] **Step 1:** Add a pin-toggle in the callback for scope measurement, e.g. `static void onTick(){ g_count++; digitalToggleFast? }` — since the core may lack `digitalToggleFast`, use: keep `g_count`, and in the callback do `digitalWrite(LED_BUILTIN, (g_count ^= 1))` on a spare header pin (pick a Saleae-wired pin, e.g. D9 as used by `pwm_test`). Set `pinMode(pin, OUTPUT)` in setup and `begin(onTick, 1000)`.
- [ ] **Step 2:** Flash + capture VCOM: `cd evkb/interval_timer_test && cmake --build build && pkill -f LinkServer; sleep 2; /Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/interval_timer_test.elf` (background); read `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200 with pyserial (never `cat`). Expect `IT=PASS`.
- [ ] **Step 3:** Saleae: capture the toggled pin, confirm 1000 µs period ⇒ **500 Hz** square wave (toggle halves the callback rate); also `update(50)` ⇒ 10 kHz. Reference `evkb/pwm_test/measure_pwm.py` (`from saleae import automation`, `127.0.0.1:10430`, `TimedCaptureMode`, export raw digital CSV).
- [ ] **Step 4:** Record measured frequency (observational, no commit). **Do not** run the check-5 live bus-clock reprogram on HW (it disturbs every ROOT2 peripheral) — that check stays QEMU-only.

### Task 6: Finish

- [ ] **Step 1:** Regression — all QEMU gates PASS: `evkb/interval_timer_test/run_qemu_intervaltimer.sh`, `evkb/irq_attach_test/run_qemu_irq.sh`, `evkb/wire_master_test/run_qemu_wire.sh`, `evkb/spi_loopback_test/run_qemu_spi.sh`.
- [ ] **Step 2:** Push: `cd cores && git push`; `cd ~/Development/qemu2 && git push`; `cd ~/Development/rt1170/evkb && git push` (if it has a remote).
- [ ] **Step 3:** Memory note `rt1176-intervaltimer-pit.md`: PIT1 @ 0x400D8000, IRQ 155 shared across 4 channels (poll TFLG); LDVAL=ticks−1; clock = BUS root CLOCK_ROOT2 (mux0 OscRC48MDiv2 = 24 MHz), gate LPCG62 @ 0x40CC67C0; runtime `pit_clock_hz()` mirrors the QEMU `imxrt_ccm` `bus_root_clk` (both decode ROOT2 — the model-mirrors-silicon contract); gate check 5 (reprogram ROOT2 DIV → period preserved) is what exposed/closed the fixed-clock CCM gap. One-line pointer in MEMORY.md. Link `[[rt1176-gpio-irq-cm7-trap]]` (shared NVIC/attachInterruptVector machinery).
- [ ] **Step 4:** superpowers:finishing-a-development-branch.

---

## Self-review (author checklist — done)
- **Spec coverage:** PIT regs + IRQ (T1); full Teensy API `begin`/`update`/`end`/`priority`/`operator` + 4-channel pool + shared `TFLG`-demux ISR + runtime `pit_clock_hz()` (T2); gate 5 checks incl. exhaustion + clock faithfulness (T3); QEMU CCM ROOT2 derivation + PIT1 rewire, PIT2/GPT left fixed (T4); Saleae/HW with the live-reprogram caveat (T5); regression/push/memory (T6). All spec sections mapped.
- **Placeholder scan:** none — LPCG62 (`0x40CC67C0`), IRQ 155/156, base `0x400D8000`, mux-0 24 MHz all concrete; QEMU code mirrors `imxrt1060_ccm.c`.
- **Type consistency:** `pit_channel_t`/`PIT1_CHANNEL`/`PIT_TCTRL_TEN|TIE`/`PIT_TFLG_TIF` (T1) used verbatim in T2; `ldvalFromMicros`/`beginPeriod`/`updatePeriod`/`pit_apply_priority`/`pit_clock_hz`/`pit_isr` consistent within T2; `IRQ_PIT1` (T1) used in T2 + `operator`; QEMU `bus_root_clk` field (T4 Step1) matches its uses (T4 Steps 2–4) and `s->ccm.bus_root_clk` (T4 Step4).
- **Deferrals verified in-plan:** core-CMake source pickup (T2 S3); `hw/clock.h` include spelling cribbed from `imxrt1060_ccm.h` (T4 S1); runner `sleep 15` sized for guest DWT-spin delays (T3 S3).
- **TDD/gate-first:** check 5 runs red against unmodified QEMU (T3 S4) → green after the CCM fix (T4 S6) — the model gap is genuinely test-driven.
