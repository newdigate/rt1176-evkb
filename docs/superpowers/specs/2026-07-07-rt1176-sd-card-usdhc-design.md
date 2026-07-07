# RT1176 SD card (USDHC/SDIO) — Design

**Status:** approved (design), ready for implementation plan
**Date:** 2026-07-07

## Goal

Bring up Arduino `SD` + `SdFat` on the RT1176 core over the **USDHC1 SDIO controller**, so a real µSD in the EVKB's on-board slot (**J15**) can be mounted, written, and read (files + directories), and survive a power cycle. This is also the capstone feed for the audio stack — once files read back, a follow-on `AudioPlaySdWav` node plays a WAV out J101.

Two phases in **one** spec→plan→implement cycle:

- **Phase A — SDIO block driver + card init.** Port the checked-out `SdFat/src/SdCard/SdioTeensy.cpp` (the Teensy-4 USDHC backend) by adding an **in-file `__IMXRT1176__` branch** — retargeting only base address, clock, and pin-mux, exactly the SPI/Wire hybrid-port pattern. Both block-transfer paths ship and are verified: **PIO/FIFO** (`SdioConfig(FIFO_SDIO)`, the Arduino default) and **SDMA** (`SdioConfig(DMA_SDIO)`, `DS_ADDR`).
- **Phase B — FAT filesystem layer.** SdFat's silicon-agnostic `FatLib`/`ExFatLib` + the `newdigate/SD` (PaulS_SD) Arduino `SD.h` wrapper, on top of a **new core `FS.h`** (`File`/`FileImpl`/`FS` — the core has none yet).

`AudioPlaySdWav` (WAV → J101) is **spec C, deferred**.

Boundary (matches SPI/Wire): the SDIO driver + FAT live in the **libraries** (`newdigate/SdFat`, `newdigate/SD`); the **core** provides USDHC register defs, `IRQ_USDHC1`, and `FS.h`. Difference from the Wire/SPI *moves*: `~/Development/SdFat` and `~/Development/PaulS_SD` are **already the checked-out `newdigate` forks** (HEAD `4e82958`, active 1060-EVKB work), so this is "extend in place," not a fresh library move.

## Why this shape (exploration findings)

Four-agent recon across qemu2, teensy4/SdFat source, the EVKB RM + board netlist, and the core:

