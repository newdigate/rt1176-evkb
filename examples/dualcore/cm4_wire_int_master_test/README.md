# cm4_wire_int_master_test — CM4 interrupt-driven LPI2C5 master to the WM8962 (Phase 4.1)

The first per-slice CM4 enablement of Phase 4.1: a bare-metal **CM4 image
self-configures LPI2C5** (the on-board codec bus — `CCM_LPCG102`,
`CCM_CLOCK_ROOT41` mux 1, **LPSR-domain** pads `GPIO_LPSR_05/04`) and reads
the real on-board **WM8962 R15 device ID** via a **HYBRID** transaction: the
register-pointer **write** is the HW-verified **polled** core
(`lpi2c1176_master_write`, `sendStop=0`, bus held), then the repeated START +
(after a TDF wait) the RXD command are issued from `i2c_read_reg`, and the
**data read stays interrupt-driven** — the CM4 NVIC-enables **IRQ 36 (LPI2C5)
on its own NVIC** (the first non-MU peripheral IRQ routed to the CM4, via the
qemu2 per-line split-IRQ) and its `LPI2C5_IRQHandler` captures the RX bytes
(RDF) and completes on SDF. Observations stream over the **MU** to the CM7
(a pure reporter that never touches Wire/LPI2C5), which prints them on
LPUART1/VCOM.

**Status: ★★HW-VERIFIED (2026-07-19).** The design was *not* originally a
hybrid — see "Silicon-truth" below for the cold-bus race the EVKB probe
caught in the first (pure-ISR) implementation, and why the fix landed as a
hybrid rather than a pure interrupt-driven master.

This gate is a clone of `cm4_wire_test/` (Phase 3.2): the LPI2C5 self-config
half is unchanged (same shared C core), and the transaction protocol (write
register pointer `{0x00,0x0F}`, repeated-START read 2 bytes) mirrors that
precedent. The register-pointer **write** reuses the HW-verified **polled**
`lpi2c1176_master_write` verbatim (the same shared C core the CM7 `TwoWire`
master runs); only the **read completion** is a **fresh** interrupt-driven
state machine — the CM7's own I2C path is fully polled, so there was no
existing ISR-master to distill for that half. Its shape (per-phase `MIER`,
ACK/NACK judged at STOP/error completion) is validated behaviorally against
the NXP SDK `LPI2C_MasterTransferHandleIRQ` / `LPI2C_RunTransferStateMachine`
and Zephyr, and written clean per the project license firewall — no SDK code
is copied. **This write/read split is itself the fix for a cold-bus race the
first, pure-ISR implementation hit — see "Silicon-truth" below.**

| token | QEMU | HW | proves |
|---|---|---|---|
| `irqcnt` | `00000003` (world-varying, **not** byte-identical) | `00000004` (only `>0` asserted) | the CM4 serviced the LPI2C5 read IRQ on its own NVIC (isolated routing proof) |
| `mcr`    | `00000001` | `00000001` | the CM4 enabled the LPI2C master block |
| `lpcg`   | `00000001` | `00000001` | CCM_LPCG102 readback (**informative**) |
| `croot`  | `00000100` | `00000100` | CCM_CLOCK_ROOT41 readback (**informative**) |
| `rdv`    | `00000000` | `00006243` | world-varying by design — see below |
| `err`    | `00000000` | `00000000` | ISR outcome — 0 = OK, no NDF/ALF/FEF (**asserted in both worlds**) |
| `done`   | `00000001` | `00000001` | CM4 interrupt-driven sequence completed |

`WIRE_INT_MASTER_CM4=PASS` requires `irqcnt>0 && mcr=1 && err=0 && done=1`.
`rdv` is deliberately **not** folded into that verdict — it is asserted
per-world by the runner (QEMU) / by the operator (HW) instead.

**The byte-identical-except set is `{irqcnt, rdv}`** — both are world-varying
and neither is compared for cross-world equality (`irqcnt` only as `>0`,
`rdv` per-world). `mcr`, `lpcg`, `croot`, `err`, and `done` are byte-identical
HW vs QEMU. (Before the cold-bus fix below, only `rdv` varied by design;
`irqcnt` joining it is a direct, expected side effect of moving the
register-pointer write off the ISR.)

