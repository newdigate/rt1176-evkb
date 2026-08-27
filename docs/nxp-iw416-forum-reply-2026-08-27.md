# NXP forum reply — 2026-08-27 (ready to paste)

Thread: <https://community.nxp.com/t5/i-MX-RT-Crossover-MCUs/BT-firmware-accepted-over-UART-but-controller-never-runs-and/m-p/2408524#M37090>

Companion u-blox case: **CA-276115**. Evidence:
`examples/networking/m2_hci_probe/transcript_hw_evkb.txt`.

Paste everything below the line into the forum reply.

---

Hi Daniel — thank you, we've completed all three items you asked for. Summary
first, then the log.

**1. Rework — complete.** All five items of the EdgeFast BT PAL rework for the
MIMXRT1170-EVKB are done: R404 and R1901 (fitted earlier), plus **R1902 fitted**
and **R1816 + R183 removed**. We confirmed the two removals did not harm the
readable BT link — our own 115200-baud loader still downloads the full image
cleanly (131,840 bytes, framing errors = 0, byte-identical to before the removals).

**2. External power — done.** J38 → 1–2, 5 V into the J43 barrel jack, SW5 on.

**3. Unmodified-shell log — captured.** We built
`examples/edgefast_bluetooth_examples/shell` from MCUXpresso SDK v26.06.00-LTS,
`--config flexspi_nor_debug -Dcore_id=cm7`, armgcc. The only change from stock is
the module selection for our card, in the board `prj.conf`:

```
CONFIG_MCUX_COMPONENT_component.wifi_bt_module.IW61X=y               → IW416=y
CONFIG_MCUX_COMPONENT_component.wifi_bt_module.board_murata_2el_m2=y → board_murata_1xk_m2=y
```

(These are the two Kconfig `choice` members that select the module. Our card is a
u-blox MAYA-W161 — an IW416 — so we selected the 1XK/IW416 profile you named.
`CONFIG_BT_SIGNING=y` and everything else is stock.)

Result at the shell prompt:

```
@bt> bt.init
[FW Download] Start to download firmware from 0x301198fc: 6812
download starts(131840)
......................................................................  (141 dots)
download success!
[FW Download]BLE FW is downloaded: 8265
          ← then nothing, for the remaining 90+ seconds. bt.init never returns.
```

**Two things changed since our earlier report, and both matter:**

**A — the 3 Mbaud download now COMPLETES.** Before the flow-control rework the
stock loader corrupted at the 115200 → 3,000,000-baud switch and looped
indefinitely. With R1902 fitted and R1816 removed (real CTS back-pressure), it
now moves all 131,840 bytes in ~1.45 s (timestamps 6812 → 8265 ms — the
high-speed rate) and prints `download success!`. So the rework fixed the
download-side problem, exactly as expected — thank you for insisting on it.

**B — the controller is still silent after a SUCCESSFUL download.** This is the
core question. After `download success!` / `BLE FW is downloaded`, the stack
receives nothing from the controller — no `Bluetooth initialized`, no error — and
`bt.init` hangs. The host MCU is alive and blocked, not crashed; with the console
detached we read over SWD:

```
DHCSR = 0x01010001   (running: S_HALT=0, S_LOCKUP=0, S_RETIRE_ST=1)
CFSR  = 0x00000000   (no configurable fault)
HFSR  = 0x00000000   (no hard fault ever taken)
```

So the CM7 is waiting in the stack for an HCI reply that never comes.

This is now a very clean data point: on NXP's **own unmodified EdgeFast stack**,
with the full rework and external power, the card **accepts a complete,
CRC-checked firmware image and then runs no HCI controller**. The earlier "the
3 Mbaud download is corrupt" explanation no longer applies — the download
demonstrably succeeds.

**Our questions (unchanged, now isolated from any download-integrity variable):**

1. Does the IW416 ROM authenticate the UART-downloaded BT firmware (e.g. against
   OTP-fused keys), and if so, how is a rejection signalled to the host? We see
   every block accepted, `download success!`, then silence — no error frame and
   no re-announcement.
2. Is the stock `uartIW416_bt.bin` (16.92.21.p155.2) built for a generic /
   un-fused IW416, or does a part fused for a specific OEM key require a matching
   signed image? If so, that points us back to u-blox (companion case CA-276115).
3. In the first seconds after a successful UART download, should the controller
   answer `HCI_Reset` immediately at the post-download rate, or is a vendor
   command / delay required first?

We're happy to share the full serial capture and the complete SWD register block,
and we can enable `DEBUG_PRINT` in `fw_loader_uart.c` for a narrated download
trace if that would help.

Thanks again,
[your name]