- **qemu2 already models USDHC well enough to gate against.** Both controllers are the generic `TYPE_IMX_USDHC` in `hw/sd/sdhci.c` + the stock SD-card model `hw/sd/sd.c`. `hw/arm/fsl-imxrt1170.c` wires `usdhc_base[]={0x40418000,0x4041C000}`, IRQ `{133,134}`, `capareg=0x057834b4`. `hw/arm/mimxrt1170-evk.c:90-106` attaches a `TYPE_SD_CARD` to each controller's `sd-bus`: **`-drive if=sd,index=0` → USDHC1**, `index=1` → USDHC2; no drive ⇒ card reports not-inserted. Full init (CMD0/8/ACMD41/2/3/7 via the memory-card model), single/multi-block read+write, and **all three data engines — PIO, SDMA, ADMA2-32** — are present. The i.MX register layout is handled (MIX_CTRL@0x48, VEND_SPEC@0xC0, PROT_CTRL↔HostCtrl translation, `trnmod` caching); recent commits fixed CC-before-TC ordering and MIX_CTRL read-back specifically for the SDK/SdFat driver. **Crucially, no `min_access_size` bug** — `usdhc_mmio_ops` sets `valid.min_access_size=1` and omits `.impl`, so byte/halfword accesses reach the device (unlike the LPSPI/FlexSPI traps). Gaps are all out-of-scope: SDIO I/O functions (CMD5/52/53), SDR104 tuning (no-op), the i.MX-route 1.8V UHS switch, and open-ended-multiblock-**via-DMA** (only the PIO open-ended path is wired). If a gate stalls, the tell is a `LOG_UNIMP` on a register — run `qrun` with `-d unimp,guest_errors`.
- **The µSD slot is USDHC1** (netlist socket J15). No NXP SDK `pin_mux.c`/`clock_config.c` exists in the tree, so the authorities were the RT1176 RM (`rm_full.txt`) and the EVKB board netlist (`pstxnet.dat`) — the netlist is real connectivity, as good as a pin_mux. USDHC1 base **`0x40418000`**, IRQ **133**, clock **CLOCK_ROOT58** (`CCM_CLOCK_ROOT58_CONTROL=0x40CC1D00`) off **SYS_PLL2_PFD2 ÷2 ≈198 MHz** (MUX field=4, DIV field=1), gate **LPCG117** (`CCM_LPCG117_DIRECT=0x40CC6EA0`).
- **Pins are `GPIO_SD_B1_00..05`, ALT0** (IOMUXC base `0x400E8000`). Mux reset value is **5 (GPIO)**, so ALT0 must be written explicitly (`MUX_MODE=0`). RT1176 pad-ctl bitfields are **ODE(bit4)/PULL[3:2]/PDRV(bit1)** — totally different from the 1062's PKE/DSE/SPEED/PUE/PUS, so the Teensy pad values do **not** carry over.

  | Signal | Pad | SW_MUX_CTL off | SW_PAD_CTL off | pad value |
  |---|---|---|---|---|
  | USDHC1_CMD | GPIO_SD_B1_00 | 0x19C | 0x3E0 | 0x04 (pull-up, high-drive) |
  | USDHC1_CLK | GPIO_SD_B1_01 | 0x1A0 | 0x3E4 | 0x08 (pull-down, high-drive) |
  | USDHC1_DATA0 | GPIO_SD_B1_02 | 0x1A4 | 0x3E8 | 0x04 |
  | USDHC1_DATA1 | GPIO_SD_B1_03 | 0x1A8 | 0x3EC | 0x04 |
  | USDHC1_DATA2 | GPIO_SD_B1_04 | 0x1AC | 0x3F0 | 0x04 |
  | USDHC1_DATA3 | GPIO_SD_B1_05 | 0x1B0 | 0x3F4 | 0x04 |

- **The driver is a clean retarget.** `SdioTeensy.cpp` (1133 lines) is guarded `__IMXRT1062__` with **no `__IMXRT1176__` branch**. It accesses USDHC via `SDHC_*` compat macros aliased to the core's `USDHC1_*` struct accessors (`SdioTeensy.h:231-252`), **hardwired to USDHC1** — which is what the EVKB uses, so no port-select needed. Init is **polled/blocking** (`SdioCard::begin` → CMD0→CMD8→ACMD41→CMD2→CMD3→CMD9/10→CMD7→ACMD6 4-bit→CMD6 high-speed→`setSdclk`). Only three things are platform-specific and 1062-only today: `initClock()`/`baseClock()` (CCM CCGR6/CSCDR1/CSCMR1), and `gpioMux()`/`enableGPIO()` (`GPIO_SD_B0_xx` + 1062 pad bits). The USDHC IP block is identical to the 1062 — the same reason SPI used an in-file `#elif` rather than a Wire-style dedicated file. Block transfer is **SDMA (`DS_ADDR`) or PIO (`DATA_BUFF_ACC_PORT`)**, selectable via `SdioConfig::useDma()`; it is **not** ADMA2. The Arduino wrapper ships PIO (`PaulS_SD/src/SD.cpp:90` → `SdioConfig(FIFO_SDIO)`).
- **Core has no USDHC defs and no `FS.h`.** `imxrt1176.h` (auto-generated) and `tools/gen_imxrt1176_h.py` contain zero USDHC content — defs go in **both**. The ready-made port reference is `teensy4/imxrt.h:9468-9533` (`IMXRT_USDHC_t` struct + `USDHC1_*` accessors) — reuse verbatim, only swapping the base from `0x402C0000`→`0x40418000`. `core_pins.h`'s `IRQ_NUMBER_t` enum has no `IRQ_USDHC1`. And `PaulS_SD/src/SD.h:38` does `#include <FS.h>` with `SDClass:public FS` / `SDFile:FileImpl`, but the core ships no `FS.h` (teensy4 has a 10 KB one; it's shared FS infra, kept in the core in Teensy too).
- **`~/Development/SD` is a red herring** — hundreds of `.mid` files (FAT-image *content*, useful for building `card.img`), **not** the SD library. The Arduino wrapper is `~/Development/PaulS_SD` (`newdigate/SD`).
- **Capstone API is tiny.** `Audio/play_sd_wav.cpp` uses only `SD.open()`, `File::operator bool`, `available()`, `read(buf,512)`, `close()` — no `seek`/`position` in the streaming loop. So Phase B's minimum-viable API easily covers spec C later.

