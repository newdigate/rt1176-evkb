# CM4 Phase 3.3 — shared C register/clock core (SPI + Wire consolidation)

**Date:** 2026-07-18
**Status:** Approved (architecture locked 2026-07-18 during the Phase-3
brainstorming: "approach C") → ready for `writing-plans`
**Phase:** 3.3 — extract the LPSPI1/LPI2C5 register+clock sequences now
duplicated between the CM7 C++ classes (`newdigate/SPI` `SPIIMXRT1176.cpp`,
`newdigate/Wire` `WireIMXRT1176.cpp`) and the CM4 distilled C drivers
(`evkb/cm4_spi_test/cm4/main_cm4.c`, `evkb/cm4_wire_test/cm4/main_cm4.c`)
into a per-library **shared C core both worlds compile and call**.
**Skill:** governed by `cm4-bringup`. Guardrail (locked): **byte-identical
gate output** (the 2B `cmp` discipline applied to transcripts).

---

## 1. Goal & success criteria

One HW-verified sequence, one source file, two consumers:

- `~/Development/SPI/lpspi1176.{h,c}` — the LPSPI begin/end/clock-divider/
  polled-transfer sequences as freestanding C.
- `~/Development/Wire/lpi2c1176.{h,c}` — the LPI2C clocks+pins/begin/end/
  setClock/wait_flag/bus_recover/master-write/master-read sequences as
  freestanding C.
- `SPIClass` / `TwoWire` (CM7) delegate to them; the CM4 gate images compile
  the same files and call the same functions. The "Keep in sync with …"
  duplication headers die.

**v1 success =**
1. Both cm4 gate QEMU transcripts **byte-identical to the checked-in
   `transcript_qemu.txt`** (tokens, order, values — nothing about the MU/token
   contract changes).
2. CM7 library gates green and **byte-identical to pre-refactor baseline
   captures**: `SPI/tests/{spi_loopback_test, spi_dma_test, st7735_test}`,
   `Wire/tests/{wire_master_test, wire_slave_test, wire_oled_test}`.
3. Untouched CM4 images unchanged: `cm4_dual_test`'s `.cm4.bin` **cmp-identical**
   before/after the `teensy_add_cm4_image` macro edit (2B precedent).
4. `license-audit.sh` PASS with the new files covered.
5. Silicon anchor (see §6): wiring-free `cm4_wire_test` EVKB re-probe
   byte-identical to its checked-in HW transcript (incl. `rdv=00006243`).

**Scope guard — explicitly out:** no behavior change of any kind; no new
capability; no qemu2 change; CM4 slave/DMA/interrupt paths (deferred list
unchanged); consolidating the *SPI DMA* path (CM7-only, stays C++); the Wire
slave block (CM7-only: NVIC + ISR + `.fastrun`, stays C++); LPUART/other
peripherals (future phases can adopt the same pattern).

---

## 2. Triangulation findings (2026-07-18, side-by-side read)

Pairs compared line-by-line: `SPIIMXRT1176.cpp::{begin,setClockDividerHz,
transfer,transfer16}` vs `cm4_spi_test` C mirror; `WireIMXRT1176.cpp::{begin,
setClock,wait_flag,bus_recover,endTransmission,requestFrom}` vs
`cm4_wire_test` C mirror.

**No semantic drift — the "two sources disagree" trigger does NOT fire.**
Every register/clock/pin **value** and every **write order** matches. The
distillation deltas, all previously known/reviewed:

| # | CM7 library | CM4 mirror | Verdict |
|---|---|---|---|
| D1 | `setClockDividerHz`/`setClock` write `CR/MCR=0` before CCR/MCFGR1+MCCR0 (MEN save/restore; MEN is already 0 in the begin path) | write skipped | inert extra write of the current value; part of the CM7 HW-verified stream on the SAME instances |
| D2 | divider math (`prescale/sckdiv` loop; `pre/div/clklo/clkhi` @ src 24 MHz) | precomputed `SCKDIV=4`, `MCCR0=0x1818303F`, `MCFGR1=0x1` | arithmetic equal (re-verified: 24e6/4e6→sckdiv 4; 100 kHz→pre 1, clklo 63, clkhi 48, DATAVD/SETHOLD 24) |
| D3 | `MCFGR1 = (MCFGR1&~7)\|1` RMW | `MCFGR1 = 0x1` direct | equal because MCFGR1 post-reset = 0 (silicon-proven on LPI2C5 by every CM7 `begin()` HW run) |
| D4 | Wire read: `if (sendStop)` guards the early-NACK STOP | unconditional STOP | equal at the gate's call site (sendStop=1) |
| D5 | timeout returns 0xFF/0xFFFF (width-cast) | returns 0xFFFFFFFF, caller masks | equal after the cast |

