# cm4_wire_test — CM4 self-configured polled I2C to the WM8962 (Phase 3.2)

The second per-library CM4 enablement: a bare-metal **CM4 image
self-configures LPI2C5** (the on-board codec bus — `CCM_LPCG102`,
`CCM_CLOCK_ROOT41` mux 1, **LPSR-domain** pads `GPIO_LPSR_05/04`) and runs
three **polled I2C master transactions against the real on-board WM8962
@0x1A**, streaming observations over the **MU** to the CM7 (a pure reporter
that never touches Wire/LPI2C), which prints them on LPUART1/VCOM.

The CM4 driver (`cm4/main_cm4.c`) is a C re-expression of this project's own
HW-verified `newdigate/Wire` `WireIMXRT1176.cpp` master path + the WM8962
protocol from `newdigate/Audio` `control_wm8962.cpp` — same registers, clock,
pins; no CM7 core, no C++ runtime. (Phase 3.3 consolidates onto a shared C
core.)

| token | QEMU | HW | proves |
|---|---|---|---|
| `mcr`   | `00000001` | `00000001` | the CM4 enabled the LPI2C master block |
| `lpcg`  | `00000001` | `00000001` | CCM_LPCG102 readback (**informative**) |
| `croot` | `00000100` | `00000100` | CCM_CLOCK_ROOT41 readback (**informative**) |
| `ack`   | `00000000` | `00000000` | reset-write `R15←0x6243` @0x1A **ACKed** |
| `nack`  | `00000002` | `00000002` | absent addr `0x2A` **address-NACKed** (avoids WM8962 0x1A + FXLS8974 accel 0x18) |
| `rdn`   | `00000002` | `00000002` | ID read-back returned 2 bytes (repeated START) |
| `rdv`   | `00000000` | `00006243` | **the one expected divergence** — see below |
| `done`  | `00000001` | `00000001` | CM4 sequence completed |

## `rdv`: the deliberate QEMU-vs-silicon split

QEMU's `wm8962-stub` (`hw/i2c/wm8962_stub.c`) "ACKs all writes and returns
0x00 for all reads — it is NOT a codec model", so the QEMU runner asserts
`rdv=00000000` (the stub contract).  Real silicon answers the **WM8962 device
ID `0x6243`** — the R15 readback default (Linux `wm8962.c` reg_default
`{ 15, 0x6243 }`, used as a hardware fact only).  Write-the-ID-then-read-the-ID
is self-evidencing: a stuck-low bus reads `0x0000`, stuck-high reads `0xFFFF` —
only a live codec on a CM4-brought-up bus says `0x6243`.  The two committed
transcripts therefore differ on **exactly this one line** (precedent:
`cm4_intr_test`'s `systick` characterisation token).

## Why the hardware run is the real proof

Same circular-pass structure as `cm4_spi_test`: the qemu2 LPI2C model + stub
respond on `MCR.MEN` alone, ignoring the clock gate, clock root, and LPSR pin
mux — a CM4 that skipped those writes would still pass in QEMU.  Silicon's
`ack=0` + `rdv=6243` through the on-board pull-ups are the only proof the CM4
itself brought up the clock and the LPSR pins.  ACK/NACK is judged at STOP
completion (never at TDF, which leads the ACK bit by a byte-time on silicon —
the qemu model's deferred-NDF was corrected against this exact trap).

## Build / run (QEMU)

    cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake .
    cmake --build build
    ./run_qemu.sh

## Hardware (EVKB — the final arbiter, WIRING-FREE)

No jumper needed — the WM8962 and its pull-ups are soldered on (codec I2C
needs no MCLK for register access).  For an uncontaminated boot:

    python3 ~/Development/rt1170/rt1170-console.py \
        /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > /tmp/hw.uart &
    /Applications/LinkServer_26.6.137/LinkServer flash \
        MIMXRT1176:MIMXRT1170-EVKB load build/cm4_wire_test.elf
    sleep 3; : > /tmp/hw.uart      # drop contaminated post-flash output
    /Applications/LinkServer_26.6.137/LinkServer probe 5DQ2DDHVWO5EI \
        runscript ~/Development/rt1170/evkb/dualcore_mu_test/clean_boot.scp

Confirm `WIRE_CM4=PASS`, the asserted tokens byte-identical to
`transcript_qemu.txt`, and **`rdv=00006243`**.  Strip the leading `\0` block
from the capture (console reconnect artifact): `tr -d '\000' < /tmp/hw.uart >
transcript_hw_evkb.txt`.

## Reference transcripts

- `transcript_qemu.txt` — QEMU mimxrt1170-evk (`rdv=00000000`, stub)
- `transcript_hw_evkb.txt` — EVKB clean boot (`rdv=00006243`, real codec)

## Layout

- `cm4/` — the CM4 sketch: `main_cm4.c` (self-config LPI2C5 + 3 transactions +
  MU stream), `startup_cm4.S` + `cm4.ld` (shared with `cm4_spi_test`).
- `cm4_wire_test.cpp` — the CM7 sketch (boot CM4 + MU reporter; never touches Wire).
