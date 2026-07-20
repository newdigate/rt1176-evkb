# cm4_hotswap2_test — two co-resident CM4 images, VTOR switch (★★HW-VERIFIED)

Two CM4 programs **resident in ITCM at once**; the CM7 switches the CM4's boot
VTOR between them by reprogramming `IOMUXC_LPSR_GPR0/1` + re-pulsing `SW_RESET`
— **without re-staging**. Proves the literal D7 "reboot at a *new* VTOR" and
delivers fast switching between co-resident CM4 firmware images. Builds on
`cm4_hotswap_test` (swap-in-place).

## Layout (the two halves)

| Image | ITCM link base | Backdoor stage addr |
|-------|----------------|---------------------|
| A (`0xA1A1A1A1`) | `0x1FFE0000` (64 K) | `0x20200000` |
| B (`0xB2B2B2B2`) | `0x1FFF0000` (64 K) | `0x20210000` (= ITCM + 0x10000) |

The CM4's 128 K ITCM (`0x1FFE0000`) is aliased to the `0x20200000` backdoor, so
the two 64 K halves are genuinely co-resident. DTCM (`0x20000000`) is shared —
only one image runs at a time, so each boot re-inits its `.data`/`.bss`/stack.
One CM4 source (`cm4/main_cm4.c`), two linker scripts (`cm4_a.ld`/`cm4_b.ld`),
identity via `-DHS_IDENTITY`.

## The API — `Multicore::switchImage(stageAddr)`

New core method (`cores/imxrt1176/Multicore.{h,cpp}`): reprograms `GPR0/1` +
pulses `SW_RESET` to reboot an **already-staged** image at a (possibly new)
backdoor address, with **no image copy**. Completes the `Multicore` API:
`begin`=stage+boot, **`switchImage`=boot-a-resident-image**, `restart`=reboot-current.

## Sequence

`begin(A@0x20200000)` → `idA` · `begin(B@0x20210000)` → `idB` (both now resident)
· `switchImage(0x20200000)` → `idA2` · `switchImage(0x20210000)` → `idB2`.
`HOTSWAP2=PASS` requires `idA==idA2==A1A1A1A1 && idB==idB2==B2B2B2B2` — the
matching `idA2`/`idB2` prove both images **stayed resident** across the switches,
not merely that a reboot happened.

## Gates

- **QEMU:** `cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build && ./run_qemu.sh` (never `sh run_qemu.sh`). No qemu2 change — the machine maps the full 256 K `ocram_m4` backdoor and the boot handler re-reads `GPR0/1` fresh and accepts `0x20210000`.
- **HW — clean controlled probe (wiring-free):** `clean_boot.scp` (CM4 held), flash, dispatch, capture VCOM. ★★HW-VERIFIED 2026-07-20, stable 2×: snapshot `SCR=0`/`STAT_M4=1` (CM4 held), then `idA=A1A1A1A1`, `idB=B2B2B2B2` (booted at the **new VTOR** `0x20210000`), `idA2=A1A1A1A1`, `idB2=B2B2B2B2`, `HOTSWAP2=PASS` (`transcript_hw_evkb.txt`).
  ```sh
  pkill LinkServer; pkill redlinkserv
  # start the pyserial VCOM reader on /dev/cu.usbmodem5DQ2DDHVWO5EI3 @115200
  LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/cm4_hotswap2_test.elf
  LinkServer probe <serial> runscript clean_boot.scp
  ```

## What this unlocks

Multiple CM4 firmware images resident at once with fast VTOR-switch between them
(no re-flash, no re-copy) — an A/B firmware bank, or on-demand task images on the
CM4.
