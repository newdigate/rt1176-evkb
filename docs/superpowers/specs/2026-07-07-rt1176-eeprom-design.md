# RT1176 EEPROM (flash-emulated) — Design

**Status:** approved (design), ready for implementation plan
**Date:** 2026-07-07

## Goal

Bring up Arduino/Teensyduino `EEPROM` on the RT1176 core by implementing the AVR EEPROM C API (`eeprom_read/write_byte/word/dword/block`, `eeprom_initialize`) in `cores/imxrt1176`, backed by **flash emulation** ported from the checked-out `cores/teensy4/eeprom.c`. The `newdigate/EEPROM` library is header-only and platform-agnostic — it needs **no changes**; it works the moment the core provides the `eeprom_*` functions + an `<avr/eeprom.h>` shim. EEPROM capacity = **4284 bytes** (`E2END=4284`) with **63 flash sectors** — Teensy-4.1-class capacity. The emulation lives at the **top of RT1176's 16 MB NOR**: `FLASH_BASEADDR = 0x30FC0000` (retargeted from teensy4's 16 MB-flash define `0x60FC0000`; RT1176's FlexSPI1 XIP base is `0x30…` vs RT1062's `0x60…`, not the 8 MB Teensy-4.1 `0x607C0000`).

## Why this shape (exploration findings)

