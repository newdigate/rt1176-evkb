# probe_firmware template

Copy this directory, rename `probe.s`, and replace the EXAMPLE PROBES
section. Keep tokens fixed-order and values raw-hex so transcripts diff.

Build:

    /Applications/ARM_10/bin/arm-none-eabi-gcc -nostdlib -mcpu=cortex-m7 \
        -mthumb -Wl,-Ttext=0x30000000 -o probe.elf probe.s

Run on QEMU and on the EVKB, and diff — commands and the debugger
contamination traps are in the skill's `references/silicon-truth-loop.md`.
NOTE: under QEMU the raw LPUART writes print with no init; on the EVKB
you must add a silicon-validated LPUART1 init first (see the probe.s
comment), or the hardware transcript will be empty.
For probes needing C (interrupt legs, larger flows), start from
`evkb/dualcore_mu_test/` instead.
