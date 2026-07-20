# cm4_spi_test — CM4 self-configured polled SPI (Phase 3.1)

The first per-library CM4 enablement: a bare-metal **CM4 image self-configures
LPSPI1** (the EVKB Arduino-header SPI) — ungates its clock, muxes its pins,
configures the block — and runs a **polled master self-loopback** (external
**SDO→SDI jumper**). The CM4 has no console, so it streams each observation over
the **MU** to the CM7 (a pure reporter that never touches LPSPI1), which prints
them on LPUART1/VCOM.

The CM4 driver (`cm4/main_cm4.c`) is a C re-expression of this project's own
HW-verified `newdigate/SPI` `SPIIMXRT1176.cpp` `begin()`+`transfer()` — same
registers/clock/pins, no CM7 core, no C++ runtime. (Phase 3.3 consolidates it
and the C++ class onto a shared C core.)

| token | value | proves |
|---|---|---|
| `cr`    | `00000001` | the CM4 set LPSPI `CR.MEN` — block enabled |
| `cfgr1` | `00000001` | `CFGR1.MASTER` — master mode |
| `lpcg`  | `00000001` | CCM_LPCG104 readback (**informative**, not asserted) |
| `croot` | `00000000` | CCM_CLOCK_ROOT43 readback (**informative**, not asserted) |
| `a`     | `000000A5` | polled loopback echoed `0xA5` (8-bit) |
| `b`     | `0000003C` | polled loopback echoed `0x3C` (8-bit) |
| `w`     | `0000BEEF` | `transfer16` echoed `0xBEEF` (16-bit) |
| `buf`   | `DEADBEEF` | 4-byte buffer `{DE,AD,BE,EF}` echoed |
| `rxok`  | `00000001` | all loopback bytes matched → `SPI_CM4=PASS` |

## Why the hardware run is the real proof (QEMU is necessary, not sufficient)

The qemu2 board attaches an `ssi-loopback` child to LPSPI1
(`hw/arm/mimxrt1170-evk.c`); `imxrt_lpspi_transfer` echoes `rx=tx` **as soon as
`CR.MEN` is set — ignoring the LPCG clock gate, the clock root, and the pin
mux**. So a CM4 image that skipped the `CCM_LPCG104`/`CCM_CLOCK_ROOT43`/IOMUXC
writes would **still print `a=000000A5` and pass in QEMU** — a *circular pass*
(same shape as FlexCAN's SRXDIS gap). Therefore **`rx==tx` through the physical
SDO→SDI jumper on the EVKB is the only proof the CM4 brought up the functional
clock + pins itself.** The QEMU gate proves the register/transfer *sequence*;
silicon proves the *gating*. `lpcg`/`croot` are printed for HW diagnosis (a
failure localizes to gate/root/block/shift) but are not asserted, since a CCM
status bit may read differently on silicon without meaning failure.

## Build / run (QEMU)

    cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake .
    cmake --build build
    ./run_qemu.sh

## Hardware (EVKB — the final arbiter)

Jumper **SDO (GPIO_AD_30) → SDI (GPIO_AD_31)** on the Arduino header. Then, for
an uncontaminated boot (LinkServer's connect script otherwise wakes the CM4 and
pokes CLOCK_ROOT1 — which would mask a CM4 clock-config bug):

    python3 ~/Development/rt1170/evkb/tools/rt1170-console.py \
        /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > transcript_hw_evkb.txt &
    /Applications/LinkServer_26.6.137/LinkServer flash \
        MIMXRT1176:MIMXRT1170-EVKB load build/cm4_spi_test.elf
    sleep 3; : > transcript_hw_evkb.txt      # drop contaminated post-flash output
    /Applications/LinkServer_26.6.137/LinkServer probe 5DQ2DDHVWO5EI \
        runscript ~/Development/rt1170/evkb/dualcore_mu_test/clean_boot.scp

Confirm `SPI_CM4=PASS` and that the asserted tokens are byte-identical to
`transcript_qemu.txt`; record the observed `lpcg`/`croot`.

## Reference transcripts

- `transcript_qemu.txt` — QEMU mimxrt1170-evk
- `transcript_hw_evkb.txt` — MIMXRT1170-EVKB, clean boot, SDO→SDI jumper

## Layout

- `cm4/` — the CM4 sketch: `main_cm4.c` (self-config LPSPI1 + polled loopback +
  MU stream), `startup_cm4.S` + `cm4.ld` (shared with `cm4_dual_test`).
- `cm4_spi_test.cpp` — the CM7 sketch (boot CM4 + MU reporter; never touches SPI).
