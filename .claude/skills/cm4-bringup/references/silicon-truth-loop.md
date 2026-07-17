# The silicon-truth loop

## Probe pattern

A probe is firmware that prints one `token=HEXVALUE` line per observation
on LPUART1, in a fixed order, with zero timestamps or nondeterminism, so a
QEMU transcript and an EVKB transcript diff directly. Exemplars:
`evkb/dualcore_mu_test/` (C, boot-header image, with both reference
transcripts checked in) and `evkb/qemu_dcd_boot_test/` (asm). Start new
probes from `templates/probe_firmware/`.

Print raw register values, not just pass/fail — divergences must be
informative. Timing-sensitive readbacks (e.g. GIR auto-clear) are allowed
but must be marked as such when transcripts are compared.

## Running a probe

QEMU (image is a flexspi_nor boot-header image; the stub needs boot-xip):

    ~/Development/qemu2/build/qemu-system-arm -M mimxrt1170-evk \
        -global fsl-imxrt1170.boot-xip=on -kernel probe.elf \
        -display none -serial file:qemu.uart -monitor none

EVKB (MCU-Link on the debug USB; VCOM is the LPUART1 console):

    # console capture (survives reconnects); port from rt1170-flash.sh
    python3 ~/Development/rt1170/rt1170-console.py \
        /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > hw.uart &
    /Applications/LinkServer_26.6.137/LinkServer flash \
        MIMXRT1176:MIMXRT1170-EVKB load probe.elf     # auto-runs after
    sleep 3; : > hw.uart          # drop the contaminated post-flash output
    # for an UNCONTAMINATED run (see traps below):
    /Applications/LinkServer_26.6.137/LinkServer probe 5DQ2DDHVWO5EI \
        runscript ~/Development/rt1170/evkb/dualcore_mu_test/clean_boot.scp

Diff after stripping serial line endings:

    diff <(tr -d '\r\0' < qemu.uart) <(tr -d '\r\0' < hw.uart)

## Debugger contamination traps (all EVKB-verified)

- LinkServer's RT1176 connect script (`RT1170_connect_M7_wake_M4.scp`)
  WAKES THE CM4 into a spin loop at 0x2021FF00, sets CLOCK_ROOT1=0x201,
  and releases `SCR.BT_RELEASE_M4`.
- `SCR.BT_RELEASE_M4` is write-1-only and survives LinkServer's
  flash/reset flow (only a full system reset clears it): a post-flash
  run starts with `SCR=1` and a woken CM4.
- `DEMCR.VC_CORERESET` stays latched from flash sessions: wire or
  SYSRESETREQ resets halt silently at the reset vector.
- The boot ROM parks after any debugger-initiated reset — only a true POR
  (SW4 / power cycle) re-runs the flexspi boot AND the ROM's XMCD/DCD
  pass. `clean_boot.scp` works around all of this headlessly: SYSRESETREQ
  (SoC back to reset state, CM4 held, SCR=0), snapshot registers over the
  DAP while nothing has run, then manually dispatch the image at
  0x30002000. Note what it CANNOT do: exercise the real ROM's XMCD/DCD.

## "Silicon wins" bookkeeping

- qemu2 model changes cite the measurement: probe name + date in the code
  comment and the commit message (see `hw/misc/imxrt_mu.c` ASR bit-9
  comment for the pattern).
- Expected divergences are listed where the transcripts live (see
  `dualcore_mu_test/README.md`): currently unmodelled CCM-LPCG/GPC reads
  and timing-sensitive GIR readback.
- One board, one silicon rev: findings are recorded as measurements, not
  universal truths. If an errata or RM revision later explains one,
  update the comment.

## qemu2 regression set (run when qemu2 is touched)

From `~/Development/qemu2/build`:

    ninja qemu-system-arm
    # functional suites (imxrt1170 + imxrt1062, all must pass):
    export QEMU_TEST_QEMU_BINARY=$PWD/qemu-system-arm \
           QEMU_TEST_ARM_GCC=/Applications/ARM_10/bin/arm-none-eabi-gcc \
           MESON_BUILD_ROOT=$PWD \
           PYTHONPATH=$PWD/../tests/functional:$PWD/../python
    ./pyvenv/bin/python3 ../tests/functional/arm/test_imxrt1170.py
    ./pyvenv/bin/python3 ../tests/functional/arm/test_imxrt1062.py
    # repo gates most affected by dual-core work:
    #   evkb/serial_test/run_qemu.sh, dualcore_mu_test (diff its
    #   transcript_qemu.txt), NXP SDK mcmgr hello_world + rpmsg pingpong
    git diff | ../scripts/checkpatch.pl --no-signoff -   # CI enforces it
