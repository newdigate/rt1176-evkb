# cm4_boot_test — CM4 boot + MU library gate (Phase 1)

CM7 firmware that boots an embedded CM4 blob through the Phase-1 dual-core
library (`cores/imxrt1176/Multicore.h` + `MessagingUnit.h`) and exercises the
MU end-to-end, printing one `token=HEXVALUE` line per observation over Serial1
(LPUART1) so a QEMU transcript and an EVKB transcript diff directly. Same
discipline as `dualcore_mu_test`, but driven entirely through the library API
instead of raw registers.

Covers: `Multicore.begin()` (stage blob → LPSR-GPR VTOR → SRC release),
`Multicore.running()`/`restart()`, MU mailbox receive, the GIR/GIP doorbell
handshake incl. GIR auto-clear, an interrupt-driven receive callback on
NVIC 118, and a CM4 restart.

## Build

    cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake .
    cmake --build build

## Run in QEMU (the gate)

    ./run_qemu.sh          # asserts 10 deterministic tokens, PASS/FAIL

## Run on the EVKB

    # VCOM reader first, then flash (auto-runs), then a clean boot:
    python3 ../../rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > hw.uart &
    /Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/cm4_boot_test.elf
    sleep 3; : > hw.uart   # drop the contaminated post-flash autorun
    /Applications/LinkServer_26.6.137/LinkServer probe 5DQ2DDHVWO5EI \
        runscript ../dualcore_mu_test/clean_boot.scp   # clean boot (M4 held)

`clean_boot.scp` dispatches whatever flexspi image sits at 0x30002000 — which
is where this binary's vector table links — so it gives an uncontaminated run
(CM4 held, `SCR=0`), and `Multicore.begin()` performs the genuine write-1-only
`BT_RELEASE_M4` first-boot edge. See `dualcore_mu_test/README.md` for the
LinkServer debugger-contamination traps.

## Reference transcripts

- `transcript_hw_evkb.txt` — MIMXRT1170-EVKB, clean boot (2026-07-17)
- `transcript_qemu.txt`    — QEMU mimxrt1170-evk

They were **byte-identical** on 2026-07-17. The only token that may differ
between runs is `gir` (GIR0 still pending immediately after `MU.trigger(0)`):
it is timing-sensitive — on real silicon the CM4 usually acks before the CM7
reads it back (`gir=0`), while in QEMU the CM4 vCPU sometimes has not run yet
(`gir=1`). `run_qemu.sh` does not assert it. All other tokens are
deterministic and identical HW-vs-QEMU.
