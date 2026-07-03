# RT1176 `Wire` I²C Slave (Stage B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add interrupt-driven I²C **slave** mode to `Wire` (`begin(addr)` / `onReceive` / `onRequest`) on the RT1176 LPI2C, verified by a QEMU two-instance loopback gate and on the EVKB with an Arduino MKR Zero as bus master.

**Architecture:** The RT1176 LPI2C slave block signals address-match / RX / TX / STOP via the peripheral IRQ; the firmware ISR drains `SRDR` into a buffer, fires `onReceive(n)` at STOP, and calls `onRequest()` to feed `STDR` when the master reads. The QEMU LPI2C model gains a **slave persona**: a child `I2CSlave` object whose `event`/`recv`/`send` callbacks shadow the model's slave register block and raise the peripheral IRQ. A test machine wires instance B's slave persona onto instance A's master bus, so one firmware sketch drives A(master)→B(slave) as a self-contained loopback.

**Tech stack:** C/C++ core (register-poke), QEMU device model + `I2CSlave` QOM, existing `wire_master_test` harness pattern.

**Spec:** `docs/superpowers/specs/2026-07-02-rt1176-lpi2c-wire-design.md` (Stage B). **Depends on:** Stage A (committed: `Wire` master on LPI2C1, QEMU model + gate).

---

## Ground-truth constants (verified)

**LPI2C slave register offsets** (from LPI2C base; `PERI_LPI2C.h`):
| Reg | Off | Notes |
|---|---|---|
| SCR | `0x110` | SEN=bit0, RST=bit1, FILTEN=bit4, RTF=bit8, RRF=bit9 |
| SSR | `0x114` | TDF=bit0, RDF=bit1, AVF=bit2, TAF=bit3, RSF=bit8, SDF=bit9, BEF=bit10, FEF=bit11, AM0F=bit12, SBF=bit24, BBF=bit25 |
| SIER | `0x118` | TDIE=bit0, RDIE=bit1, AVIE=bit2, RSIE=bit8, SDIE=bit9, AM0IE=bit12 |
| SCFGR1 | `0x124` | ADRSTALL=bit0, ACKSTALL=bit3, SAEN=bit9, TXCFG=bit10, ADDRCFG=bits[18:16] |
| SCFGR2 | `0x128` | filter/hold timing (leave 0 for QEMU; SDK computes for HW) |
| SAMR | `0x140` | ADDR0=bits[10:1] (7-bit addr << 1) |
| SASR | `0x150` | RADDR=bits[10:0] (received addr incl R/W), ANV=bit14; read-only |
| STAR | `0x154` | TXNACK=bit0 |
| STDR | `0x160` | tx data[7:0]; write-only |
| SRDR | `0x170` | rx data[7:0]; read-only |

**LPI2C1** base `0x40104000`, IRQ 32 (already defined). **LPI2C2** base `0x40108000`, IRQ 33, CLOCK_ROOT38 @ `0x40CC1300` (=`0x40CC0000+38*0x80`), LPCG99 @ `0x40CC6C60` (=`0x40CC6000+99*0x20`).

**ISR install:** `attachInterruptVector(IRQ_NUMBER_t, void(*)(void))`, `NVIC_SET_PRIORITY(irq,p)`, `NVIC_ENABLE_IRQ(irq)` (macros in `core_pins.h`/`imxrt1176.h`).

**QEMU `I2CSlaveClass`** (`include/hw/i2c/i2c.h`): `int event(I2CSlave*, enum i2c_event)` (I2C_START_SEND / I2C_START_RECV / I2C_FINISH / I2C_NACK; return nonzero to NAK), `int send(I2CSlave*, uint8_t)` (master→slave, return 0=ACK), `uint8_t recv(I2CSlave*)` (slave→master). Template: `hw/sensor/tmp105.c`. Parent `TYPE_I2C_SLAVE`.

**QEMU model today** (`imxrt_lpi2c.{c,h}`): master-only (`mcr/msr/...`), owns `I2CBus *bus`. No slave regs.

---

## File structure

