# M2-MAYA-W161: Bluetooth firmware downloads successfully, controller never responds

**Support case write-up — prepared 2026-08-25.** Every figure below is a
measurement from this board, not an estimate. Full logs:
`examples/networking/m2_hci_probe/transcript_hw_evkb.txt`.

---

## 1. Summary

On an **M2-MAYA-W161-00C-00** in a **MIMXRT1170-EVKB (Rev C3)**:

* **Wi-Fi works completely** — SDIO enumeration, station, micro-AP, throughput.
* **The Bluetooth firmware downloads completely and correctly** over LPUART2
  using NXP's V3 protocol — all 131,840 bytes, with the card's own CRC-error
  retransmit path exercised and recovered.
* **The Bluetooth controller then never transmits another byte.** No response to
  `HCI_Reset` at any baud rate, ever.

**The decisive fact: NXP's own EdgeFast `shell` example, built from the
MCUXpresso SDK with its default configuration for this module, hangs at
`bt init` on the same board and prints nothing.** No software of ours is
involved in that reproduction.

We are asking u-blox to identify what the module requires between accepting a
firmware image and running an HCI controller — see §7.

---

## 2. Configuration

| Item | Value |
|---|---|
| Module | M2-MAYA-W161-00C-00 (NXP IW416) |
| Host board | MIMXRT1170-EVKB Rev C3, M.2 socket J54 |
| BT transport | LPUART2, 115200 8N1, **no hardware flow control** (see §6) |
| SDK | MCUXpresso **v26.06.00-LTS** (manifest `b01ab903`, mcuxsdk `a910e764`) |
| BT firmware | `uartIW416_bt.bin`, 131,840 bytes |
| Wi-Fi firmware | `sdIW416_wlan.bin`, 279,164 bytes |
| Combo firmware | `sduartIW416_wlan_bt.bin`, 411,064 bytes |
| Firmware version | `16.92.21.p155.2`, FP92, `w8978`, built **2026/03/12** |

Version strings read from inside the images:

```
Id: w8978o-V0, RF878X, FP92, 16.92.21.p155.2            2026/03/12 16:57:22
Id: w8978d-V0, RF878X, FP92, 16.92.21.p155.2, BT_UART   2026/03/12 17:02:37
```

---

## 3. The decisive result — NXP's own example fails identically

Built from `middleware/edgefast_open/examples/shell`,
`--toolchain armgcc --config flexspi_nor_debug`, with `IW416=y` and
`board_murata_1xk_m2=y`. Its generated configuration confirms:

```
CONFIG_BT_IND_DNLD=y            CONFIG_BT_ONLY_DNLD=y
CONFIG_LOG=y                    CONFIG_BT_HCI_DRIVER_LOG_LEVEL=4   (DBG)
```

Result on the board:

```
 Edgefast Bluetooth PAL shell demo start...
uart:~$ bt init
                        ← nothing, 45 s. Shell never returns; no echo afterwards.
```

* The example **boots and reaches its prompt**, so board, image and console are
  all sound.
* `bt init` **hangs indefinitely**, and prints **not one line** of the SDK's own
  HCI-driver logging despite DBG level.
* CPU state over SWD, console detached: **`DHCSR 0x01010001`** (running;
  `S_HALT` and `S_LOCKUP` clear), **`CFSR = 0`, `HFSR = 0`** — no fault was ever
  taken. The stack is **alive and blocked**, not crashed.

`a2dp_source`, built the same way, behaves identically: banner, then silence.

---

## 4. What demonstrably works

**The card is healthy and the transport is proven.**

* SDIO enumerates: `manfid=0x2DF cardid=0x9158 io_functions=1 rca=0x1 cccr_rev=0x3`.
* Wi-Fi firmware loads and runs (`fw_download=ok`, `card=1`); station, micro-AP
  and throughput all function.
* The BT ROM announces itself on LPUART2 at power-up with the V3 start
  indication, three times:

  ```
  bt_uart_preboot: n=16 hex=00 AB01720047 AB01720047 AB01720047
  ```

  → `chipId = 0x7201`, `loaderVer = 0`, CRC-8 valid (poly 0x07, init 0xFF).
  `0x7201` independently matches the `hw_version` read over SDIO.

* **The full BT firmware download succeeds:**

  ```
  bt_fw_download=ok chip_id=0x7201 loader_ver=0 start_inds=3 chunks=142
                 sent=131856/131840 max_off=131840 retx=1 crc_err=0
  ```

  142 chunks; the card requested every one (16-byte headers, 2048-byte
  payloads, including its out-of-order re-requests at the tail); **one CRC
  error was reported by the card and recovered by retransmission**, so the
  card's own integrity checking is live and working.

---

## 5. The failure, and the evidence that the image is *accepted*

After the download completes, the controller transmits nothing:

```
bt_post_dnld[0..3]: n=0      (four 500 ms windows, raw byte capture)
bt_raw_reset[0..2]: n=0      (raw 01 03 0C 00 at 115200, three attempts)
bt_baud_try=3000000  st=no_response
bt_baud_try=921600   st=no_response
bt_baud_try=460800   st=no_response
bt_baud_try=115200   st=no_response
bt_baud=none tried=4         framing=0 throughout
```

