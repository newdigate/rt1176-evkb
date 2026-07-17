# cm4_dual_test — dual-sketch blink + IPC capstone (Phase 2D)

The Phase-2 capstone: two independent sketches run concurrently on the two cores
and cooperate. The CM7 sketch (the Arduino core) and the CM4 sketch (a real
compiled image built via the Phase-2B `teensy_add_cm4_image` macro) each drive
their own GPIO ("blink") and exchange messages over the MU ("IPC").

| token | value | proves |
|---|---|---|
| `cm7led`  | `00000001` | CM7 blinked LED_BUILTIN (GPIO3.3) via the Arduino core, left high |
| `cm4gpio` | `00001000` | the CM4 configured + drove its **own** GPIO5.12 output (its DR readback) |
| `cm7sees` | `00001000` | the CM7 reads the **same** GPIO5.DR — the CM4's GPIO write is visible cross-core (shared peripheral) |
| `ipc`     | `00000037` | bidirectional IPC: CM7 sent `0x10`, the CM4 replied `f(0x10)=0x10*3+7=0x37` from its MU interrupt handler |

So both cores independently drive hardware, and they exchange computed IPC — the
whole Phase-1/2 stack (CM4 boot, MU, real CM4 image, CM4 NVIC, CM4 GPIO)
composed into one demo.

## GPIO readback: DR, not PSR

Both cores verify their GPIO drive by reading **DR**, not PSR/`digitalRead()`.
The qemu2 `imxrt_gpio` model returns `psr & ~gdir` — output bits are masked from
PSR (PSR models the external pad, undriven in the model), so PSR-readback of an
output reads 0 in QEMU while silicon reflects the driven pad. DR readback
(`(dr & gdir) | (psr & ~gdir)`) returns the driven value in both, so it is the
QEMU/silicon-consistent check. (On real hardware the pads also physically
toggle; that is observable with a scope/LED but not through the model's PSR.)

## Reference transcripts

- `transcript_hw_evkb.txt` — MIMXRT1170-EVKB, clean boot (2026-07-17)
- `transcript_qemu.txt`    — QEMU mimxrt1170-evk

**Byte-identical** on 2026-07-17: the CM4 driving GPIO5, the cross-core GPIO
read, and the IPC compute behave the same on silicon and in the model.

## Build / run

    cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake .
    cmake --build build
    ./run_qemu.sh
    # EVKB: flash + clean_boot.scp (see cm4_boot_test/README.md)

## Layout

- `cm4/` — the CM4 sketch: `main_cm4.c` (drive GPIO5.12 + the IPC responder),
  `startup_cm4.S` + `cm4.ld` (shared with cm4_intr_test).
- `cm4_dual_test.cpp` — the CM7 sketch (LED blink + IPC driver + cross-core read).
