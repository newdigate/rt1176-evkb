# cm4_intr_test — CM4 NVIC + timing gate (Phase 2C)

A real compiled CM4 image (built via the Phase-2B `teensy_add_cm4_image` macro)
that stands up the CM4's own interrupt and timing units and reports over the MU:

| token | value | proves |
|---|---|---|
| `dwt`     | `D00D0001` | the CM4's **DWT CYCCNT** advanced — the silicon-safe timing base (the CM7 core uses DWT, not SysTick, because the RT1176 SysTick tick ISR is unreliable — see `cores/imxrt1176/delay.c`) |
| `irqecho` | `ABCD0001` | the CM4 handled the **MU interrupt (external NVIC IRQ 118)** in its own ISR and echoed CM7's message+1 — CM4 external NVIC dispatch (Phase 1 only proved the CM7 side of IRQ 118) |
| `systick` | count | **characterisation probe, NOT asserted** — how many CM4 SysTick exceptions fired |

## The SysTick finding

The CM7's SysTick millis-ISR is unreliable on RT1176 (`delay.c`: the counter is
corrupted rather than incremented; the core switched to DWT CYCCNT). This gate
measures whether that trap extends to the CM4. **It does not:** the CM4 SysTick
exception fires and increments a counter reliably (HW: `systick=000010D3` = 4307
over the report window). So SysTick *exceptions* work on the CM4 — but DWT
remains the recommended timing base for parity with the CM7 core and because the
CM7 SysTick issue was never root-caused.

## Reference transcripts

- `transcript_hw_evkb.txt` — MIMXRT1170-EVKB, clean boot (2026-07-17)
- `transcript_qemu.txt`    — QEMU mimxrt1170-evk (`-icount shift=2`)

**Expected difference (1 token):** `systick` — QEMU `00002AC5` (10949) vs HW
`000010D3` (4307). Both are non-zero (SysTick fires in both); the exact count
differs because the time base differs (QEMU's `-icount shift=2` vs the real
400 MHz core clock with a 1000-cycle reload). It is a characterisation value,
not asserted. All other tokens (`boot`, `run`, `dwt`, `irqecho`) are identical
HW-vs-QEMU.

## Build / run

    cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake .
    cmake --build build
    ./run_qemu.sh          # asserts boot/run/dwt/irqecho; reports systick
    # EVKB: flash + clean_boot.scp (see cm4_boot_test/README.md)

The `-icount shift=2` in `run_qemu.sh` gives SysTick/DWT a deterministic time
base in QEMU (the same reason the IntervalTimer gates use it).

## Layout

- `cm4/` — the CM4 image: `startup_cm4.S` (135-entry vector table: SysTick at
  exception 15, MU IRQ 118 at vector index 134), `main_cm4.c` (DWT + SysTick +
  the MU ISR), `cm4.ld` (TCM layout, shared with cm4_image_test).
- `cm4_intr_test.cpp` — the CM7 gate sketch.