## `rdv` and `irqcnt`: the two QEMU-vs-silicon divergences

This is the same QEMU-vs-silicon split established at Phase 3.2, applied to
this gate's own `rdv` token: the QEMU `wm8962-stub` (`hw/i2c/wm8962_stub.c`)
ACKs all writes and returns `0x00` for all reads — it is not a codec model —
so the QEMU runner asserts `rdv=00000000` (the stub contract). Real silicon
answers the **WM8962 device ID `0x6243`** (the R15 readback default — a
hardware fact taken from Linux's `wm8962.c` reg_default table, used as a
fact only, no code from that GPL source). A stuck-low bus reads `0x0000`,
stuck-high reads `0xFFFF` — only a live codec, read back through the CM4's
hybrid (polled-write + interrupt-driven-read) master, says `0x6243`.

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
interrupt on its own NVIC and serviced the interrupt-driven read half of a
real transaction against the on-board WM8962 from that ISR (the register-
pointer write is polled — see "Silicon-truth" below). That second half — the
split routing a peripheral IRQ to the CM4 on real silicon — is the new thing
this hardware run proves that Phase 3.2's polled precedent could not.

## Silicon-truth: the cold-bus repeated-START race (found + fixed on the EVKB)

The **first** implementation of this gate was a **pure interrupt-driven
master**: the ISR pushed the register-pointer write bytes *and* issued the
repeated START for the read. It passed QEMU and every review, but on a
**cold boot** it deterministically read `rdv=0x0000` while the CM7 still
printed `WIRE_INT_MASTER_CM4=PASS` (`irqcnt>0`/`err=0`/`done=1` all still
held) — a false PASS.

**Root cause:** the ISR issued the repeated START the instant the write
cursor drained, racing the last register-pointer byte (`0x0F`) still
clocking out on a cold bus. The WM8962 never latched the pointer, so the
read landed on the wrong register. QEMU's `wm8962-stub` returns `0x00` for
every read regardless of which register was addressed, so it structurally
could not expose this — only the real codec on the EVKB could. Confirmed via
controlled probes: a polled-cold read returned `0x6243` (ruling out the
codec/bus), and an instrumented interrupt-first run also returned `0x6243`
with the same `irqcnt` (a timing Heisenbug, not a logic bug).

**Fix (`5736662`):** the design became the **hybrid** described above —
`i2c_read_reg` does the register-pointer write with the HW-verified polled
`lpi2c1176_master_write` (bus held, no STOP), waits for TDF, then issues the
repeated START + RXD itself, and only the **data read** is serviced by
`LPI2C5_IRQHandler`. Verified clean-boot 3× on the EVKB: `irqcnt=4`,
`rdv=00006243`, `err=0`, PASS every time.

## Build / run (QEMU)

    cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake .
    cmake --build build
    ./run_qemu.sh

## Hardware (EVKB — the final arbiter, WIRING-FREE)

No jumper needed — the WM8962 and its pull-ups are soldered on (codec I2C
needs no MCLK for register access). For an uncontaminated boot:

    python3 ~/Development/rt1170/evkb/tools/rt1170-console.py \
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
  `irqcnt=00000003`), checked in.
- `transcript_hw_evkb.txt` — EVKB clean boot, post-fix (`rdv=00006243`, real
  codec; `irqcnt=00000004`), checked in (`5736662`).

## Layout

- `cm4/` — the CM4 sketch: `main_cm4.c` (shared self-config LPI2C5 core +
  the polled register-pointer write + the fresh interrupt-driven read state
  machine in `LPI2C5_IRQHandler`/`i2c_read_reg` + MU stream), `startup_cm4.S`
  + `cm4.ld` (full vector table: SysTick@15, LPI2C5@52, MU@134).
- `cm4_wire_int_master_test.cpp` — the CM7 sketch (boot CM4 + MU reporter;
  never touches Wire/LPI2C5).