## Scope

**In scope (Phase A + B):**
- **Core:** USDHC register defs (`IMXRT_USDHC_t` overlay + `USDHC1_*` accessors, base `0x40418000`) in `tools/gen_imxrt1176_h.py` **and** `imxrt1176.h`; `CCM_CLOCK_ROOT58_CONTROL`/`CCM_LPCG117_DIRECT` + the six `GPIO_SD_B1_*` IOMUXC mux/pad symbols if the generator's loops don't already emit them; `IRQ_USDHC1=133` (+`134`) in `core_pins.h`; **`FS.h`** (+ `FS.cpp` if teensy4 ships one) ported into the core.
- **`newdigate/SdFat`:** in-file `__IMXRT1176__` branch in `SdioTeensy.{h,cpp}` (clock=CLOCK_ROOT58/LPCG117, pins=`GPIO_SD_B1_00..05`, base via the new core macros); `HAS_SDIO_CLASS 1` + `BUILTIN_SDCARD` for `__IMXRT1176__` in `SdFatConfig.h`. Both PIO + SDMA paths.
- **`newdigate/SD` (PaulS_SD):** `__IMXRT1176__` in the `BUILTIN_SDCARD` guard (`SD.h:40`); base-class selection already resolves via `__arm__`.
- **Gates:** two — `sd_block_test` (raw SDIO, PIO **and** SDMA) and `sd_fs_test` (filesystem) — lib-owned under `SdFat/tests/`, following the `eeprom_test`/SPI-tests template. HW verification of both.

**Explicitly deferred (YAGNI):**
- **`AudioPlaySdWav`** (spec C).
- **ADMA2** — the driver uses SDMA/PIO, not ADMA2; leave it untouched.
- **50 MHz high-speed** as a *requirement* — the CMD6 high-speed switch runs if it succeeds, but the target/verified clock is the 25 MHz default (QEMU tuning is a no-op; §Risks).
- **1.8 V / UHS (SDR50/104) signaling** — the board's `SD1_VSELECT` analog switch stays at 3.3 V; not exercised.
- **SDMA *through the full filesystem stack*** — SDMA is verified at the raw-block level (Gate 1) with a DMA-placed buffer, which proves the SDMA silicon path. Routing SDMA under SdFat would require the SdFat cache buffer to live in DMA-reachable memory (a deeper change); the filesystem gate uses PIO (the `SD.h` default). *(Flagged for the spec-review gate — say so if you want SDMA-under-FAT in this cycle.)*
- **Card-detect as an interrupt** — presence is read as a plain GPIO if needed (QEMU ties presence to image-attached), not wired to an IRQ.

## Architecture — components

