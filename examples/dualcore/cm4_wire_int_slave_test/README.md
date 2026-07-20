# cm4_wire_int_slave_test — Phase 4.2: CM4 interrupt-driven I2C slave

The CM4 runs an **interrupt-driven I2C slave** @`0x42`, distilled from the
HW-verified `TwoWire` slave via the shared `lpi2c1176` core
(`lpi2c1176_slave_config`/`lpi2c1176_slave_enable` + the `handle_slave_isr`
body). The CM4 NVIC-enables the LPI2C IRQ on its **own** NVIC (via the qemu2
per-line split-IRQ) and services the whole exchange in its ISR: it records the
master's write bytes (RDF), serves a fixed response byte (TDF, held by
TXDSTALL clock-stretch), and counts STOPs (SDF). The CM7 only boots the CM4
image and reports MU tokens on LPUART1/VCOM.

## World-split instance (spec §4.2)

No LPI2C instance is *both* QEMU-bus-bridged *and* EVKB-header-accessible, so
one instance-agnostic slave implementation is bound at build time to a
different instance per world:

| World | Instance | Base | IRQ | Master | Built target |
|-------|----------|------|-----|--------|--------------|
| QEMU gate | LPI2C2 persona (bridged onto LPI2C1's bus) | `0x40108000` | 33 | CM7 polled, on LPI2C1 | `cm4_wire_int_slave_test.elf` |
| HW probe | LPI2C1 (Arduino header) | `0x40104000` | 32 | **external** I2C master | `cm4_wire_int_slave_test_hw.elf` |

The slave logic and the protocol constants are identical in both worlds, so
the asserted tokens match. On silicon, LPI2C1's IRQ 32 reaches both NVICs
natively (RM Tables 4-1/4-2) — no qemu2 support is needed for the HW side.

## Protocol constants (both worlds)

The master writes `{0xA5, 0x5A, 0xC3}` (STOP), then reads 1 byte; the slave
responds `0x3C`.

## Tokens (MU ch0)

`ready=CAFE0001` (slave configured + enabled), then `{irqcnt, b0, b1, b2,
resp, err, done}`:

| Token | Expect | Meaning |
|-------|--------|---------|
| `irqcnt` | >0 | CM4 serviced its slave IRQ on its own NVIC (routing proof) |
| `b0/b1/b2` | `A5/5A/C3` | bytes the master wrote, captured by the CM4 ISR |
| `resp` | `3C` | byte the ISR loaded into `STDR` on TDF (deterministic both worlds) |
| `err` | `0` | `0` OK / `4` = QEMU wait-guard expired (stalled exchange) |
| `done` | `1` | CM4 sequence completed |

`WIRE_INT_SLAVE_CM4=PASS` requires `irqcnt>0`, `b0/b1/b2`, `resp=3C`, `err=0`,
`done=1`.

### Documented model limit (QEMU)

The **master-observed** read byte (`mrd`, and `wr`-side timing with it) is
**not** asserted in QEMU: qemu2's `imxrt_lpi2c` serves the master's `CMD_RXD`
synchronously on the CM7 vCPU with a `0xFF` empty-FIFO fallback and does not
model the TXDSTALL clock-stretch across vCPUs, so whether the CM4 ISR refills
`STDR` before the master latches races vCPU scheduling (observed both PASS and
`mrd=FF` on an identical binary). The **slave-side** `resp` token IS
deterministic and IS asserted (the CM4 always takes its pended TDF IRQ and
loads `STDR`). The master-observed byte is verified on hardware only, by the
external master's own serial printout (`rd=3C`) — this is the same "HW is the
only oracle for read-data paths" split as Phase 4.1's `rdv`.

## QEMU gate

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
./run_qemu.sh          # NOT `sh run_qemu.sh` (gate-lib re-exec quirk)
```

## HW probe (wired, external master)

Needs a **3.3V** I2C master board (Arduino MKR Zero or similar — the
`wire_slave_test` precedent) running `ext_master/ext_master.ino`.

### Wiring (EVKB Arduino header)

| External master | EVKB | Note |
|-----------------|------|------|
| SDA | **A4** (GPIO_AD_09) | |
| SCL | **A5** (GPIO_AD_08) | |
| GND | **GND** | **ESSENTIAL** — a floating ground was the Phase-3.2 I2C flakiness |

> ### ★ UNPLUG USB OTG2 FIRST
> `A5` = `GPIO_AD_08` = `LPI2C1_SCL` is **also** wired to `USB_OTG2_ID` on the
> EVKB. A USB OTG adapter on OTG2 grounds the ID pin, which clamps SCL to 0 V
> (a dead **0 Ω A5→GND even with the board powered off**) and makes LPI2C1 on
> the header fail silently — SCL stuck low, the slave never addressed, an
> Arduino master's `Wire` call hangs. This cost a full HW-debug session on
> 2026-07-19; see the `rt1176-a5-ad08-otg2-id-short` memory. A board-off short
> can never be firmware.

- **3.3V logic only.** The EVKB pads are not 5V-tolerant; never wire a 5V
  master without a level shifter.
- The EVKB pads carry internal pull-ups (pad_ctl `0x1E`). Add external
  2.2–4.7 kΩ pull-ups to 3V3 if the bus is marginal.
- Two **separate** transactions (STOP between write and read). The sketch uses
  `endTransmission()` (STOP) then `requestFrom()` — do not switch to a
  repeated START; the slave's `exchange_complete()` needs both STOPs.

### Procedure

1. `cmake --build build` (produces `cm4_wire_int_slave_test_hw.elf`).
2. `pkill LinkServer; pkill redlinkserv` before flashing (per the EVKB
   flashing note).
3. Start the pyserial VCOM reader (115200, `gtimeout`) **before** resetting the
   board (per the macOS serial-capture note — `cat` resets the baud to 9600).
4. Flash `build/cm4_wire_int_slave_test_hw.elf` (LinkServer). Reset.
5. Confirm `ready=CAFE0001` and the `EXT-MASTER: run ext_master now` prompt on
   the EVKB VCOM.
6. Power/run the external master (`ext_master.ino`).
7. Record BOTH serial streams:
   - EVKB VCOM: `irqcnt>0`, `b0=A5 b1=5A b2=C3`, `resp=3C`, `err=0`, `done=1`,
     `WIRE_INT_SLAVE_CM4=PASS`.
   - External master: `wr=0 rd=3C` — the HW-side oracle for the response byte.

`transcript_hw_evkb.txt` holds the probe output once run.
