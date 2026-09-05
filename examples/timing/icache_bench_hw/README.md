# icache_bench_hw — CM7 L1 I-cache microbench (silicon-only)

NEW-36. The `imxrt1176` core enables the Cortex-M7 L1 instruction cache in
`ResetHandler` since teensy-cores `2304743`; before that every flash-resident
(XIP) function ran at raw QSPI speed (NEW-33 finding (a)). This bench A/Bs the
cache in ONE boot over three workloads, each in ITCM and in FLASH, and prints
the boot ROM's `SCB_CCR` and the `CCSIDR` decode first.

Measured 2026-09-05 (three boots of the default build, one of the ITCM build;
`transcript_hw_evkb.txt`):

| workload | shape | ITCM | flash, I-cache off | flash, I-cache on |
|---|---|---|---|---|
| `calls` (cyc/call) | 64 tiny functions, unrolled direct chain | 8.3 | 192.3 | 8.3 (cold rep 36 µs vs 34) — **23.0×** |
| `crc` (cyc/byte) | table-driven CRC32, table+buffer in DTCM | 9.0 | 9.0 | 9.0 — **1.0×** |
| `sbc` (µs/frame) | M2Radio `Sbc::encode`, 44.1 kHz joint 16×8 bitpool 53 | 125 | 1106 | 187 (`realtime_x` 15.5) — **5.9×** |

Readings: `ccr_reset=0x00040200` — the boot ROM hands over with **IC=0**
(only BP and STKALIGN set); `isize_kb=32 ways=2 line_b=32`. Witnesses: every
`wit_match=1`, `sbc_crc=0x6c24f764` identical across QEMU, the flash build
and the ITCM build. Every prediction in the spec's table held.

## Read the numbers with these caveats
- **Best case.** The bench's whole flash footprint (~3 KB) fits the 32 KB
  cache, so no cell ever sees a capacity or conflict miss. acid_box's
  `M2_BT_OUT` build routes tens of KB of M2Radio to flash, where set
  conflicts are real — the application number is the acid_box LOOPSTAT A/B
  in `examples/display/acid_box/transcript_hw_evkb_bt.txt`, not this table.
- **`icache=off` is "AHB-buffered", not "unbuffered".** The FlexSPI AHB
  prefetch buffer still serves small hot loops: `crc`'s ~20-byte loop never
  missed, hence 1.0×. The uncached `calls` cost here (~0.19 µs/call, 1 KB of
  contiguous code) is far below the 5–30 µs/call NEW-33 measured in acid_box,
  whose flash-resident chain is tens of KB interleaved with LVGL.
- **`cold_us/rep` only says something for `calls`** (a ~34 µs rep, ~2 µs of
  fill). For `crc` and `sbc` the rep is 100–1000× longer than the fill.

## No QEMU gate, on purpose
qemu2 masks `CCR.IC` out of every write and NOPs `ICIALLU`, so every row reads
`icache=off` there and the cycle counts mean nothing. The `wit_match` /
`sbc_crc_match` lines are checked under QEMU as a test of the bench's own logic
(`../../../tools/rt1170-qemu.sh build/icache_bench_hw.elf`).

## Build / run
```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
cmake -B build-sbc-itcm -DICACHE_BENCH_SBC_ITCM=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build-sbc-itcm
```
`build/` routes `Sbc.cpp.obj` to FLASH (a linker script derived from the core's
`imxrt1176.ld`, acid_box's mechanism); `build-sbc-itcm/` leaves it in ITCM for
the reference cell. Flash with LinkServer (`flash … load` → `verify`), attach
`tools/rt1170-console.py`, press SW4. Full captures: `transcript_hw_evkb.txt`.

## Follow-on (spec §8 trigger)
`realtime_x_on` = 15.5 ≥ 5, so acid_box's `M2_BT_OUT` ITCM routing is worth
revisiting — filed as NEW-37, with this bench as its input.
