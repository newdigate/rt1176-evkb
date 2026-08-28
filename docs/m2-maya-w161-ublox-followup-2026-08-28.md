# u-blox CA-276115 follow-up — 2026-08-28 (ready to send)

Decisive new evidence: a **module-substitution test** that isolates the fault to
the MAYA-W161. Evidence: `examples/networking/m2_hci_probe/transcript_hw_evkb.txt`.

Paste everything below the line into the case as an update.

---

Update on CA-276115 — we now have a decisive result: **the fault is specific to
the MAYA-W161 module.**

We performed a straight substitution test. Keeping *everything* else identical —
same MIMXRT1170-EVKB, same board rework, same external 5 V supply, same
MCUXpresso SDK v26.06.00-LTS, the same stock NXP `uartIW416_bt.bin`
(16.92.21.p155.2), and the *same compiled EdgeFast `shell` binary* — we replaced
the M2-MAYA-W161 in the M.2 slot with a genuine **Embedded Artists Murata Type
1XK M.2 (EAR00385)**, which is the same NXP IW416 silicon and the module NXP's
`board_murata_1xk_m2` build profile targets.

Result at the shell, unchanged binary:

```
@bt> bt.init
[FW Download] Start to download firmware from 0x301198fc
download starts(131840)
download success!
[FW Download]BLE FW is downloaded
Bluetooth initialized
Settings Loaded
```

**With the Murata 1XK the Bluetooth controller runs** — `Bluetooth initialized`
prints only after the controller answers HCI (HCI_Reset, Read_Local_Version,
Read_BD_ADDR, …). With the **MAYA-W161**, the *same* image at the *same* point
downloads identically (`download success!`) and then the controller is
**silent** — `bt.init` never returns, no HCI reply at any baud rate.

Only the module changed, and the fault moved with it. This rules out the board,
the rework, the power supply, the SDK, the firmware image and NXP's Bluetooth
stack — all of which are proven correct by the Murata module running to a live
controller on the identical setup.

We have also ruled out a defective MAYA-W161 unit: on that same card the **Wi-Fi
works fully** (SDIO enumeration, station, micro-AP, throughput), and its **BT
UART carries the entire 131,840-byte firmware download in both directions** with
the ROM start-indication intact. The card, its power, its level shifters and its
BT UART are all electrically sound. The *only* thing that does not happen is the
controller starting after a fully successful, CRC-validated download — a
firmware/authentication behaviour, not a hardware one.

The most consistent explanation is that the **MAYA-W161 enforces firmware
authentication that the stock NXP image does not satisfy** — i.e. the part
expects a u-blox-signed / u-blox-provisioned Bluetooth image, and silently does
not execute the generic NXP image after accepting the download.

**Our questions are now narrow:**

1. Does the M2-MAYA-W161-00C-00 require a **u-blox-specific or u-blox-signed**
   Bluetooth firmware image (as opposed to the stock NXP `uartIW416_bt.bin`)?
2. If so, **where do we obtain the correct image**, and is there a
   u-blox-documented download/`hciattach` procedure for it?
3. Is the MAYA-W161 **OTP-fused for a u-blox secure-boot key** such that the
   generic NXP image is expected to be accepted-but-not-run? If so, is that the
   documented behaviour we are seeing?

We can share the full serial transcript (both modules, side by side) and the SWD
register evidence on request.

Thank you,
[your name]
