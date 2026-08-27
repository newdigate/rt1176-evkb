# NXP IW416: BT firmware accepted over UART but controller never runs; and EdgeFast download corrupts at the 3 Mbaud switch on the MIMXRT1170-EVKB

**Support request — prepared 2026-08-26.** Companion to u-blox case
**CA-276115** (same hardware, module-vendor questions). This ticket asks only
what NXP uniquely owns: the **IW416 ROM's firmware-authentication behaviour**
and an **EdgeFast download bug on NXP's own reference board**. Every figure is a
measurement; full logs in `examples/networking/m2_hci_probe/transcript_hw_evkb.txt`.

> ## ✅ POSTED to the NXP community forum — 2026-08-26
>
> A formal NXP support request needs a registered company; NXP directed this to
> their public i.MX RT community forum instead. Thread:
> <https://community.nxp.com/t5/i-MX-RT-Crossover-MCUs/BT-firmware-accepted-over-UART-but-controller-never-runs-and/m-p/2408524#M37090>
> Watch it for replies before re-running any experiment.

> ### ✅ ALL THREE PRECONDITIONS MET — 2026-08-27
>
> 1. **Full rework DONE.** `R1902` fitted (0 Ω), `R1816` removed, `R183` removed,
>    on top of `R404` + `R1901`. Our own probe re-verified the readable BT link
>    survived the removals — clean 115200 download, `sent=131856/131840`,
>    `framing=0`, byte-for-byte the pre-rework result.
> 2. **External 5 V DONE.** `J38` → 1–2, 5 V on the `J43` barrel jack, `SW5` on.
> 3. **Unmodified-shell log CAPTURED.** Built `examples/edgefast_bluetooth_examples/shell`
>    stock — the only change is the sanctioned module selection in the board
>    `prj.conf` (two Kconfig `choice` lines: `IW61X`→`IW416`,
>    `board_murata_2el_m2`→`board_murata_1xk_m2`); `flexspi_nor_debug`, armgcc
>    10.2.1, `CONFIG_BT_SIGNING=y` stock. **See §3a.**
>
> **The result changes the report.** With the flow-control rework in place, at
> `@bt> bt.init` the firmware now **downloads successfully** (`download success!`,
> 131,840 B in ~1.45 s — the high-speed 3 Mbaud phase) and the controller is then
> **silent**: no `Bluetooth initialized`, `bt.init` never returns, CM7 alive and
> blocked (DHCSR `0x01010001`, CFSR/HFSR = 0, no fault). So:
>
> * **Failure B (3 Mbaud download corruption) is RESOLVED by the rework** — the
>   very phase that corrupted before now completes on NXP's own stack (§4).
> * **Failure A is now cleanly ISOLATED** — a complete, uncorrupted, CRC-checked
>   download followed by a dead controller, with zero of our code in the path.
>   "The download was corrupt" is no longer available as an explanation.
>
> Reply drafted for the forum in `docs/nxp-iw416-forum-reply-2026-08-27.md`.

---

## 1. Summary — two DISTINCT failures, please keep them separate

On a **u-blox M2-MAYA-W161** (NXP **IW416**) in a **MIMXRT1170-EVKB Rev C3**,
BT HCI over LPUART2:

**Failure A — the core question.** With a clean 115200-baud UART download, the
BT firmware **downloads completely and is accepted** (the ROM stops requesting
data and does not re-announce), and the controller then **never transmits** —
no HCI response at any baud rate.

**Failure B — an EdgeFast/board bug, NOW RESOLVED by the rework.** Before the
flow-control rework, NXP's stock EdgeFast download switched to **3,000,000 baud
mid-download** and the link **corrupted at the switch** on this board; the
download never completed. With `R1902` fitted and `R1816` removed (2026-08-27),
the 3 Mbaud download **completes** — `download success!` on the stock shell
(§3a). Root cause confirmed: the missing CTS flow control.

> These have different root points, and completing the rework proves it: fixing
> B (the 3 Mbaud corruption) did **not** address A. After a now-*successful*
> download, the controller is still silent (§3a). **Failure A is the remaining
> question**, and it is now isolated from any download-integrity confound.

Wi-Fi (SDIO) on the same card works fully — enumeration, station, micro-AP,
throughput — so the card, its power, and its level shifters are healthy.

---

## 2. Configuration

| Item | Value |
|---|---|
| Chipset / module | NXP **IW416** in u-blox M2-MAYA-W161-00C-00 |
| Host board | MIMXRT1170-EVKB Rev C3, M.2 J54, LPUART2 |
| SDK | MCUXpresso **v26.06.00-LTS** |
| BT firmware | `uartIW416_bt.bin` **16.92.21.p155.2**, FP92, `w8978`, 131,840 B |
| Also tested | **16.92.21.p142.5** (FP91, from `github.com/NXP/wifi_nb_fw@a91d9d6`, Jan 2025) — same result |
| Module select | `WIFI_IW416_BOARD_MURATA_1XK_M2` (the SDK has no MAYA profile) |
| Board rework | `R404` (PDn) + `R1901` (module→MCU RXD) fitted & verified; flow-control pair `R1816`/`R1902` **not** done — see §5 |