**QEMU (`~/Development/qemu2`):**
- `include/hw/i2c/imxrt_lpi2c.h` — add slave shadow regs + `I2CSlave` child pointer + FIFOs.
- `hw/i2c/imxrt_lpi2c.c` — slave-register MMIO read/write; new `TYPE_IMXRT_LPI2C_SLAVE` child (event/recv/send → shadow regs + raise IRQ); create/realize child.
- `hw/arm/mimxrt1170-evk.c` — loopback: attach `lpi2c[1]` slave persona to `lpi2c[0].bus` (keep the AT24C on `lpi2c[0]` — the master gate still works; slave persona only activates when firmware sets `SCR.SEN`).

**Firmware (`~/Development/rt1170/evkb/cores/imxrt1176`):**
- `tools/gen_imxrt1176_h.py` + regen `imxrt1176.h` — LPI2C1 slave regs + LPI2C2 master+slave regs + LPI2C2 clock/pins + `IRQ_LPI2C2`.
- `Wire.h` — add slave API + fields + ISR method.
- `Wire.cpp` — slave `begin(addr)`, `onReceive`, `onRequest`, ISR.
- `Wire_instances.cpp` — add `Wire1` (LPI2C2) instance + ISR trampolines.

**Sketch (`~/Development/rt1170/evkb`):**
- `wire_slave_test/` — loopback gate (`Wire` master ↔ `Wire1` slave) + CMake + runner.

---

## Task 1: QEMU — slave register shadow block in the LPI2C model

**Files:** Modify `~/Development/qemu2/include/hw/i2c/imxrt_lpi2c.h`, `~/Development/qemu2/hw/i2c/imxrt_lpi2c.c`

- [ ] **Step 1: Add slave state to the header struct**

In `struct IMXRTLPI2CState`, after the master fields, add:
```c
    /* Slave persona (Stage B) */
    I2CSlave *slave;          /* child I2CSlave, attachable to another instance's bus */
    uint32_t scr, ssr, sier, scfgr1, scfgr2, samr, sasr, star;
    Fifo8 slave_rx;           /* bytes master wrote to us, drained via SRDR */
    bool slave_selected;      /* addressed this transfer */
```
Add register offset + bit defines near the master ones (SCR 0x110 … SRDR 0x170, SSR bits, SCR_SEN, etc. per the table above). Add `#define TYPE_IMXRT_LPI2C_SLAVE "imxrt.lpi2c-slave"`.

- [ ] **Step 2: Handle slave-register MMIO in read/write**

In `imxrt_lpi2c_read`, add cases: SCR→`s->scr`, SSR→compute (TDF set when addressed-for-read, RDF set when `slave_rx` non-empty, plus latched AVF/SDF), SIER→`s->sier`, SAMR→`s->samr`, SASR→`s->sasr` (read clears AVF), SRDR→pop `slave_rx` (bit8 RXEMPTY when empty), SCFGR1/2→stored.
In `imxrt_lpi2c_write`: SCR (SEN/RST — RST flushes `slave_rx`), SIER (store + update IRQ), SAMR (store address), SCFGR1/2 (store), STDR (push to a pending tx byte for `recv`), STAR (ACK/NACK), SSR (W1C the latched flags).

- [ ] **Step 3: Add a slave IRQ helper**

```c
static void imxrt_lpi2c_slave_update_irq(IMXRTLPI2CState *s) {
    bool active = ((s->sier & SIER_RDIE) && (s->ssr & SSR_RDF))
               || ((s->sier & SIER_TDIE) && (s->ssr & SSR_TDF))
               || ((s->sier & SIER_AVIE) && (s->ssr & SSR_AVF))
               || ((s->sier & SIER_SDIE) && (s->ssr & SSR_SDF));
    /* OR with the existing master irq condition; raise s->irq if either set */
    qemu_set_irq(s->irq, active || imxrt_lpi2c_master_irq_active(s));
}
```
Refactor the existing master IRQ raise into `imxrt_lpi2c_master_irq_active()` so both personas share `s->irq`.

- [ ] **Step 4: Build QEMU**

