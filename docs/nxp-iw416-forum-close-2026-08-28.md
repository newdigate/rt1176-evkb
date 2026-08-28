# NXP forum closing note — 2026-08-28 (ready to paste)

Thread: <https://community.nxp.com/t5/i-MX-RT-Crossover-MCUs/BT-firmware-accepted-over-UART-but-controller-never-runs-and/m-p/2408524#M37090>

Paste everything below the line into the thread.

---

Resolution / closing update — thank you for the help, and here is the answer for
anyone who finds this thread later.

After completing the rework and moving to external power (per Daniel's three
preconditions), we ran a **module-substitution test**. Holding *everything* else
identical — same MIMXRT1170-EVKB, same rework, same external 5 V, same MCUXpresso
SDK v26.06.00-LTS, the same stock `uartIW416_bt.bin`, and the *same compiled
EdgeFast `shell` binary* — we swapped the u-blox M2-MAYA-W161 for a genuine
**Embedded Artists Murata Type 1XK M.2 (EAR00385)** (the IW416 module the
`board_murata_1xk_m2` profile targets).

With the Murata 1XK, `bt.init` runs to completion:

```
download success!
[FW Download]BLE FW is downloaded
Bluetooth initialized
Settings Loaded
```

The controller comes up cleanly. With the u-blox MAYA-W161, the *same* binary
downloads identically (`download success!`) and then the controller is silent.

**So this was not an NXP problem.** The board, the SDK, the stock
`uartIW416_bt.bin`, and the EdgeFast stack are all correct — proven by the Murata
IW416 module running to a live controller on the identical setup. The fault is
specific to the u-blox MAYA-W161 (its firmware provisioning / authentication),
and we are taking that to u-blox.

Two useful side notes for others on this board:

* **The 3 Mbaud download corruption we reported earlier was the missing
  flow-control rework.** After fitting `R1902` and removing `R1816` (NXP's
  five-item EdgeFast rework), the stock loader's 115200 → 3,000,000-baud switch
  completes cleanly and the download succeeds. Before that rework it corrupted at
  the switch and looped; the rework fixes it.
* On the MIMXRT1170-EVKB the loader must stay well-fed at 3 Mbaud, which needs
  that flow-control rework in place; at 115200 it is unconditional.

Thanks again for pointing us at the rework and the external-supply requirement —
both mattered, and together they got a genuine Murata module all the way up.

Best regards,
[your name]