**Resolution rule for the shared core: the CM7 logic verbatim wins** (it is
the general form; D1–D5 all collapse into it). Consequence: the CM4 images
gain D1's inert writes and D3's RMW — analyzed in §6.

Build facts that shape the design:
- `teensy_add_cm4_image` compiles each source by absolute path with **no
  include directories** and no `-D` platform defines (`-mcpu=cortex-m4
  -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -O2 -ffreestanding -fno-common
  -ffunction-sections -fdata-sections -Wall -Wextra -MMD`).
- `import_arduino_library` globs `${LIB_ROOT}/*.c` with `CONFIGURE_DEPENDS`
  (staleness fix 2026-07-15) → the new lib `.c` files enter every consumer
  build automatically; unreferenced archive members cost nothing on
  non-1176 platforms (the files are freestanding portable C, no guard needed).
- Struct blast radius is exactly the four `*IMXRT1176.{h,cpp}` files —
  `SPI.h/SPI.cpp` references are the guarded non-1176 platform branches.
- `license-audit.sh` already sweeps `$HOME/Development/SPI` + `Wire`
  (part 1) and walks cm4 `.o.d` depfiles (part 2, since the 3.2 follow-up).

---

## 3. Design

### 3.1 Shared files (per library, MIT, our own HW-verified code)

`lpspi1176.h` / `lpi2c1176.h` are **freestanding C11, no includes beyond
`<stdint.h>/<stddef.h>`** (they must compile under the CM4 image flags and
inside the C++ libraries). Each defines:

1. **A register overlay** (`lpspi1176_regs_t`, `lpi2c1176_regs_t`) with the
   documented offsets (LPSPI CR 0x10 … RDR 0x74; LPI2C VERID 0x00 … MRDR
   0x70 — master block only; the slave block stays on `IMXRT_LPI2C_t`),
   pinned by `_Static_assert(offsetof(...))` in the header, **and
   cross-asserted against `IMXRT_LPSPI_t`/`IMXRT_LPI2C_t` in the library
   .cpp** (which sees both headers) — drift breaks the CM7 build.
2. **A hardware-description struct** (`lpspi1176_hw_t`, `lpi2c1176_hw_t`):
   pointers + values mirroring today's `SPI_Hardware_t`/`I2C_Hardware_t`
   register-reference fields (lpcg, clock_root+val, per-pin mux ptr+val,
   pad ptr, select ptr+val, pad_ctl_val, func_clock for SPI).
3. **The bit macros** (CR/CFGR1/TCR/RSR; MCR/MSR/MTDR-cmd/MRDR) — moved, not
   duplicated: the .cpp/.c copies are deleted.
4. **The sequence functions** (bodies = CM7 logic verbatim):
   - SPI: `lpspi1176_begin(regs, hw, clock_hz, *tcr_base)`,
     `lpspi1176_end(regs, hw)`,
     `lpspi1176_set_clock_hz(regs, func_clock, clock_hz, *tcr_base)`,
     `lpspi1176_transfer_frame(regs, tcr_base, data, framesz)` →
     raw RDR or 0xFFFFFFFF on timeout.
   - Wire: `lpi2c1176_clocks_pins(hw)`,
     `lpi2c1176_begin(regs, hw, clock_hz)` (= clocks_pins + MCR RST/0 +
     set_clock + MEN), `lpi2c1176_end(regs, hw)`,
     `lpi2c1176_set_clock(regs, hz)`,
     `lpi2c1176_wait_flag(regs, mask, error_mask, *err)`,
     `lpi2c1176_bus_recover(regs)`,
     `lpi2c1176_master_write(regs, addr, data, len, sendStop)` → 0/2/3/4/5,
     `lpi2c1176_master_read(regs, addr, dst, quantity, sendStop)` → count.

Provenance header on each file: "this project's HW-verified RT1176 LPSPI1/
LPI2C bring-up (SPIIMXRT1176.cpp / WireIMXRT1176.cpp, MIT), re-expressed as
the single shared C core (Phase 3.3); consumed by the CM7 class and the CM4
gate images."

### 3.2 CM7 side

- `SPI_Hardware_t` recomposes: `{ lpspi1176_hw_t hw; dma_rxisr; miso_pin[];
  mosi_pin[]; sck_pin[]; cs_pin[]; }` — references become the C struct's
  pointers (`&CCM_LPCG104_DIRECT` style initializers). `begin/end/
  setClockDividerHz/transfer/transfer16` delegate; the DMA path is untouched
  (uses `port()`, `tcr_base`, and the now-header macros).