### 1. Core: USDHC register defs (`imxrt1176.h` + `tools/gen_imxrt1176_h.py`)
`imxrt1176.h` is auto-generated → add to **both**. Use generator **idiom B** (the raw struct-overlay heredoc, as `IMXRT_LPSPI_t` at `gen_imxrt1176_h.py:296-329`): emit the `IMXRT_USDHC_t` struct mirroring `teensy4/imxrt.h:9468-9501` (DS_ADDR 0x00, BLK_ATT 0x04, CMD_ARG 0x08, CMD_XFR_TYP 0x0C, CMD_RSP0-3 0x10-0x1C, DATA_BUFF_ACC_PORT 0x20, PRES_STATE 0x24, PROT_CTRL 0x28, SYS_CTRL 0x2C, INT_STATUS/EN/SIGNAL_EN 0x30-0x38, AUTOCMD12 0x3C, HOST_CTRL_CAP 0x40, WTMK_LVL 0x44, MIX_CTRL 0x48, ADMA_SYS_ADDR 0x58, VEND_SPEC 0xC0, TUNING_CTRL 0xCC, …), then `#define IMXRT_USDHC1_ADDRESS 0x40418000` (+ `USDHC2 0x4041C000`) and the flat `USDHC1_*` accessor macros (`USDHC1_BLK_ATT`, `USDHC1_CMD_XFR_TYP`, `USDHC1_MIX_CTRL`, …) that `SdioTeensy.h` consumes. Add USDHC1 to the clock-root/LPCG emitters (`CCM_CLOCK_ROOT58_CONTROL`, `CCM_LPCG117_DIRECT`) and the six `GPIO_SD_B1_00..05` pads to the IOMUXC mux/pad dicts, **only if** the generator's existing loops don't already cover those indices/pads (verify in Task 1). The `SDHC_*` *bitfield* macros stay library-owned (they already live in `SdioTeensy.h:11-31+`).

### 2. Core: `IRQ_USDHC1` (`core_pins.h`)
Add `IRQ_USDHC1 = 133` (and `IRQ_USDHC2 = 134`) to the `IRQ_NUMBER_t` enum, matching the RM vector numbers (the enum already uses RM numbers verbatim, e.g. `IRQ_SAI1=76`, `IRQ_USB_OTG1=136`). Needed by the driver's ISR-attach for the SDMA-completion path even though init is polled.

### 3. Core: `FS.h` (+ `FS.cpp` if present) — new filesystem infra
Port `teensy4/FS.h` into `cores/imxrt1176`: `class File : public Stream` backed by `FileImpl` (virtuals `read/write/available/peek/flush/seek/position/size/close/name/isDirectory/openNextFile/rewindDirectory`), plus the `FS` base class. Silicon-agnostic; the core already provides `Stream`. This is the dependency `PaulS_SD/src/SD.h` needs (`SDClass:public FS`, `SDFile:public FileImpl`). Idiomatic placement is the core (as in teensy4), not the SD library — it's shared by SD/LittleFS/MTP.

