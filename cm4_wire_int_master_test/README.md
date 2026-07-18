# cm4_wire_int_master_test — CM4 interrupt-driven LPI2C5 master to the WM8962 (Phase 4.1)

The first per-slice CM4 enablement of Phase 4.1: a bare-metal **CM4 image
self-configures LPI2C5** (the on-board codec bus — `CCM_LPCG102`,
`CCM_CLOCK_ROOT41` mux 1, **LPSR-domain** pads `GPIO_LPSR_05/04`) and then
runs an **interrupt-driven master transaction** entirely from its own ISR:
it NVIC-enables **IRQ 36 (LPI2C5) on its own NVIC** — the first non-MU
peripheral IRQ routed to the CM4, via the qemu2 per-line split-IRQ — and
services the transfer state machine in `LPI2C5_IRQHandler`, reading the
real on-board **WM8962 R15 device ID**. Observations stream over the **MU**
to the CM7 (a pure reporter that never touches Wire/LPI2C5), which prints
them on LPUART1/VCOM.

This gate is a clone of `cm4_wire_test/` (Phase 3.2): the LPI2C5 self-config
half is unchanged (same shared C core), and the transaction protocol (write
register pointer `{0x00,0x0F}`, repeated-START read 2 bytes) mirrors that
precedent, but the polled write/read calls are replaced end-to-end by a
**fresh** interrupt-driven master state machine — the CM7's own I2C path is
polled, so there was no existing ISR-master to distill. Its shape (per-phase
`MIER`, ACK/NACK judged at STOP completion, repeated-START for the read) is
validated behaviorally against the NXP SDK `LPI2C_MasterTransferHandleIRQ` /
`LPI2C_RunTransferStateMachine` and Zephyr, and written clean per the
project license firewall — no SDK code is copied.

| token | QEMU | HW | proves |
|---|---|---|---|
| `irqcnt` | `00000006` | `>0` (magnitude may differ) | the CM4 serviced the LPI2C5 IRQ on its own NVIC (isolated routing proof) |
| `mcr`    | `00000001` | `00000001` | the CM4 enabled the LPI2C master block |
| `lpcg`   | `00000001` | `00000001` | CCM_LPCG102 readback (**informative**) |
| `croot`  | `00000100` | `00000100` | CCM_CLOCK_ROOT41 readback (**informative**) |
| `rdv`    | `00000000` | `00006243` | **the one expected content divergence** — see below |
| `err`    | `00000000` | `00000000` | ISR outcome — 0 = OK, no NDF/ALF/FEF (**asserted in both worlds**) |
| `done`   | `00000001` | `00000001` | CM4 interrupt-driven sequence completed |

`WIRE_INT_MASTER_CM4=PASS` requires `irqcnt>0 && mcr=1 && err=0 && done=1`.
`rdv` is deliberately **not** folded into that verdict — it is asserted
per-world by the runner (QEMU) / by the operator (HW) instead.

## `rdv` and `irqcnt`: the two QEMU-vs-silicon divergences

This is the same QEMU-vs-silicon split established at Phase 3.2, applied to
this gate's own `rdv` token: the QEMU `wm8962-stub` (`hw/i2c/wm8962_stub.c`)
ACKs all writes and returns `0x00` for all reads — it is not a codec model —
so the QEMU runner asserts `rdv=00000000` (the stub contract). Real silicon
answers the **WM8962 device ID `0x6243`** (the R15 readback default — a
hardware fact taken from Linux's `wm8962.c` reg_default table, used as a
fact only, no code from that GPL source). A stuck-low bus reads `0x0000`,
stuck-high reads `0xFFFF` — only a live codec, read back through the CM4's
own interrupt-driven master, says `0x6243`.

`irqcnt` can *also* differ in magnitude between worlds: the ISR reacts to
whatever `MSR` flags are present on each entry rather than assuming a fixed
interrupt count, so QEMU's FIFO cadence (which may drain more eagerly) and
silicon's need not produce the same tally. That is why only `irqcnt>0` is
asserted (the CM4 took the IRQ at all) — never equality with the QEMU value.

## Why the hardware run is the real proof

Same circular-pass shape as the Phase 3.2 polled gate: the qemu2 LPI2C model
+ stub respond on `MCR.MEN` alone, ignoring the clock gate, clock root, and
LPSR pin mux — a CM4 that skipped those self-config writes would still pass
in QEMU. But this gate adds a second circularity specific to Phase 4.1: the
**split-IRQ routing itself is a qemu2 software model**, so a green QEMU run
only proves the CM4's ISR correctly handles LPI2C5 *once qemu2 decides to
deliver the interrupt there* — it says nothing about whether real silicon's
interrupt distribution actually routes external IRQ 36 to the CM4's own
NVIC (as opposed to the CM7's, or nowhere).

The wiring-free EVKB run retires both circularities at once: `irqcnt>0`
together with `rdv=00006243` is only possible if the CM4 (a) brought up the
LPI2C5 clock and LPSR pins itself, **and** (b) actually took the LPI2C5
interrupt on its own NVIC and serviced a real transaction against the
on-board WM8962 from that ISR. That second half — the split routing a
peripheral IRQ to the CM4 on real silicon — is the new thing this hardware
run proves that Phase 3.2's polled precedent could not.

## Build / run (QEMU)

    cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake .
    cmake --build build
    ./run_qemu.sh

## Hardware (EVKB — the final arbiter, WIRING-FREE)

No jumper needed — the WM8962 and its pull-ups are soldered on (codec I2C
needs no MCLK for register access). For an uncontaminated boot:

    python3 ~/Development/rt1170/rt1170-console.py \
        /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > /tmp/hw.uart &
    /Applications/LinkServer_26.6.137/LinkServer flash \
        MIMXRT1176:MIMXRT1170-EVKB load build/cm4_wire_int_master_test.elf
    sleep 3; : > /tmp/hw.uart      # drop contaminated post-flash output
    /Applications/LinkServer_26.6.137/LinkServer probe 5DQ2DDHVWO5EI \
        runscript ~/Development/rt1170/evkb/dualcore_mu_test/clean_boot.scp

Confirm `WIRE_INT_MASTER_CM4=PASS` and that `mcr`, `err`, and `done` are
byte-identical to `transcript_qemu.txt`, with `irqcnt>0` (its exact value
may differ from QEMU's — only `>0` is asserted), and **`rdv=00006243`**.
Strip the leading `\0` block from the capture (console reconnect artifact):
`tr -d '\000' < /tmp/hw.uart > transcript_hw_evkb.txt`.

## Reference transcripts

- `transcript_qemu.txt` — QEMU mimxrt1170-evk (`rdv=00000000`, stub;
  `irqcnt=00000006`), checked in.
- `transcript_hw_evkb.txt` — EVKB clean boot (`rdv=00006243`, real codec);
  added by the operator after the wiring-free hardware probe (not yet
  present at the time of this commit).

## Layout

- `cm4/` — the CM4 sketch: `main_cm4.c` (shared self-config LPI2C5 core +
  the fresh interrupt-driven master state machine in `LPI2C5_IRQHandler` +
  MU stream), `startup_cm4.S` + `cm4.ld` (full vector table: SysTick@15,
  LPI2C5@52, MU@134).
- `cm4_wire_int_master_test.cpp` — the CM7 sketch (boot CM4 + MU reporter;
  never touches Wire/LPI2C5).
