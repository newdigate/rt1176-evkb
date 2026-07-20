# cm4_image_test — real compiled CM4 image gate (Phase 2A)

The keystone of Phase 2: a **real compiled Cortex-M4 image** (its own startup +
linker, in `cm4/`), staged by the Phase-1 `Multicore.begin()` and run on the
CM4. Unlike the Phase-1 hand-assembled leaf blob, this image runs a genuine
reset handler — it copies `.data` (ITCM LMA → DTCM VMA), zeroes `.bss`, enables
the M4F FPU, and uses a DTCM stack — then reports three "canaries" over the MU
that only read correct if that startup machinery worked:

| token | value | proves |
|---|---|---|
| `data`  | `DA7A0001` | `.data` copied ITCM→DTCM (CM4-private views alias the backdoor) |
| `bss`   | `00000B55` | `.bss` zero-initialised in CM4 DTCM |
| `stack` | `0000008C` | DTCM stack works (0+1+4+9+16+25+36+49 = 140) |
| `echo`  | `12345679` | the real image keeps running + MU round-trips |
| `data2/bss2/stack2` | (repeat) | `Multicore.restart()` re-runs the whole startup |

## Layout

- `cm4/` — the bare-metal CM4 sub-image (NOT the Arduino core):
  - `cm4.ld` — ITCM `0x1FFE0000` / DTCM `0x20000000`, 128 K each; `.data` LMA in
    ITCM so the whole image stages contiguously; `__StackTop = 0x20020000`
    (capped at the CM4's 128 K DTCM — reusing the CM7's `0x20040000` would bus-fault).
  - `startup_cm4.S` — minimal reset handler: relocate VTOR to `0x1FFE0000`,
    reload MSP, enable FPU, copy `.data`, zero `.bss`, call `main`. No clock/PLL/
    FlexRAM/DCDC (the CM7 + boot ROM already did those; the CM4 TCM is fixed LMEM).
  - `main_cm4.c` — the heartbeat.
- `cm4_image_test.cpp` — the CM7 gate sketch (boots the image, asserts canaries).

The CM4 image is built as a **first-class dual target** (Phase 2B) by
`teensy_add_cm4_image` (in `teensy-cmake-macros`): it compiles the `cm4/`
sources with cortex-m4 flags + their own linker and emits `cm4_image.h` (a
`uint32_t[]`) into the build dir; `teensy_target_link_cm4_image` embeds it in
the CM7 sketch. The generated binary is byte-identical to the HW-verified 2A
image.

## Build / run

    cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake .
    cmake --build build
    ./run_qemu.sh                       # the gate: asserts 9 tokens
    # EVKB: flash + clean_boot.scp (see cm4_boot_test/README.md for the recipe)

## Reference transcripts

- `transcript_hw_evkb.txt` — MIMXRT1170-EVKB, clean boot (2026-07-17)
- `transcript_qemu.txt`    — QEMU mimxrt1170-evk

**Byte-identical** on 2026-07-17: the real-image CM4 memory path (staging,
`.data` ITCM→DTCM copy, CM4-private DTCM read/write, FPU, VTOR relocation)
behaves the same on silicon and in the qemu2 model. No token is timing-sensitive
here (the CM4 sends before the CM7 polls).
