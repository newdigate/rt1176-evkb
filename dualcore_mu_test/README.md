# dualcore_mu_test — RT1176 CM4/SRC/MU probe (QEMU vs silicon)

CM7 firmware that boots the CM4 with an embedded probe blob and prints one
`token=HEXVALUE` line per observation over LPUART1, so a hardware transcript
and a QEMU transcript diff directly. Covers: CM4 held at boot, release via
IOMUXC_LPSR_GPR VTOR + `SRC_SCR.BT_RELEASE_M4`, both MU mailbox directions,
the GIR/GIP doorbell (incl. GIR auto-clear on ack), the MU interrupt on
NVIC 118, `CTRL_M4CORE` SW-reset restart of a running core, and the
write-1-only stickiness of the release bit.

## Build

    cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake .
    cmake --build build

## Run in QEMU

    qemu-system-arm -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on \
        -kernel build/dualcore_mu_test.elf -display none -serial stdio

## Run on the EVKB

    LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/dualcore_mu_test.elf
    # then, for an uncontaminated run (see below):
    LinkServer probe <serial> runscript clean_boot.scp
    # console: rt1170-console.py (LPUART1 via the MCU-Link VCOM)

**Contamination warning:** LinkServer's RT1176 connect script
(`RT1170_connect_M7_wake_M4.scp`) wakes the CM4 into a spin loop at
0x2021FF00 and sets `SCR.BT_RELEASE_M4`, and `SCR` survives debugger resets
(the bit is write-1-only). A post-flash run therefore starts with
`scr_pre=00000001` and a woken CM4. `clean_boot.scp` produces a clean run
headlessly: SYSRESETREQ (SoC back to reset state, CM4 held), then a manual
dispatch of the image at 0x30002000 — needed because the boot ROM parks
after a debugger-initiated reset on this silicon. A power cycle or SW4 works
too.

## Reference transcripts

- `transcript_hw_evkb.txt` — MIMXRT1170-EVKB, clean boot (2026-07-16)
- `transcript_qemu.txt` — QEMU mimxrt1170-evk after the silicon-matching fixes

Expected differences (3 tokens): `lpcg`, `gpcst` (CCM LPCG / GPC blocks not
modelled in QEMU, read 0 there) and `girset` (timing-sensitive: silicon's CM4
acks the doorbell faster than the CM7 reads CR back; in QEMU the CM4 vCPU
usually hasn't run yet, so GIR0 still reads 1).

## Silicon findings baked into the QEMU model

- Boot VTOR must be the system/backdoor address (0x20200000). A CM4-private
  TCM address (0x1FFE0000) releases the slice (`STAT_M4CORE` 1→0) but the
  core never fetches. SP/PC values *inside* the vector table may point into
  the CM4 TCM views.
- `SCR.BT_RELEASE_M4` is write-1-only: a 0 write is ignored (readback keeps
  1, the CM4 keeps running, and a following 1 write is not a release edge).
- `ASR` bit 9 reads 1 in every observed state; the RM/SDK-documented RS
  (bit 7) never reads 1 on silicon, even with the CM4 held. The reliable
  hold indicator is `SRC STAT_M4CORE` bit 0 (1 held, 0 released).
- `CTRL_M4CORE.SW_RESET` reads back 0 immediately and restarts a released
  core from the (re-)programmed LPSR GPR vector table.