**`framing=0` with zero bytes at four rates is important**: a controller
transmitting at an unmatched baud rate would produce framing errors. Nothing is
being transmitted at all.

### The key diagnostic — the ROM stops asking

The **combo** image over SDIO gives us a reference for what *rejection* looks
like on this card. With the combo loaded and Wi-Fi running (`card=1`), the BT
UART emits a **fourth** start indication:

```
bt_uart_postsdio: n=5 hex=AB01720047     ← BT core still in ROM, still asking
```

After our **UART** download, it does the opposite — it goes silent and never
re-announces (`n=0` at both capture points).

**So the image is accepted and the bootloader is exited.** The failure lies
between accepting a CRC-validated image and running a working HCI controller.

*(Incidentally: this means the combo image over SDIO does **not** bring
Bluetooth up on this module, contrary to SIM UBX-21010495 R09 §4.4.3/§4.4.6.
Question 3 below.)*

---

## 6. What we have already eliminated

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| 1 | UART-config ("CMD5") block needed | **Refuted** | Card answers `CRC_ERR (0x0001)`, re-requests the header, gets identical bytes, never requests the payload. `loader_ver=0` throughout. |
| 2 | Wrong firmware image | **Unlikely** | Image self-identifies as `BT_UART`; first 16 bytes are a valid V3 header (`type=1`, load `0x000A2010`, len 2048, CRC-32 `0x83DFA3EA`, verified). Both SDKs on hand ship byte-identical files. |
| 3 | Combo image needed | **Superseded** | Combo contains both build IDs; separately, it leaves BT in ROM (§5). |
| 4 | Host download truncating | **Refuted** | Widening the idle poll to 15 s left `sent` unchanged — the card stops requesting deliberately. |
| 5 | Hardware flow control required | **Refuted** | Card's CTS input driven asserted (after reset, so `CON[7]` samples as 1) — no change at any rate. |
| 6 | Wrong HCI baud rate | **Refuted** | 3 M / 921600 / 460800 / 115200 all tried, `framing=0`, zero bytes. |
| 7 | Missing boot-sleep wake | **Refuted** | Implemented NXP's `wakeUpControllerFromBootSleep()` (10 ms low on `GPIO_DISP_B2_13`, mux returned to `LPUART2_RTS_B`). The card **reacts** — it greets an extra time, `start_inds` 2→3 — but the outcome is unchanged. |

**Only signature / secure-boot rejection remains**, and it is indistinguishable
from success from the host side.

### Board rework — stated for completeness

The MIMXRT1170-EVKB requires rework for M.2 Bluetooth. NXP's guide lists five
changes; we have fitted **`R404`** (PDn) and **`R1901`** (module→MCU RXD), both
verified working. Not done: fit `R1902`, remove `R1816`, remove `R183`.

**These cannot cause this failure.** `R1902`/`R1816` concern the *host reading
the card's RTS output* — they affect whether the host can be throttled, and
cannot prevent the card from transmitting. The direction that would gate the
card's transmission is its **CTS input**, which is populated, and which we have
driven asserted with no change (row 5 above).

---

## 7. Questions for u-blox

1. **Is `uartIW416_bt.bin` version `16.92.21.p155.2` from MCUXpresso SDK
   v26.06.00-LTS the correct and approved Bluetooth image for
   M2-MAYA-W161-00C-00?** Does the module require a u-blox-specific or
   differently-signed build? The SDK contains no MAYA module profile — only
   Murata (`board_murata_1xk_m2`), which we selected.

2. **Does the MAYA-W161 enforce image authentication on the UART-downloaded BT
   firmware, and how is a rejection signalled to the host?** We observe the ROM
   accept every block, stop requesting, and then emit nothing — with no error
   frame and no re-announcement.

3. **SIM UBX-21010495 R09 §4.4.3/§4.4.6 describes loading the combo image over
   SDIO and then `hciattach … any 3000000 flow`.** On this module the BT core
   re-announces itself from ROM *after* the combo download completes, i.e. the
   combo image does not bring Bluetooth up. Is that expected for
   M2-MAYA-W161-00C-00?

4. **Is any step required after the V3 download before HCI responds** — a
   vendor command, a GPIO transition, a minimum delay — beyond
   `wakeUpControllerFromBootSleep()`, which we already perform?

5. **What baud rate should the controller be at immediately after a UART
   firmware download**, before any `0xFC09` baud change?

6. **Can you supply a known-good reference capture** (e.g. from EVK-MAYA-W1)
   showing the expected bytes on the HCI UART in the seconds following a
   successful download? That would let us distinguish "this module" from "this
   board" immediately.

---

## 8. Reproduction

Fastest path, using only NXP-supplied software:

```
west build -b evkbmimxrt1170 middleware/edgefast_open/examples/shell \
  --toolchain armgcc --config flexspi_nor_debug -- -Dcore_id=cm7 \
  -DCONFIG_MCUX_COMPONENT_component.wifi_bt_module.IW416=y \
  -DCONFIG_MCUX_COMPONENT_component.wifi_bt_module.board_murata_1xk_m2=y
```

Flash, reset, then at the prompt type `bt init`. It hangs, with no output.