---

## 3. Failure A — the image is accepted, then the controller is silent

### The download completes and is accepted

Our own clean-room V3 loader (protocol only, at 115200), instrumented:

```
bt_fw_download=ok chip_id=0x7201 loader_ver=0 start_inds=2 chunks=142
               sent=131856/131840 max_off=131840 retx=1 crc_err=0
```

All 131,840 bytes; the card requested every chunk (16-byte headers, 2048-byte
payloads, including out-of-order re-requests at the tail); **one CRC error was
reported by the card and recovered by retransmission** — so the ROM's own
integrity checking is live.

### Then nothing — and the ROM behaves as if the image was ACCEPTED, not rejected

```
bt_post_dnld[0..3]: n=0     (four 500 ms raw-capture windows after download)
bt_raw_reset[0..2]: n=0     (raw HCI_Reset 01 03 0C 00 at 115200, x3)
HCI_Reset at 3000000 / 921600 / 460800 / 115200 : no response, framing=0 at all
```

The **key discriminator**: after the UART download the ROM **stops requesting
and does not re-announce**. For contrast, when the combo image is downloaded
over **SDIO**, the BT core **does re-announce** on the UART (`AB 01 72 00 47`
reappears) — i.e. that is what "still in bootloader / not accepted" looks like
on this part. So the UART download is **accepted and the loader exited**, and
the failure is *between accepting a CRC-valid image and running a controller*.

### Firmware version is not the variable

Two official builds — **FP92 p155.2** (2026-03) and **FP91 p142.5** (2025-01,
different internal structure, load `0x00080000` vs `0x000A2010`, 178 vs 142
chunks) — both download completely, both are accepted, both leave the controller
silent. So this is not a stale/wrong *version*.

### Questions for NXP (Failure A)

1. **Does the IW416 ROM authenticate the UART-downloaded BT firmware against
   OTP-fused keys**, and if so, **how is a rejection signalled to the host?**
   We observe: every block accepted, ROM stops requesting, no error frame, no
   re-announcement, then silence. Is that byte-pattern the documented
   secure-boot / signature-reject behaviour, or does an authentic image behave
   this way for another reason?
2. **Is stock `uartIW416_bt.bin` (16.92.21.p155.2) built for a generic /
   un-fused IW416**, or does it require a matching OTP/secure-boot provisioning
   on the part? (If the part is fused for a specific OEM key, is the stock image
   expected to be rejected — which would point us back to u-blox, CA-276115.)
3. **What is the expected controller behaviour in the first seconds after a
   successful UART download** at 115200 — should it answer `HCI_Reset`
   immediately, or is a vendor command / delay required first?

---

## 3a. Failure A reproduced on NXP's UNMODIFIED stack (2026-08-27, full rework + external 5 V)

After completing the flow-control rework (`R1902` fitted, `R1816` + `R183`
removed) and powering the board from the `J43` barrel jack at 5 V, we built the
stock `examples/edgefast_bluetooth_examples/shell` — unmodified but for the
sanctioned module selection (`IW416` / `board_murata_1xk_m2`) — and typed
`bt.init`:

```
@bt> bt.init
[FW Download] Start to download firmware from 0x301198fc: 6812
download starts(131840)
...................................................................... (141 dots)
download success!
[FW Download]BLE FW is downloaded: 8265
          ← then nothing, for the remaining 92 s. bt.init never returns.
```

The download moved all 131,840 bytes in **1,453 ms** (6812 → 8265) — ~90 KB/s,
the high-speed (3 Mbaud) phase, not 115200. It **completed**, and NXP's loader
printed `download success!`. Then the controller went silent.

CPU state during the hang (SWD, console detached, `tools/rt1170-swdprobe.py --health`):

```
DHCSR 0x01010001   running (S_HALT=0, S_LOCKUP=0, S_RETIRE_ST=1)
CFSR  0x00000000   no configurable fault
HFSR  0x00000000   no hard fault ever taken
```

The host MCU is **alive and blocked**, not crashed — exactly what our own probe
showed at 115200, now on NXP's own unmodified code after a *successful* download.
This is the seam between this ticket and u-blox CA-276115.

---

## 4. Failure B — EdgeFast download corrupts at the 3 Mbaud switch (MIMXRT1170-EVKB)

> **✅ RESOLVED 2026-08-27 by the flow-control rework.** With `R1902` fitted and
> `R1816` removed (real CTS back-pressure), the stock EdgeFast shell now
> **completes** the 3 Mbaud download (`download success!`, §3a) instead of
> corrupting. The account below is the pre-rework record and the root-cause
> analysis that predicted the fix — it answers Question 4: the full flow-control
> rework is what makes 3 Mbaud usable on this board.

