# probe_firmware template

Copy this directory, rename `probe.s`, and replace the EXAMPLE PROBES
section. Keep tokens fixed-order and values raw-hex so transcripts diff.

Build:

    /Applications/ARM_10/bin/arm-none-eabi-gcc -nostdlib -mcpu=cortex-m7 \
        -mthumb -Wl,-Ttext=0x30000000 -o probe.elf probe.s

Run on QEMU and on the EVKB, and diff — commands and the debugger
contamination traps are in `../../references/silicon-truth-loop.md`.
For probes needing C (interrupt legs, larger flows), start from
`evkb/dualcore_mu_test/` instead.
