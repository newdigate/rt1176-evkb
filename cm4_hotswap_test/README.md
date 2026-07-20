# cm4_hotswap_test — CM4 runtime hot-swap (D7, ★★HW-VERIFIED)

The CM7 boots the CM4 with **image A**, then reboots the *running* CM4 into a
**different program** (image B) at runtime via a second `Multicore.begin()` —
which re-pulses `SRC_CTRL_M4CORE.SW_RESET`. Each image streams a distinct
identity over the MU; seeing **A's identity then B's** proves the hot-swap.
Resolves the last open Phase-1 item, **D7** (restart a running CM4 at a new
VTOR), with the clean controlled probe it had lacked.

## How it works (the machinery already existed)

- `Multicore::begin()` stages the image at the `0x20200000` backdoor, programs
  `IOMUXC_LPSR_GPR0/1` (the CM4 boot VTOR), and pulses `SW_RESET` on every call
  (gating only the one-time `BT_RELEASE_M4`). So a **second `begin(imageB)` is
  the hot-swap edge** — it overwrites the staged image and re-resets the CM4.
- **Swap-in-place:** two images built from one source (`cm4/main_cm4.c`) via
  `-DHS_IDENTITY` (A=`0xA1A1A1A1`, B=`0xB2B2B2B2`), both staged at `0x20200000`.
  Each `mu_send`s a ready handshake + its identity, then parks in **WFI** (so
  the CM4 isn't fetching its own code while the CM7 overwrites the backdoor with
  B just before the reset lands). Both images are embedded in the one CM7 elf
  (`teensy_target_link_cm4_image` called twice).

## Tokens (MU ch0)

`readyA=CAFE0001`, `idA=A1A1A1A1`, `readyB=CAFE0001`, `idB=B2B2B2B2`.
`HOTSWAP=PASS` requires `idA==A1A1A1A1 && idB==B2B2B2B2` (A ran, then B after the
swap). Identity-distinct so a stale/duplicate MU token can't false-pass.

## Gates

- **QEMU:** `cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build && ./run_qemu.sh` (never `sh run_qemu.sh`). qemu2's `fsl_imxrt1170_cm4_boot()` re-reads `GPR0/1` fresh + `cpu_reset()`s (it explicitly restarts a *running* CM4), so this passes with no qemu2 change.
- **HW — clean controlled probe (wiring-free):** D7's whole point is an *isolated* proof (the 2026-07-17 observation was confounded by LinkServer pre-releasing the CM4). Use `clean_boot.scp` (SYSRESETREQ → CM4 held, snapshot, then dispatch the CM7 image without waking the M4):
  ```sh
  pkill LinkServer; pkill redlinkserv
  # start the pyserial VCOM reader on /dev/cu.usbmodem5DQ2DDHVWO5EI3 @115200
  LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/cm4_hotswap_test.elf
  LinkServer probe <serial> runscript clean_boot.scp
  ```
  ★★HW-VERIFIED 2026-07-20, stable 2×: the snapshot showed **`SCR=0`, `STAT_M4=1`**
  (CM4 genuinely held — no contamination), then `idA=A1A1A1A1` → `idB=B2B2B2B2`,
  `HOTSWAP=PASS` (`transcript_hw_evkb.txt`). The CM7's `begin(A)` was the first,
  clean release; `begin(B)` rebooted the running CM4 into a different program.

## What this unlocks

Runtime CM4 firmware swapping — load a new CM4 image and restart into it without
a board reset. (A "new VTOR / two resident images" variant would re-link a second
image for a different backdoor address + reprogram `GPR0/1`; the qemu2 handler
already re-reads `GPR0/1` fresh, so only a second linker layout is needed.)