Run: `ninja -C ~/Development/qemu2/build qemu-system-arm 2>&1 | tail -3`
Expected: links clean. (No behavior change yet — firmware doesn't touch slave regs, and no slave is attached.)

- [ ] **Step 5: Regression — Stage A gate still green**

Run: `cd ~/Development/rt1170/evkb/wire_master_test && sh run_qemu_wire.sh 2>&1 | grep -iE "PASS|FAIL"`
Expected: `PASS` (master path untouched).

- [ ] **Step 6: Commit** (`qemu2`): `hw/i2c/imxrt_lpi2c: add LPI2C slave register block (shadow)`.

---

## Task 2: QEMU — the `I2CSlave` child persona (event/recv/send)

**Files:** Modify `~/Development/qemu2/hw/i2c/imxrt_lpi2c.c`, `~/Development/qemu2/hw/arm/fsl-imxrt1170.c`

- [ ] **Step 1: Define the child I2CSlave type**

A small `I2CSlave` subtype holding a back-pointer to its owner `IMXRTLPI2CState`:
```c
struct IMXRTLPI2CSlave { I2CSlave parent_obj; IMXRTLPI2CState *ctrl; };
OBJECT_DECLARE_SIMPLE_TYPE(IMXRTLPI2CSlave, IMXRT_LPI2C_SLAVE)
```
Callbacks (only active when the owner's `SCR.SEN` is set — otherwise NAK so it's invisible to the master gate):
```c
static int slave_event(I2CSlave *i2c, enum i2c_event ev) {
    IMXRTLPI2CState *s = IMXRT_LPI2C_SLAVE(i2c)->ctrl;
    if (!(s->scr & SCR_SEN)) return 1;              /* not in slave mode -> NAK */
    switch (ev) {
    case I2C_START_SEND:  s->ssr |= SSR_AVF; s->sasr = /*RADDR=addr<<1|0*/; s->slave_selected = true; break;
    case I2C_START_RECV:  s->ssr |= SSR_AVF | SSR_TDF; s->sasr = /*RADDR|1*/; s->slave_selected = true; break;
    case I2C_FINISH:      s->ssr |= SSR_SDF; s->slave_selected = false; break;
    case I2C_NACK:        break;
    }
    imxrt_lpi2c_slave_update_irq(s);
    return 0;
}
static int slave_send(I2CSlave *i2c, uint8_t data) {   /* master writes a byte to us */
    IMXRTLPI2CState *s = IMXRT_LPI2C_SLAVE(i2c)->ctrl;
    if (!(s->scr & SCR_SEN)) return 1;
    if (fifo8_num_used(&s->slave_rx) < 256) fifo8_push(&s->slave_rx, data);
    s->ssr |= SSR_RDF;
    imxrt_lpi2c_slave_update_irq(s);
    return 0;                                          /* ACK */
}
static uint8_t slave_recv(I2CSlave *i2c) {             /* master reads a byte from us */
    IMXRTLPI2CState *s = IMXRT_LPI2C_SLAVE(i2c)->ctrl;
    uint8_t v = s->slave_tx_pending;                   /* last byte firmware wrote to STDR */
    s->ssr |= SSR_TDF;                                 /* ask firmware for the next byte */
    imxrt_lpi2c_slave_update_irq(s);
    return v;
}
```
Register the type with `.parent = TYPE_I2C_SLAVE`, class_init setting `k->event/send/recv`.

- [ ] **Step 2: Create the child in the model, wire the back-pointer**

In `imxrt_lpi2c_init`: `object_initialize_child(OBJECT(dev), "slave", &s->slave_obj, TYPE_IMXRT_LPI2C_SLAVE);` and set `IMXRT_LPI2C_SLAVE(&s->slave_obj)->ctrl = s;`. Expose `s->slave = I2C_SLAVE(&s->slave_obj);`. Do NOT realize it here (the machine realizes it onto a chosen bus, or leaves it unrealized/detached).

- [ ] **Step 3: Build + Stage A regression**

Run: `ninja -C ~/Development/qemu2/build qemu-system-arm 2>&1 | tail -2` then the master gate. Expected: builds; master gate PASS (slave persona NAKs when `SEN`=0, so the EEPROM path is unaffected even if attached).

- [ ] **Step 4: Commit** (`qemu2`): `hw/i2c/imxrt_lpi2c: add I2CSlave child persona (event/recv/send)`.

---

## Task 3: QEMU — loopback wiring in the machine

**Files:** Modify `~/Development/qemu2/hw/arm/mimxrt1170-evk.c`

- [ ] **Step 1: Attach LPI2C2's slave persona onto LPI2C1's bus**

After the EEPROM attach, add:
```c
    /* Stage B loopback: LPI2C2's slave persona lives on LPI2C1's bus, so firmware
     * can drive Wire(LPI2C1, master) -> Wire1(LPI2C2, slave) in one image. The
     * persona NAKs unless firmware sets SCR.SEN, so the master EEPROM gate is
     * unaffected. */
    {
        IMXRTLPI2CState *m = &FSL_IMXRT1170(dev)->lpi2c[0];
        IMXRTLPI2CState *sl = &FSL_IMXRT1170(dev)->lpi2c[1];
        i2c_slave_realize_and_unref(sl->slave, m->bus, &error_fatal);
    }
```

- [ ] **Step 2: Build + Stage A regression**

Run: rebuild QEMU + master gate. Expected: PASS (persona attached but inert until SEN set).

- [ ] **Step 3: Commit** (`qemu2`): `hw/arm/mimxrt1170-evk: loopback LPI2C2 slave on LPI2C1 bus for Wire slave gate`.

---

## Task 4: Firmware — generate LPI2C1 slave + LPI2C2 (master+slave) defs

**Files:** Modify `tools/gen_imxrt1176_h.py`; regenerate `imxrt1176.h`; add `IRQ_LPI2C2` to `core_pins.h`

- [ ] **Step 1: Add the register block** to the generator (before the final `#endif`):
```python
    L += ["",
          "/* LPI2C1 slave register block */",
          "#define LPI2C1_SCR   (*(volatile uint32_t *)0x40104110u)",
          "#define LPI2C1_SSR   (*(volatile uint32_t *)0x40104114u)",
          "#define LPI2C1_SIER  (*(volatile uint32_t *)0x40104118u)",
          "#define LPI2C1_SCFGR1 (*(volatile uint32_t *)0x40104124u)",
          "#define LPI2C1_SCFGR2 (*(volatile uint32_t *)0x40104128u)",
          "#define LPI2C1_SAMR  (*(volatile uint32_t *)0x40104140u)",
          "#define LPI2C1_SASR  (*(volatile uint32_t *)0x40104150u)",
          "#define LPI2C1_STAR  (*(volatile uint32_t *)0x40104154u)",
          "#define LPI2C1_STDR  (*(volatile uint32_t *)0x40104160u)",
          "#define LPI2C1_SRDR  (*(volatile uint32_t *)0x40104170u)",
          "/* LPI2C2 (loopback slave for QEMU gate), base 0x40108000 */",
          "#define LPI2C2_MCR    (*(volatile uint32_t *)0x40108010u)",
          "#define LPI2C2_MSR    (*(volatile uint32_t *)0x40108014u)",
          "#define LPI2C2_MIER   (*(volatile uint32_t *)0x40108018u)",
          "#define LPI2C2_MCFGR1 (*(volatile uint32_t *)0x40108024u)",
          "#define LPI2C2_MCCR0  (*(volatile uint32_t *)0x40108048u)",
          "#define LPI2C2_MTDR   (*(volatile uint32_t *)0x40108060u)",
          "#define LPI2C2_MRDR   (*(volatile uint32_t *)0x40108070u)",
          "#define LPI2C2_SCR   (*(volatile uint32_t *)0x40108110u)",
          "#define LPI2C2_SSR   (*(volatile uint32_t *)0x40108114u)",
          "#define LPI2C2_SIER  (*(volatile uint32_t *)0x40108118u)",
          "#define LPI2C2_SCFGR1 (*(volatile uint32_t *)0x40108124u)",
          "#define LPI2C2_SCFGR2 (*(volatile uint32_t *)0x40108128u)",
          "#define LPI2C2_SAMR  (*(volatile uint32_t *)0x40108140u)",
          "#define LPI2C2_SASR  (*(volatile uint32_t *)0x40108150u)",
          "#define LPI2C2_STAR  (*(volatile uint32_t *)0x40108154u)",
          "#define LPI2C2_STDR  (*(volatile uint32_t *)0x40108160u)",
          "#define LPI2C2_SRDR  (*(volatile uint32_t *)0x40108170u)",
          "#define CCM_CLOCK_ROOT38_CONTROL (*(volatile uint32_t *)0x40CC1300u)",
          "#define CCM_LPCG99_DIRECT        (*(volatile uint32_t *)0x40CC6C60u)"]
```
(LPI2C2 pins are not needed for the QEMU loopback — it uses no physical pads. HW slave uses LPI2C1's existing pins.)

- [ ] **Step 2: Regenerate + add IRQ**

Run: `cd ~/Development/rt1170/evkb/cores/imxrt1176 && python3 tools/gen_imxrt1176_h.py`. Add `IRQ_LPI2C2 = 33,` after `IRQ_LPI2C1 = 32,` in `core_pins.h`.

- [ ] **Step 3: Verify** `grep -cE "LPI2C1_SCR|LPI2C2_SSR|CCM_LPCG99" imxrt1176.h` == 3; blink builds.

- [ ] **Step 4: Commit** (`cores`): `imxrt1176: generate LPI2C slave regs + LPI2C2 for slave loopback`.

---

## Task 5: Firmware — slave API in `Wire.h`

**Files:** Modify `~/Development/rt1170/evkb/cores/imxrt1176/Wire.h`

- [ ] **Step 1: Extend `hardware_t` and `TwoWire`**

Add to `hardware_t`: `volatile uint32_t &scr, &ssr, &sier, &samr, &sasr, &scfgr1, &stdr, &srdr;`.
Add to `TwoWire` public: `void begin(uint8_t address); void onReceive(void (*cb)(int)); void onRequest(void (*cb)(void)); void handle_slave_isr();`.
Add private: `uint8_t slave_addr; void (*on_receive)(int) = nullptr; void (*on_request)(void) = nullptr; uint8_t s_rx_buf[BUFFER_LENGTH]; uint8_t s_rx_len = 0; uint8_t s_tx_buf[BUFFER_LENGTH]; uint8_t s_tx_len = 0; uint8_t s_tx_idx = 0;`.
Note: `write()` during an `onRequest` callback must append to `s_tx_buf` (slave-response) rather than the master `tx_buf`; add a `bool in_slave_request = false;` flag and branch in `write()`.

- [ ] **Step 2: Syntax-check + commit** (`cores`): `imxrt1176: TwoWire slave-mode API (begin(addr)/onReceive/onRequest)`.

---

## Task 6: Firmware — slave engine + ISR in `Wire.cpp` and `Wire_instances.cpp`

**Files:** Modify `Wire.cpp`, `Wire_instances.cpp`

- [ ] **Step 1: `begin(uint8_t address)`**

```c
void TwoWire::begin(uint8_t address) {
    slave_addr = address; s_rx_len = 0; s_tx_len = 0; s_tx_idx = 0;
    hw->lpcg = 1u; hw->clock_root = hw->clock_root_val;
    hw->scl_mux = hw->scl_mux_val; hw->scl_pad = hw->pad_ctl_val;
    hw->sda_mux = hw->sda_mux_val; hw->sda_pad = hw->pad_ctl_val;
    hw->scl_select_input = hw->scl_select_val; hw->sda_select_input = hw->sda_select_val;
    hw->scr = 0x2u; hw->scr = 0u;                       // SCR.RST then clear
    hw->samr = ((uint32_t)address << 1);                // SAMR.ADDR0 = 7-bit<<1
    hw->scfgr1 = (1u << 9);                             // SCFGR1.SAEN (7-bit addr enable)
    hw->sier = (1u<<1) | (1u<<2) | (1u<<9);             // RDIE | AVIE | SDIE
    attachInterruptVector(hw->irq, hw->irq_handler);
    NVIC_SET_PRIORITY(hw->irq, hw->irq_priority);
    NVIC_ENABLE_IRQ(hw->irq);
    hw->scr = 0x1u;                                      // SCR.SEN
}
void TwoWire::onReceive(void (*cb)(int)) { on_receive = cb; }
void TwoWire::onRequest(void (*cb)(void)) { on_request = cb; }
```

- [ ] **Step 2: ISR handler**

```c
void TwoWire::handle_slave_isr() {
    uint32_t ssr = hw->ssr;
    if (ssr & (1u<<2)) {                        // AVF: address valid (new transfer)
        (void)hw->sasr;                         // read clears AVF; RADDR bit0 = R/W
        s_rx_len = 0;
    }
    if (ssr & (1u<<1)) {                        // RDF: master wrote a byte
        uint8_t d = (uint8_t)hw->srdr;
        if (s_rx_len < BUFFER_LENGTH) s_rx_buf[s_rx_len++] = d;
    }
    if (ssr & (1u<<0)) {                        // TDF: master wants a byte
        if (s_tx_idx == 0 && on_request) {      // first byte of a read -> fill response
            s_tx_len = 0; in_slave_request = true; on_request(); in_slave_request = false;
        }
        hw->stdr = (s_tx_idx < s_tx_len) ? s_tx_buf[s_tx_idx++] : 0xFF;
    }
    if (ssr & (1u<<9)) {                        // SDF: STOP -> transfer complete
        hw->ssr = (1u<<9);                      // W1C SDF
        if (s_rx_len && on_receive) on_receive(s_rx_len);
        s_rx_len = 0; s_tx_idx = 0; s_tx_len = 0;
    }
}
```
Update `write(uint8_t)` to append to `s_tx_buf` when `in_slave_request`. `read()`/`available()` return from `s_rx_buf` when in slave mode (track a `bool is_slave` set by `begin(addr)`).

- [ ] **Step 3: Add `Wire1` (LPI2C2) instance + ISR trampolines in `Wire_instances.cpp`**

Add a second `hardware_t lpi2c2_hw` (LPI2C2 regs, IRQ_LPI2C2, LPCG99/ROOT38; master fields from LPI2C2_*; slave fields from LPI2C2_S*). Provide static ISR trampolines: `static void wire_isr()  { Wire.handle_slave_isr(); }` and `static void wire1_isr() { Wire1.handle_slave_isr(); }`; set each `hardware_t.irq_handler` accordingly. `TwoWire Wire1(&lpi2c2_hw);` and `extern TwoWire Wire1;` in `Wire.h`.

- [ ] **Step 4: Build** the (Task 7) gate sketch; fix compile errors.

- [ ] **Step 5: Commit** (`cores`): `imxrt1176: Wire I2C slave engine + ISR + Wire1 instance`.

---

## Task 7: QEMU gate — `wire_slave_test` loopback

**Files:** Create `~/Development/rt1170/evkb/wire_slave_test/{wire_slave_test.cpp,CMakeLists.txt,run_qemu_wire_slave.sh}`

- [ ] **Step 1: Sketch** — `Wire1` = slave @0x42, `Wire` = master; master writes then reads; assert slave callbacks fired.
```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "Wire.h"
volatile int rx_count = 0; volatile uint8_t rx0 = 0;
void onRecv(int n) { rx_count = n; rx0 = Wire1.read(); }
void onReq() { Wire1.write((uint8_t)0x11); Wire1.write((uint8_t)0x22); Wire1.write((uint8_t)0x33); Wire1.write((uint8_t)0x44); }
void setup() {
    Serial1.begin(115200); while(!Serial1){}
    Wire1.begin((uint8_t)0x42); Wire1.onReceive(onRecv); Wire1.onRequest(onReq);   // slave
    Wire.begin(); Wire.setClock(100000);                                           // master
    Wire.beginTransmission(0x42); Wire.write((uint8_t)0xAB); uint8_t w = Wire.endTransmission();
    delayMicroseconds(200);
    Serial1.print("slave_rx_count="); Serial1.println(rx_count);
    Serial1.print("slave_rx0=0x"); Serial1.println(rx0, HEX);
    Serial1.print("master_wr="); Serial1.println(w);
    uint8_t got = Wire.requestFrom((uint8_t)0x42,(uint8_t)4,true);
    Serial1.print("master_rd("); Serial1.print(got); Serial1.print(")=");
    while (Wire.available()) { Serial1.print(Wire.read(), HEX); Serial1.print(' '); }
    Serial1.println();
}
void loop(){}
```

- [ ] **Step 2: CMakeLists.txt + runner** — mirror `wire_master_test` (copy, rename target to `wire_slave_test`, output `wire_slave.uart`). Gate greps:
```bash
grep -q "slave_rx_count=1"    "$OUT" || { echo FAIL rx_count; exit 1; }
grep -q "slave_rx0=0xAB"      "$OUT" || { echo FAIL rx0; exit 1; }
grep -q "master_wr=0"         "$OUT" || { echo FAIL wr; exit 1; }
grep -q "master_rd(4)=11 22 33 44 " "$OUT" || { echo FAIL rd; exit 1; }
echo "PASS: Wire I2C slave verified (onReceive + onRequest via loopback)"
```

- [ ] **Step 3: Build + run gate**

Run: `cd ~/Development/rt1170/evkb/wire_slave_test && cmake -B build -G Ninja . >/dev/null 2>&1 && cmake --build build 2>&1 | tail -2 && sh run_qemu_wire_slave.sh 2>&1 | grep -iE "PASS|FAIL"`
Expected: `PASS`. Debug via the printed values (rx_count/rx0/wr/rd) if not.

- [ ] **Step 4: Regression** — master gate + serial + adc still PASS.

- [ ] **Step 5: Commit** (`cores` + sketch repo): `test: wire_slave_test QEMU loopback gate (onReceive/onRequest)`.

---

## Task 8: Hardware — RT1170 slave ↔ MKR Zero master

**Files:** Create `~/Development/rt1170/evkb/wire_slave_hw/` (sketch) + a MKR Zero master sketch (provided to the user)

- [ ] **Step 1: RT1170 slave sketch** — `Wire.begin(0x42)` (LPI2C1, the Arduino header), `onReceive` mirrors received bytes over `Serial1`, `onRequest` sends `11 22 33 44`. (Reuses the HW-verified LPI2C1 pins from Stage A.)

- [ ] **Step 2: MKR Zero master sketch** (user uploads via Arduino IDE): `Wire.begin()`; loop writes a known byte to 0x42, then `requestFrom(0x42,4)` and prints over `SerialUSB`. Same wiring + pull-ups as Stage A's HW test.

- [ ] **Step 3: Flash RT1170, wire the boards, power-cycle**, capture both serials. Expected: RT1170 prints the byte the MKR wrote; MKR prints `11 22 33 44` read from the RT1170 slave.

- [ ] **Step 4: Commit** the HW sketch.

---

## Task 9: Finish

- [ ] Run all QEMU gates (master, slave, serial, adc, rx) — confirm green.
- [ ] superpowers:finishing-a-development-branch → push `cores` + `qemu2`.
- [ ] Update `[[rt1176-lpi2c-wire]]` memory with the slave register map + loopback approach; update the task list.

---

## Self-review

**Spec coverage (Stage B):** slave `begin(addr)`/`onReceive`/`onRequest` — Tasks 5/6. Interrupt-driven ISR (AVF/RDF/TDF/SDF) — Task 6. QEMU slave persona (I2CSlave child shadowing slave regs + IRQ) — Tasks 1/2. Two-instance loopback gate — Tasks 3/7. HW verify with MKR as master — Task 8. ✔

**Placeholder scan:** The QEMU persona `event`/`recv`/`send` bodies are the actual logic (SSR flag setting, FIFO push/pop, IRQ raise) — the one genuinely novel piece; `sasr`/`slave_tx_pending` fill-ins are named and their computation stated (RADDR = addr<<1 | R/W). Firmware code is complete with real register offsets/bits. No TBD/vague steps.

**Type consistency:** `hardware_t` slave fields (`scr/ssr/sier/samr/sasr/scfgr1/stdr/srdr`) match between `Wire.h`, `Wire_instances.cpp`, and `Wire.cpp`. SSR bit numbers (TDF=0, RDF=1, AVF=2, SDF=9) consistent between firmware ISR and QEMU persona. `Wire1` declared in `Wire.h`, defined in `Wire_instances.cpp`, used in the gate. Slave `endTransmission` status convention unchanged from Stage A.
