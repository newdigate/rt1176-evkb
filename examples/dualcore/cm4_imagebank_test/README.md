# cm4_imagebank_test — Cm4ImageBank over switchImage (★★HW-VERIFIED)

A small CM7-side registry (`Cm4ImageBank`, `cores/imxrt1176`) over the
HW-verified `Multicore` boot machinery: register several build-fixed CM4 images
in **uniform ITCM slots** and switch the running CM4 among them by handle —
a **fast no-copy VTOR flip** when the target is resident, a **stage + evict**
when it is not. No `Multicore` or qemu2 change.

## Layout (uniform slots)

`CM4_SLOT_SIZE` (one CMake knob; 4 KB here → 32 slots) drives BOTH the linker
`ORIGIN` (per image, via `teensy_add_cm4_slot_image`) and the runtime
`CM4_SLOT_STAGE(k)` the CM7 hands `bank.add()` — one source of truth.

| Image | Slot | link `ORIGIN` | stage (`CM4_SLOT_STAGE`) | Identity |
|-------|------|---------------|--------------------------|----------|
| A | 0 | `0x1FFE0000` | `0x20200000` | `0xA1A1A1A1` |
| B | 1 | `0x1FFE1000` | `0x20201000` | `0xB2B2B2B2` |
| C | 2 | `0x1FFE2000` | `0x20202000` | `0xC3C3C3C3` |
| D | **0** (shares A) | `0x1FFE0000` | `0x20200000` | `0xD4D4D4D4` |

## What it proves

- **Co-residency + no-copy flip:** A/B/C in distinct slots stay resident
  (`resABC=E`); `switchTo(A)` after B/C is a flip (`idA2=A1A1A1A1`).
- **Slot-scoped eviction:** `switchTo(D)` (slot 0) evicts A only (`resD=7` —
  B,C,D resident, A gone).
- **Re-stage (un-fakeable):** `switchTo(A)` after D re-copies A onto slot 0;
  `idA3=A1A1A1A1` (NOT `D4D4D4D4`) proves the bank re-staged rather than flipping
  onto D's resident blob. `resA3=E`.

## Gates

- **QEMU:** `cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build && ./run_qemu.sh` (never `sh run_qemu.sh`). No qemu2 change — the machine maps the full 256 KB `ocram_m4` backdoor and re-reads `GPR0/1` fresh per boot. GREEN, stable 3× (`transcript_qemu.txt`).
- **HW — clean controlled probe (wiring-free):** `clean_boot.scp` (CM4 held: `SCR=0`/`STAT_M4=1`), flash, dispatch, capture VCOM:
  ```sh
  pkill LinkServer; pkill redlinkserv
  # start the pyserial VCOM reader on /dev/cu.usbmodem5DQ2DDHVWO5EI3 @115200
  LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/cm4_imagebank_test.elf
  LinkServer probe <serial> runscript clean_boot.scp
  ```
  ★★HW-VERIFIED 2026-07-20 (`transcript_hw_evkb.txt`): the clean-boot snapshot
  showed `SCR=0`/`STAT_M4=1` (CM4 genuinely held), then the token stream came
  out **byte-identical to QEMU**, stable 2× — `resABC=E`, `idA2=A1A1A1A1`,
  `resD=7`, **`idA3=A1A1A1A1` (not `D4`)**, `resA3=E`, `IMAGEBANK=PASS`.

## What this unlocks

An A/B (or A/B/C/…) CM4 firmware bank switched by handle with a fast VTOR flip;
paging more images than fit resident by sharing slots. Foundation for
SD-loaded CM4 images (spec §8): the bank's `blob` is already source-agnostic.