Reproducible with **NXP software only**, on **NXP's own reference board**.
Built `examples/edgefast_bluetooth_examples/shell` for the 1170 with
`DEBUG_PRINT` enabled so NXP's `fw_loader_uart.c` narrates. From its trace:

1. 115200 header requests are clean and CRC-valid (`REQ=0xA7 Len=10 Off=0 Err=0`);
2. a type-5 UART-config block is sent and **ACKed** (so the type-5 command is
   accepted in-context on this board/firmware);
3. `change baud-rate req to 3000000`, `changeBaudrate() ret 0` — the switch
   **reports success**;
4. immediately after: `Invalid Header 0x00` / `0x71`,
   `REQ = 0xA7, Len = 10a7, Off = 5400, Err = 400, CRC = 0`, `CRC Mismatched`,
   and `file download: 0: 131840` — stuck at offset 0. The offsets are
   byte-concatenations (`5400` = two bytes run together): **framing loss**, not
   overflow.

So the card switched to 3 Mbaud and is transmitting; the RT1176 LPUART2 at
3,000,000 baud on this board — DMA-fed, **no usable hardware flow control** —
cannot recover the frames. With stock flow-control settings the first
post-switch header times out, the loader falls back to 115200, retries, and
**loops indefinitely** (>20 min observed).

Root cause on the board: **LPUART2's RTS/CTS pads are the gigabit PHY's
reset/interrupt lines** (`R1866` → `ETHPHY_RST_B`, `R1816` → `RGMII1_PHY_INTB`),
so there is no clean host-RX back-pressure path even after NXP's documented
five-item rework — `R1866` is not in that rework. At 3 Mbaud without flow
control the host cannot keep up.

### Questions for NXP (Failure B)

4. On the **MIMXRT1170-EVKB**, is the 3 Mbaud EdgeFast BT firmware download a
   **known limitation without the full flow-control rework** (remove `R1816`,
   fit `R1902`) — and even with it, given `R1866` keeps the host RTS on the PHY
   reset, is 3 Mbaud actually supported on this board, or should the
   `fw_download_secondary_speed` be left at 115200?
5. Is the **infinite retry loop** on a failed secondary-baud switch
   (`fw_loader_uart.c`) intended? A hard failure after N retries would be far
   easier to diagnose than an endless progress-dot stream.

### Minor, low priority (same tree)

6. In `edgefast_bluetooth`'s shell, `SHELL_CMD_REGISTER(bt, ...)` registers the
   `bt` command with a **0-parameter range**, so `fsl_shell.c` rejects every
   subcommand (`bt init` → "Incorrect command parameter(s)"); the working
   invocation is the dotted `bt.init`. Likely an `SHELL_ADVANCE` / range-init
   mismatch worth a look.

---

## 5. What we have already eliminated (so these need not be suggested)

All on silicon, MIMXRT1170-EVKB:

| Hypothesis | Verdict |
|---|---|
| type-5 UART-config block missing | our standalone injection is rejected (`CRC_ERR 0x0001`); but NXP's in-flow type-5 IS accepted (§4) — so this is not the blocker for Failure A |
| wrong firmware **version** | refuted — FP91 and FP92 both accepted, both silent (§3) |
| combo-over-SDIO needed | it does **not** bring BT up on this module — the BT core re-announces from ROM after the SDIO combo download completes |
| host download truncating | refuted — 15 s idle poll leaves `sent` unchanged |
| HCI baud rate | refuted — 4 rates, `framing=0`, zero bytes |
| `wakeUpControllerFromBootSleep()` GPIO pulse | implemented; the card **reacts** (extra greeting) but outcome unchanged |

CPU state during the silence (SWD, console detached): `DHCSR 0x01010001`
(running; `S_HALT`/`S_LOCKUP` clear), `CFSR`/`HFSR` = 0 — the host MCU is alive
and blocked in `controller_init`, not crashed.

Only **signature / secure-boot rejection** remains for Failure A, which is
exactly Q1/Q2 — and it is the seam between this ticket and u-blox CA-276115.

---

## 6. Reproduction (Failure B, NXP software only)

```
west build -b evkbmimxrt1170 examples/edgefast_bluetooth_examples/shell \
  --toolchain armgcc --config flexspi_nor_debug -- -Dcore_id=cm7 \
  -DCONFIG_MCUX_COMPONENT_component.wifi_bt_module.IW416=y \
  -DCONFIG_MCUX_COMPONENT_component.wifi_bt_module.board_murata_1xk_m2=y
```

Flash, reset, at the `@bt>` prompt type `bt.init`. The download prints progress
dots and never completes; enabling `DEBUG_PRINT` in `fw_loader_uart.c` shows the
corruption at the 3 Mbaud switch. (`edgefast_open`'s newer shell for the 1170
accepts space-separated `bt init` and hangs the same way.)

For Failure A: full instrumented download + HCI evidence at 115200 is in
`examples/networking/m2_hci_probe/transcript_hw_evkb.txt`.