- `I2C_Hardware_t` recomposes the same way (irq/irq_function/irq_priority
  stay as C++-only siblings). `begin()/setClock/endTransmission/requestFrom/
  end` delegate; `begin(address)` (slave) calls `lpi2c1176_clocks_pins` then
  keeps its SCR/NVIC body; `wait_flag`/`bus_recover` members are deleted.
- `port()` casts stay; delegation casts `port_addr` to the shared regs type
  (safe by the offset cross-asserts).

### 3.3 CM4 side

- `teensy_add_cm4_image` gains an **optional `INCLUDE_DIRS`** multi-value
  arg → per-image `-I` flags on the **compile** command only. Images that
  don't pass it get byte-identical command lines (verified by the
  `cm4_dual_test` `.cm4.bin` cmp).
- `cm4_spi_test`: SOURCES += `$ENV{HOME}/Development/SPI/lpspi1176.c`,
  INCLUDE_DIRS `$ENV{HOME}/Development/SPI`. `main_cm4.c` keeps the MU
  scaffolding, token order, and a `static const lpspi1176_hw_t` table (the
  same literal addresses it has today, now feeding the desc struct) and
  calls the shared functions; the local sequence bodies are deleted.
- `cm4_wire_test`: same shape with `lpi2c1176.c`, the WM8962 protocol
  bytes/addresses and the three-transaction flow unchanged.

Address literals remain only as the CM4 gates' per-instance desc tables
(bare-metal images have no imxrt1176.h); the *sequences* — the part that
drifted risk — exist once. The CM7 instance tables keep sourcing addresses
from `imxrt1176.h` macros, so the literal-vs-macro equivalence stays pinned
by the byte-identical gate outputs (and was literal-for-literal reviewed in
3.1/3.2).

---

## 4. Gates (nothing new; the guardrail is byte-identity)

No gate scripts change. Before touching code, capture baselines:
`cm4_spi_test`/`cm4_wire_test` assert against their checked-in
`transcript_qemu.txt`; the six library tests' `.uart` outputs are snapshotted
to the scratchpad pre-refactor. After: all gates green, transcripts
byte-identical, cm4 gates stable 3×. Audit-GATES builds (`sd_wav_play_test`
etc.) rebuild clean so their depfiles pick up the new lib files.

## 5. License firewall

All moved code is this project's own MIT (SPIIMXRT1176.cpp /
WireIMXRT1176.cpp and their reviewed distillations). New files carry the MIT
header + provenance note. No SDK/Zephyr/qemu2 code is involved; the GPL
one-way firewall is untouched (no qemu2 change at all). Audit: REPOS already
covers both library repos; GATES already covers both cm4 gates; run + PASS.

## 6. Risk-trigger walk & probe decision

- *undocumented/reserved bits*: none new. *boot/reset sequencing*: none.
  *memory aliasing*: none. *clock/power gating*: *sequences* unchanged;
  no new CCM accesses. *two sources disagree*: checked — no drift (§2).
  *new/changed qemu2 model*: none.
- *reset/default values you now depend on*: **the one honest brush.** Via D1/
  D3 the CM4 images newly (a) rewrite `CR/MCR=0` while already 0 (write of
  the current value — the CM7 HW-verified stream does exactly this on the
  same block instances) and (b) RMW `MCFGR1` relying on its post-reset 0
  (every CM7 `Wire2.begin()` HW run has depended on exactly this default on
  this exact LPI2C5 instance since the i2s bring-up). No *new silicon fact*
  is being relied on — but the CM4 `.cm4.bin`s change, so the 3.1/3.2 HW
  transcripts no longer describe the shipped binaries.

**Decision:** no trigger mandates a probe in the strict sense; per the
silicon-wins ethos the phase still gets a **wiring-free `cm4_wire_test`
EVKB re-probe** (flash + `clean_boot.scp`, expect byte-identical HW
transcript incl. `rdv=00006243`) as the 3.3 silicon anchor — it exercises
the shared `lpi2c1176.c` begin+write+read paths end-to-end on silicon.
`cm4_spi_test` re-probe needs the SDO(AD_30)→SDI(AD_31) jumper: run it if
the jumper is still fitted, otherwise queue it in the roadmap (QEMU + the
wire anchor + unchanged-sequence review cover it meanwhile).

## 7. Commit plan

Four repos touched, committed in dependency order with cross-referencing
messages: `teensy-cmake-macros` (INCLUDE_DIRS), `SPI` (lpspi1176 + class
delegation), `Wire` (lpi2c1176 + class delegation), `evkb` (gate mains +
CMakeLists + spec + roadmap). Roadmap session-log entry flips 3.3.