### 4. `newdigate/SdFat`: `SdioTeensy` `__IMXRT1176__` branch (Phase A)
Widen the top-of-file platform guards in **both** `SdioTeensy.h` and `SdioTeensy.cpp` to include `__IMXRT1176__`. The `SDHC_*→USDHC1_*` register aliases (`SdioTeensy.h:231-252`) are reused **verbatim** (they now resolve to the Task-1 core macros). Add a `__IMXRT1176__` branch to the three platform-specific functions:
- **`initClock()` (`SdioTeensy.cpp:354`):** `CCM_CLOCK_ROOT58_CONTROL = MUX(4=SYS_PLL2_PFD2)|DIV(1)` (≈198 MHz), then `CCM_LPCG117_DIRECT = 1`. Replaces the 1062 `CCGR6`/`CSCDR1`/`CSCMR1` block. (Verify SYS_PLL2_PFD2 is ≈396 MHz under the core's `BootClockRUN` in `startup.c`; if not, adjust the divisor.)
- **`baseClock()` (`:369`):** return `198000000` so `setSdclk()`'s `SDCLKFS`/`DVS` math yields the 400 kHz identification clock and the 25 MHz run clock. `setSdclk()`/`SYS_CTRL` divider logic is register-identical and reused as-is.
- **`gpioMux()` (`:320`) / `enableGPIO()` (`:330`):** mux `GPIO_SD_B1_00..05` to ALT0 (write `0`, since reset is `5`); on enable, write pad-ctl CMD/DATA=`0x04`, CLK=`0x08` (RT1176 ODE/PULL/PDRV encoding); on disable, mux to GPIO for the high-speed clock-switch bracket. Replaces the `GPIO_SD_B0_xx` + PKE/DSE/SPEED path.
- **Card power-enable:** the driver has an `#if defined(ARDUINO_MIMXRT1060_EVKB)` power-switch hook (`SdioTeensy.cpp:651-663`, drives a load-switch GPIO low=on). The **1170-EVKB** uses a switched `VSD_3V3` rail (netlist P-FET Q11, gate = a control net) — **identify that enable GPIO** and add a parallel `__IMXRT1176__`/board hook. No-op under QEMU (card present ⇔ image attached), so this is HW-phase only.

### 5. `newdigate/SdFat`: `SdFatConfig.h`
Add a `__IMXRT1176__` block setting `HAS_SDIO_CLASS 1` and `BUILTIN_SDCARD` (mirroring the `__IMXRT1062__` block at ~416-419), so `SdCardFactory` instantiates `SdioCard` and `SdFs`/`SdFat` select the SDIO backend.

### 6. `newdigate/SD` (PaulS_SD): Phase-B wrapper
Add `__IMXRT1176__` to the `BUILTIN_SDCARD` guard (`SD.h:40`). The `#if defined(__arm__)` base-class selection (`SDFAT_BASE=SdFs`, `SDFAT_FILE=FsFile`, `SD.h:44-48`) already resolves for 1176 — no change. `SD.begin(BUILTIN_SDCARD)` → `SdFs.begin(SdioConfig(FIFO_SDIO))` → the Phase-A `SdioCard`.

### 7. Gates (lib-owned, `SdFat/tests/`)
- **`sd_block_test` (Phase A, raw SDIO):** `SdioCard::begin(cfg)`, then write a known pattern to a spread of sectors and read back byte-exact — run twice, `SdioConfig(FIFO_SDIO)` and `SdioConfig(DMA_SDIO)` (SDMA buffer in **DMAMEM/OCRAM**). Markers `SD_INIT=PASS`, `SD_BLOCK_PIO=PASS`, `SD_BLOCK_DMA=PASS`. Isolates SDIO silicon from FAT.
- **`sd_fs_test` (Phase B, filesystem):** `SD.begin(BUILTIN_SDCARD)` on a FAT `card.img` → create/open-for-write, write bytes, close, reopen, read back byte-exact, list the root dir, check `exists()`/`size()`. Markers `SD_FS_WRITE=PASS`, `SD_FS_READ=PASS`, `SD_FS_DIR=PASS`. Proves host↔firmware FAT interop.

Each `tests/<name>/` is self-contained (own `CMakeLists.txt` + `run_qemu_<name>.sh` + `toolchain/`), `import_arduino_library`-ing `cores` (imxrt1176), `SdFat` (self), `SD` (PaulS_SD), and `SPI` (SdFat pulls it for the unused SdSpiCard path). Runner sources `gate-lib.sh`, `gate_init`, invokes `qrun -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel <elf> … -drive if=sd,format=raw,file=<card.img>`, greps the markers over VCOM (`-serial file:`). `card.img` is built at gate setup (`mkfs.fat` + `mtools mcopy` a known file; the `~/Development/SD` corpus is staging material) or committed.

## Data flow / behavior

**Phase A (raw):** `SdioCard::begin` → `initSDHC` (`SYS_CTRL.RSTA` reset, `initClock` = ROOT58/LPCG117, `enableGPIO` = GPIO_SD_B1 pads, `SYS_CTRL.INITA` 80-clock pulse, `setSdclk(400kHz)`) → polled CMD0/8/ACMD41/2/3/7 + ACMD6 (4-bit) → `setSdclk(25MHz)`. `writeSector(lba,buf)`/`readSector` → `rdWrSectors`: PIO polls `PRES_STATE.BREN/BWEN` moving 512 B through `DATA_BUFF_ACC_PORT`; SDMA writes `DS_ADDR=buf`, `MIX_CTRL.DMAEN`, issues CMD17/24, waits `INT_STATUS` transfer-complete. Byte-exact readback proves the path.

**Phase B (filesystem):** `SD.open(path)` (PaulS_SD `SDClass:FS`) → `SdFs`/`FatLib` → `SdioCard::readSector`/`writeSector` (Phase A) → USDHC1. `File::read/write/close` route through `FS.h`'s `File`/`FileImpl` (core) into `FsFile`. On QEMU the FAT lives in the attached `card.img`; on HW, in the real card's FAT — a host-written file must read back byte-identical in firmware and vice-versa.

Both are identical on the QEMU `TYPE_IMX_USDHC` model (PIO the most-hardened path; SDMA present) and on silicon — except the SDMA DMA-reachability caveat (§Risks), where QEMU passes but silicon is the arbiter.

## Testing

**QEMU gates** (qemu2 models USDHC + SD-card attach — no model change expected for basic I/O):
- `sd_block_test`: init + byte-exact sector write/read under **both** FIFO and DMA configs → `SD_INIT`/`SD_BLOCK_PIO`/`SD_BLOCK_DMA` = PASS.
- `sd_fs_test`: file write→reopen→read-back byte-exact + dir list on `card.img` → `SD_FS_WRITE`/`SD_FS_READ`/`SD_FS_DIR` = PASS. Also confirms a **host-created** file is readable in firmware (real FAT interop, not just self-consistency).

Run `qrun` with `-d unimp,guest_errors` during bring-up to instantly surface any register the driver touches that the model doesn't implement.

**Hardware (final arbiter):** LinkServer flash + VCOM @115200, a real µSD in J15.
- Gate 1 on HW: raw block **PIO and SDMA** (SDMA is the one QEMU can't fully vouch for — DMA reachability).
- Gate 2 on HW: file write / read-back / dir list.
- **Persistence:** write a signature file, power-cycle the board, re-read it — the real SD proof (a within-run buffer would pass QEMU but fail this).
The controller drives flash + VCOM; the user physically inserts the card / confirms.

**Contingency (QEMU model fixes, if hit):** (a) a `LOG_UNIMP` register → add it to the i.MX intercept in `hw/sd/sdhci.c` (same class as prior model touch-ups); (b) if the driver's high-speed CMD6 path stalls on the no-op tuning, keep 25 MHz for the gate and defer high-speed to HW; (c) SDMA under DMA if it drains differently than silicon — mirror-silicon adjust as done for eDMA/SPI-DMA. qemu2 is otherwise **unchanged** (USDHC + SD attach already exist).

## Risks

- **SDMA buffer not DMA-reachable (the big one).** `DS_ADDR` takes the caller's buffer; on this core `.bss` lands in DTCM, which the USDHC AXI master generally **cannot** reach. QEMU treats TCM as ordinary RAM, so an SDMA-to-DTCM transfer **passes in QEMU but faults/zeroes on silicon** — the SerialUSB/eDMA-class divergence. Mitigation: Gate-1 SDMA buffers go in **DMAMEM/OCRAM**; HW is the arbiter; the filesystem gate uses PIO. If SDMA-under-FAT is later wanted, the SdFat cache buffer must be DMA-placed.
- **1170-EVKB card power-enable GPIO unknown.** The µSD has a switched `VSD_3V3` rail (P-FET Q11); its enable GPIO must be traced from the netlist and driven at `begin()`, else the card is unpowered on HW (QEMU N/A). Model on the existing 1060-EVKB hook.
- **Clock assumption.** ROOT58 ← SYS_PLL2_PFD2÷2 assumes PFD2 ≈396 MHz under the core's boot clock; if `startup.c` programs PFD2 differently, `baseClock()` and the `setSdclk` divisors are wrong (clock too fast/slow → init fails). Verify PFD2 when implementing the clock branch; fall back to a mux/divider that lands ≤198 MHz.
- **High-speed (CMD6) switch.** QEMU's tuning is a no-op; if the driver *requires* a successful high-speed handshake it could diverge. Keep the verified clock at 25 MHz default; treat 50 MHz as HW-only follow-on.
- **Register-def correctness.** A wrong USDHC offset silently no-ops (QEMU) or faults (HW). Cross-check the `IMXRT_USDHC_t` overlay against `teensy4/imxrt.h` and the RM; only the base changes (`0x40418000`).
- **`min_access_size`** — confirmed **not** an issue for USDHC (byte access allowed), unlike LPSPI/FlexSPI.
- **SdFat build surface.** SdFat pulls SPI for `SdSpiCard`; ensure the gate links `newdigate/SPI` (or the SDIO-only path is cleanly compiled) so Phase B builds.

## References

- **Port source (checked out):** `SdFat/src/SdCard/SdioTeensy.{cpp,h}` (the USDHC backend — `initClock` :354, `baseClock` :369, `setSdclk` :530, `gpioMux` :320, `enableGPIO` :330, `begin` :641, 1060-EVKB power hook :651, `SDHC_*→USDHC1_*` aliases `.h:231-252`), `SdFat/src/SdCard/SdioCard.h` (`SdioConfig` FIFO/DMA), `SdFat/src/SdFatConfig.h` (`HAS_SDIO_CLASS`/`BUILTIN_SDCARD`), `PaulS_SD/src/SD.{h,cpp}` (`SDClass:FS`, `:40` `BUILTIN_SDCARD`, `:90` `FIFO_SDIO`), `teensy4/imxrt.h:9468-9533` (`IMXRT_USDHC_t` + `USDHC1_*` to retarget), `teensy4/FS.h` (the `File`/`FileImpl`/`FS` to port).
- **Core:** `cores/imxrt1176/imxrt1176.h` + `tools/gen_imxrt1176_h.py` (add USDHC — LPSPI overlay `:296-329` is the template), `core_pins.h` (`IRQ_NUMBER_t`).
- **EVKB hardware:** `rm_full.txt` (USDHC1 base `0x40418000`, ROOT58/LPCG117, IRQ 133, pad offsets, pad-ctl fields), `MIMXRT1170-EVKB-DESIGNFILES/.../pstxnet.dat` (J15 → GPIO_SD_B1_00..05, card-detect GPIO_EMC_B1_24, Q11 power, SD1_VSELECT).
- **qemu2:** `hw/sd/sdhci.c` (`TYPE_IMX_USDHC`), `hw/arm/fsl-imxrt1170.c` (usdhc bases/IRQs/capareg), `hw/arm/mimxrt1170-evk.c:90-106` (`-drive if=sd,index=0` → USDHC1).
- **Capstone (spec C):** `Audio/play_sd_wav.cpp` (`SD.open`/`available`/`read`/`close`).
- **Precedents:** hybrid-port method + boundary — [[rt1176-spi-library-move]], [[rt1176-wire-library-move]]; core register/generator + flash-adjacent bring-up — [[rt1176-eeprom]]; DMA-memory (DMAMEM) traps QEMU can't catch — [[rt1176-edma-dmachannel]], [[rt1176-serialusb]]; the audio stack SD feeds — [[rt1176-i2s-sai]], [[rt1176-audiooutput-i2s]]; gate/runner infra — [[rt1170-qemu]], [[rt1170-gate-lib]]; HW flash + serial — [[rt1170-evkb-flashing]], [[macos-serial-capture]]; repo boundaries — [[rt1170-evkb-git-repo]].