- **`newdigate/EEPROM` is header-only** (`git@…/PaulStoffregen/EEPROM`, fork): `EEPROM.cpp` is a 28-byte stub; `EEPROM.h` is the Christopher Andrews `EERef`/`EEPtr`/`EEPROMClass` templates, which call the **AVR C API** via `#include <avr/eeprom.h>` (`eeprom_read_byte`, `eeprom_write_byte`, `eeprom_read_block`, `eeprom_write_block`, `eeprom_read/write_word/dword`). The library is platform-agnostic; all platform work lives in the **core** (exactly Teensy's boundary — `eeprom.c` is a core file, `EEPROM.h` is a library). So this is a **core bring-up, not a library move** (the opposite boundary from the Wire/SPI moves).
- The core **already declares** the eeprom_* API in `cores/imxrt1176/avr_functions.h` (`eeprom_initialize`, `eeprom_read_byte`, `eeprom_read_block`, `eeprom_write_byte`, `eeprom_write_block`) but **nothing implements them** (no `eeprom.c`), and the `<avr/eeprom.h>` that `EEPROM.h` includes **does not exist** in the core (`cores/imxrt1176/avr/` has only `pgmspace.h`). So the bring-up = add `eeprom.c` (the implementation) + `avr/eeprom.h` (the shim with `E2END` + the decls).
- **The port source is checked out**: `cores/teensy4/eeprom.c` (357 lines) — proven i.MX RT EEPROM emulation. It maps EEPROM addresses across `FLASH_SECTORS` 4 KB flash sectors with a wear-leveling packing (`sector = (addr>>2) % FLASH_SECTORS`, offset packing, per-sector compaction on fill), and implements the **RAM-resident FlexSPI program/erase primitives** (`eepromemu_flash_write` = write-enable `0x06` + quad-write `0x32`; `eepromemu_flash_erase_sector` = write-enable + sector-erase `0x20`; `flash_wait` = poll status `0x05`; all with `__disable_irq()` + `arm_dcache_delete` + a FlexSPI `SWRESET` AHB-buffer purge). Same hybrid-port pattern as Wire/SPI: port verbatim-in-behavior, retargeting the base addresses.
- RT1176 flash: **FlexSPI1 NOR XIP window at `0x30000000`, 16384 KB** (`imxrt1176.ld` `FLASH (rx) ORIGIN=0x30000000 LENGTH=16384K`), no EEPROM region reserved. FlexSPI1 peripheral base = **`0x400CC000`** (`imxrt1176.h` `FLEXSPI_BASE`), but the header has **only the base** — none of the IP-command registers (`LUT*`, `IPCR*`, `IPCMD`, `INTR`, `TFDR0/RFDR0`, `MCR0`, `LUTKEY/LUTCR`) the primitives need.
- **qemu2 models FlexSPI program/erase** (`hw/ssi/imxrt_flexspi.c`): it walks the IP-command LUT sequence and performs NOR page-program (bitwise-AND) + sector/block/chip erase (to 0xFF) against a **writable** flash window (`dma_memory_write` to `flash_base+addr`), exactly matching silicon NOR + teensy4's expectations. The RT1170 machine (`hw/arm/fsl-imxrt1170.c`) sets the FlexSPI1 `flash-base`/`flash-size` props and comments that the window exists so "a real driver programs/erases the flash." Optional `-drive if=mtd,file=…` gives file-backed persistence; without it the window is RAM-backed (writable within a run). **So the EEPROM write/read/wear-leveling path is QEMU-gateable** — not a HW-only gamble.

## Scope

**In scope:** the full AVR EEPROM C API in the core (`eeprom_initialize` + `eeprom_read/write_byte/word/dword/block`), flash-emulated via a port of `cores/teensy4/eeprom.c` retargeted to RT1176 (FlexSPI1 `0x400CC000`, flash region `0x30FC0000`, 63 sectors), the RAM-resident (`.fastrun`/ITCM) FlexSPI program/erase primitives, the `avr/eeprom.h` shim (E2END=4284), the FlexSPI IP-command register defs (generator + header), the reserved linker region, and `arm_dcache_delete`. A QEMU gate + HW verification. `newdigate/EEPROM` unchanged (only build-linked into the gate).

**Explicitly deferred (YAGNI):** any non-flash backend (LPGPR/SNVS registers don't survive full power loss; RAM stubs aren't EEPROM). The 32K/64K-block erase helpers teensy4 exposes (`eepromemu_flash_erase_32K/64K_block`) beyond what `eeprom.c` itself calls — port them for parity only if trivial, else skip. A user-facing `EEPROM.begin(size)` dynamic sizing (Teensy's EEPROM is fixed-size).

## Architecture — components

### 1. `cores/imxrt1176/avr/eeprom.h` (new shim)
The header `EEPROM.h` includes. Defines `#define E2END 4284` (EEPROM top address; 4285 bytes usable) and declares/re-exports the `eeprom_*` functions (include or mirror `avr_functions.h`'s decls). If `EEPROM.h`'s `#include <avr/io.h>` doesn't already resolve in the core, add a minimal `avr/io.h` shim too (verify during Task 1).

### 2. FlexSPI IP-command register defs (`imxrt1176.h` + `tools/gen_imxrt1176_h.py`)
`imxrt1176.h` is auto-generated → add to **both**. The registers `eeprom.c` uses, at **FlexSPI1 base `0x400CC000`** (same IP + offsets as the RT1062 FlexSPI teensy4 targets — only the base differs): `MCR0` (+`_SWRESET`), `LUTKEY` (+`_VALUE`), `LUTCR` (+`_UNLOCK`), `IPCR0`, `IPCR1` (+`_ISEQID`/`_IDATSZ`), `IPCMD` (+`_TRG`), `INTR` (+`_IPCMDDONE`/`_IPTXWE`), `IPTXFCR` (+`_CLRIPTXF`), `IPRXFCR` (+`_CLRIPRXF`), `TFDR0`, `RFDR0`, `LUT60/61/62/63` (LUT slots 60–63 the primitives repurpose), + the LUT-instruction opcode/pad macros (`FLEXSPI_LUT_INSTRUCTION`, `CMD_SDR`/`ADDR_SDR`/`READ_SDR`/`WRITE_SDR` opcodes, `NUM_PADS_1/4`). Take the exact offsets + bit values from `cores/teensy4/imxrt.h`, retarget the base.

### 3. `cores/imxrt1176/eeprom.c` (new — the port)
Port `cores/teensy4/eeprom.c` verbatim-in-behavior:
- **Emulation (silicon-agnostic, copy as-is):** `eeprom_initialize` (scan sectors, build `sector_index[]`), `eeprom_read_byte`/`_word`/`_dword`/`_block`, `eeprom_write_byte`/`_word`/`_dword`/`_block` — the sector-mapping + wear-leveling logic. Set `FLASH_SECTORS 63`.
- **RAM-resident FlexSPI primitives (retarget the base):** `flash_wait`, `eepromemu_flash_write`, `eepromemu_flash_erase_sector` (+ the 32K/64K helpers if trivial). All the `FLEXSPI_*` accesses now resolve to `0x400CC000` (via the Task-2 defs). Must carry `__disable_irq()`/`__enable_irq()`, the `arm_dcache_delete` purge, and the `SWRESET` AHB purge exactly.
- **Retarget:** `#define FLASH_BASEADDR 0x30FC0000` (RT1176 FlexSPI1 XIP top − 256 KB), `#define FLASH_SECTORS 63`.
- **Placement:** the flash primitives (and anything they call) must be linked into **ITCM (`.fastrun`)** so they don't fetch from the FlexSPI flash mid-erase/program. Mark them `FASTRUN`/`.fastrun` (whatever idiom the core uses, matching the LPI2C slave ISR / the flash-unsafe-code convention).

### 4. Linker region (`imxrt1176.ld`)
Reserve the top **256 KB** of flash (`0x30FC0000 .. 0x31000000`; 63×4 KB sectors = 252 KB + 4 KB slack) so `.text`/`.flashmem`/the loaded image and the OCRAM heap never occupy it, and it is **not** part of `_flashimagelen`. Either a `MEMORY` sub-region + an assertion that the image end (`__text_csf_end`) stays below `0x30FC0000`, or a documented reserved-by-convention region with a link-time `ASSERT`. The EEPROM base is fixed in `eeprom.c` (`FLASH_BASEADDR`), so the linker's job is only to keep the image out.

### 5. `arm_dcache_delete` (core)
`eeprom.c` calls it to purge stale flash data from the M7 D-cache after erase/program. Provide it (port from teensy4's cache maintenance, or a **no-op** if the RT1176 core runs D-cache-off — the SerialUSB effort already treats dcache as a no-op; [[rt1176-serialusb]]). The FlexSPI `SWRESET` AHB-buffer purge in `flash_wait` is the load-bearing coherency step regardless.

## Data flow / behavior

`EEPROM.read(i)`/`write(i,v)`/`get`/`put` (library) → `eeprom_read/write_byte/block` (core `eeprom.c`) → for writes, the wear-leveling logic picks a flash location and calls `eepromemu_flash_write` (or `_erase_sector` on sector-full compaction) → the ITCM FlexSPI primitive issues IP commands to FlexSPI1 → the NOR sector is programmed/erased. Reads are direct memory loads from the XIP window (`FLASH_BASEADDR + …`). Behavior identical to Teensy 4.1 (same emulation), on both the QEMU FlexSPI model (program=AND, erase=0xFF) and silicon.

## Testing

**QEMU gate** `eeprom_test` (qemu2 models program/erase — within-run writable flash): (a) `eeprom_initialize`, write a spread of byte values across the address space + `EEPROM.put()` a small struct, read them all back byte-exact via `eeprom_read_byte`/`EEPROM.get()`; (b) **wear-leveling**: write the *same* address enough times to fill its sector and force a compaction + `eepromemu_flash_erase_sector`, then confirm the value + neighbors survive (exercises the erase path + sector rotation). Markers like `EEPROM_RW=PASS` / `EEPROM_WEAR=PASS` / `EEPROM_ALL=PASS`. Prove the emulation + FlexSPI program/erase plumbing regardless of real timing.

**Hardware (final arbiter — persistence is the whole point):** write known bytes (e.g. a signature + a counter), **reset the board**, read them back over VCOM — data surviving a power/reset cycle is the real EEPROM proof (a within-run RAM buffer would pass QEMU but fail this). The controller drives flash + VCOM + reset; the user confirms. A second flash-cycle read (without re-writing) double-confirms cross-power persistence.

**Contingency:** if the RT1170 machine's FlexSPI1 `flash-size` prop is < 16 MB (doesn't cover `0x30FC0000`), bump it in `hw/arm/fsl-imxrt1170.c` — a one-line qemu2 tweak (same class as prior model touch-ups). qemu2 otherwise UNCHANGED (the program/erase model already exists).

## Risks

- **Self-programming the boot flash (the big one).** The erase/program routines run while FlexSPI1 can't serve XIP fetches. Mitigations: they live in **ITCM (`.fastrun`)** with `__disable_irq()` around the op (teensy4 already structures it this way — port faithfully, do NOT let any called function stay in flash); the EEPROM region is 15+ MB from the boot image so a mis-addressed write can't hit `.text`; QEMU-gate the whole path first; worst case on HW is a re-flash with LinkServer (recoverable, not a permanent brick). **Verify every function on the flash-op call path is `.fastrun`** (a single flash-resident callee faults).
- **Cache/AHB coherency.** After erase/program, reads must see fresh data — keep the FlexSPI `SWRESET` AHB purge + `arm_dcache_delete`; if the core runs D-cache-on, confirm `arm_dcache_delete` actually invalidates (not a no-op) for the EEPROM range.
- **FlexSPI register-def correctness.** The LUT/IPCR/IPCMD offsets + bit fields must match FlexSPI1 exactly (base `0x400CC000`); cross-check against `cores/teensy4/imxrt.h` and the RT1176 RM. A wrong offset silently no-ops the flash op (QEMU) or faults (HW).
- **QEMU flash-size coverage** — see the contingency; check the prop value before assuming the gate reaches `0x30FC0000`.
- **The image must not grow into the EEPROM region** — the linker `ASSERT` guards this; a large future image would trip it (by design).

## References

- Port source: `cores/teensy4/eeprom.c` (the emulation + FlexSPI primitives) + `cores/teensy4/imxrt.h` (the `FLEXSPI_*` register defs to retarget) + `cores/teensy4/avr/eeprom.h` (the E2END + decl shim to mirror).
- Library (unchanged, build-linked): `/Users/nicholasnewdigate/Development/EEPROM` — `EEPROM.h` (EERef/EEPtr/EEPROMClass), `examples/` (eeprom_write/read/get/put/update/clear/iteration/crc), all iterate over `EEPROM.length()`.
- Core: `cores/imxrt1176/avr_functions.h` (the existing eeprom_* decls), `avr/pgmspace.h` (the existing avr shim precedent), `imxrt1176.h` + `tools/gen_imxrt1176_h.py` (FLEXSPI_BASE `0x400CC000`; the generator to extend), `imxrt1176.ld` (the flash region).
- qemu2: `hw/ssi/imxrt_flexspi.c` (the program/erase model), `hw/arm/fsl-imxrt1170.c` (FlexSPI1 flash-base/flash-size props), `hw/arm/mimxrt1170-evk.c` (optional `-drive if=mtd` persistent backing).
- Precedents: the hybrid-port method + FlexSPI/`.fastrun` idioms — [[rt1176-lpi2c-wire]] (fastrun-ITCM ISR), [[rt1176-serialusb]] (dcache no-op), [[rt1176-spi-library-move]] / [[rt1176-wire-library-move]] (teensy4-source hybrid ports), [[rt1170-evkb-flashing]] (LinkServer flash + VCOM for the HW test).
