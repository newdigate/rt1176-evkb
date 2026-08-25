# W21 handoff: where the M.2 Bluetooth programme actually stands

**Read this before touching Bluetooth.** W20 built the whole HCI transport and
it works; the card does not. Five days of hypotheses are recorded here so the
next session does not re-run any of them.

> ★ **The single most useful sentence in this file** (final, 2026-08-25):
> everything on the host side is finished and proven; the 3 Mbaud idea was
> built, gated and **run on silicon, and it is refuted** — the card transmits
> *nothing* after a fully successful firmware download, at any of four rates,
> with or without flow control. The **USB dongle** is the recommendation again.

---

> ## ★★ CURRENT STATE — 2026-08-25, read this before anything below
>
> This block replaces an earlier one that had accreted in layers and ended up
> contradicting itself. Everything below in the body still reads as it did when
> written; where it disagrees with this block, this block wins.
>
> **Since this handoff was written, five more hypotheses were tested on silicon
> and every one of them failed.** The conclusion is unchanged but far better
> supported, and the fault is now narrowly located.
>
> ### What was tested and refuted
>
> | # | hypothesis | verdict |
> |---|---|---|
> | 6 | **Wrong baud** — u-blox attach at 3 Mbaud (SIM §4.4.6), we only ever used 115200 | **refuted** — 3M/921600/460800/115200, `framing=0` and zero bytes at all four |
> | 5 | **Hardware flow control** | **refuted properly** — the first test drove CTS low across the PDn release, latching `CON[7]=0` (a Reserved config, SIM §2.4.5 Table 6); re-run with correct ordering, no change |
> | 7 | **Missing boot-sleep wake** — NXP call `wakeUpControllerFromBootSleep()` *before* the download: 10 ms LOW on `GPIO_DISP_B2_13`, mux returned to `LPUART2_RTS_B` | **refuted** — implemented; the card *reacts* (`start_inds` 2→3, the only behavioural change we ever produced) and the outcome is identical |
> | 3 | **Combo needs both images** | **superseded** — the combo *contains* both (verified via embedded build IDs), and separately: it does **not** bring Bluetooth up (below) |
> | — | **Downloads are "alternatives"** | **refuted** — that was the *wrong Wi-Fi image*; see below |
>
> ### Two findings that changed the picture
>
> **The combo image over SDIO does not bring up Bluetooth on this card.** Dumped
> in bytes, not inferred: with the combo loaded and Wi-Fi running (`card=1`), the
> BT UART emits a *fourth* `AB 01 72 00 47` — a fresh ROM start indication. The
> BT core is still in its bootloader. That contradicts NXP's
> `controller_wifi_nxp.c` and u-blox's own §4.4.3/§4.4.6 procedure, and it
> vindicates BT-1's pivot to the UART download.
>
> **NXP's real pairing is one image per bus, and we had it wrong.** With
> `CONFIG_BT_IND_DNLD` set, `wlan_bt_fw.h` swaps the combo for **both**
> `sdIW416_wlan.h` *and* `uartIW416_bt.h`. Measured: after a BT UART download,
> the **combo** over SDIO fails (`cmd-timeout, card=0`) but the **WLAN-only**
> image succeeds (`ok, card=1`). They are two halves of one mode.
> ★ **Pair `M2_BT_UART_DNLD=ON` with `sdIW416_wlan.bin`, never the combo.**
>
> ### Where the fault is
>
> With both radios correctly provisioned — Wi-Fi running, BT firmware accepted,
> ROM loader **exited** (no re-greet, unlike the combo path) — Bluetooth still
> answers nothing at any rate.
>
> **The failure is after the jump**: between accepting a CRC-validated image and
> running a working HCI controller. Not transport (proven bidirectional), not
> delivery (142 chunks, a real retransmit recovered), not arrival (the ROM stops
> asking, which it only does once it has what it wants — and the combo path shows
> us what *rejection* looks like: it keeps announcing).
>
> ★ `CONFIG_BT_IND_DNLD` was **already set** in our EdgeFast build — auto-selected
> by Kconfig, verified by reading the generated config. So NXP's own stack used
> the same path and the same image and failed the same way. That strengthens the
> exoneration rather than weakening it.
>
> ### The second-host experiment — attempted, and it did not work
>
> The card was moved to a **MIMXRT1060-EVKB** to separate "this module" from
> "this board". `m2_hci_probe` now builds for `rt1062` (BT UART is `Serial2` on
> both boards — LPUART3 there), so the instrument is identical.
>
> **It never greeted, after three hand reworks.** `R345` (PDn) and `R96` (the
> card→MCU RX leg) are both DNP from the factory and were bridged; `R343`/`R344`
> were also removed, **unnecessarily, on a wrong diagnosis of mine** — a pin-swing
> test that drove `GPIO1` while the teensy4 core hands every pad to `GPIO6`, which
> manufactured a confident "PIN STUCK" verdict out of a disconnected register.
>
> With that fixed, the continuity probe reports the RX line **driven high
> externally** (an idle UART — so the bridges conduct) and PDn **swinging** when
> read back at the pad. And the card is **silent for 3 s** after every reset.
>
> ★ **That is not evidence about the module** — the same physical card greets
> reliably on the 1170. The 1060 is simply not equivalent yet, in a way the
> netlist, the continuity probe and the swing test have all failed to surface.
> **It cannot serve as the independent platform it was brought in to be**, and
> further work there needs a scope on J8.22/J8.56, not more firmware.
>
> ### ✅ The vendor case is FILED — u-blox **CA-276115** (2026-08-25)
>
> Submitted through the u-blox portal. The write-up is
> `docs/m2-maya-w161-support-case.md`; nothing was attached, so if they ask for
> logs the file to send is
> `examples/networking/m2_hci_probe/transcript_hw_evkb.txt`.
> **Check for a reply before repeating any experiment** — question 3 (does the
> combo image bring BT up on this module?) and question 6 (a known-good
> reference capture) are the two whose answers would change what to do next.
>
> ### So §9 stands: the USB dongle
>
> Every host-side and configuration-level explanation is eliminated. Only
> **secure boot / signature rejection** survives, and it is untestable from the
> host. This is now a vendor support case, not a debugging problem — and it is a
> well-scoped one. Full account: `examples/networking/m2_hci_probe/transcript_hw_evkb.txt`.


## 1. What exists now (all merged, all green)

**`M2Radio/hci/` — a clean-room HCI stack**, MIT, 123 host checks under
ASan/UBSan:

| unit | what it does |
|---|---|
| `H4Parser` | H4 byte stream → packets; pure state machine |
| `Hci` | command queue honouring `Num_HCI_Command_Packets`, reply matching, timeouts, resync; **every exit named and counted** |
| `HciEvents` | Inquiry Result (field-major!) and Remote Name parsers |
| `BtFwLoader` | **NXP V3 UART firmware download**, decoded from the card's own bytes |
| `HciTransport` / `HciPump` | `Serial2` + a yield-attached `EventResponder` |

**`examples/networking/m2_hci_probe`** with two gates (sweep 119 → **121**):
* `run_qemu.sh` — card-absent fallback, times out BY NAME, then heartbeats.
* `[hci]` — a Python fake controller on LPUART2 via `-serial unix:…,server`
  (needs **no qemu2 change**: `serial_hd(1)` is LPUART2 and its model does
  chardev RX). Five phases: `full`, `drop-reset`, `garbage`, `starve`, and
  **`fwdnld`** — which plays the card's V3 bootloader and **verifies every byte
  the host serves**. Demonstrated RED four different ways.

**Core:** `addMemoryForRead/Write` on `HardwareSerialIMXRT`; a stale comment fixed.

---

## 2. What silicon said

**The V3 firmware download WORKS on the real card.**

    bt_fw_download=ok chip_id=0x7201 loader_ver=0 start_inds=2 chunks=142
                    sent=131856/131840 max_off=131840 retx=1 crc_err=0 card_err=0x0001

All 131,840 bytes delivered, in the card's own structure (16-byte header, then
the payload it describes, ending exactly at the last byte), and its **CRC-error
retransmit path exercised for real**. The protocol decode was verified three
ways: `0xAB` is NXP's V3 start-indication marker; the chipId `0x7201` is the
same `hw_version` we read independently **over SDIO**; and CRC-8 (poly 0x07,
init 0xFF, over the whole frame) gives exactly the `0x47` the card sent.

**And then the controller says nothing.** Ever. No HCI reply, no re-greet, and
`BT_WAKE_HOST` never asserts.

★ **ORDER IS LOAD-BEARING AND GETTING IT WRONG IS SILENT.** The BT core greets
ONCE per power-up. `Serial2` must be listening BEFORE the card is powered and
the download must run BEFORE any SDIO work. Getting this wrong reports
`no_start_indication` from a perfectly healthy card.

---

## 3. Hypotheses tested — DO NOT RE-RUN THESE

| # | hypothesis | verdict | evidence |
|---|---|---|---|
| 1 | UART-config / "baud change" block missing | **REFUTED** | injecting NXP's type-5 header makes it *worse*: card answers CRC_ERR, re-asks, then silence. Never requests the payload. |
| 2 | Wrong / u-blox-specific firmware image | **REFUTED** | u-blox ship NO firmware — their docs say MAYA-W1 firmware is NXP's, already in the i.MX BSP and MCUXpresso SDK. The image we have IS the vendor's. |
| 3 | Our SDIO download truncates the combo image | **REFUTED** | widened the idle poll to 15 s: `max_pause_ms=15000`, `sent` unchanged at 402,288/411,064. The card stops **deliberately**, with Wi-Fi running fine. |
| 4 | Secure-boot / signature rejection | **UNTESTED** | the only one standing, and indistinguishable from "silently ignored" from the host side. |
| 5 | Card needs hardware flow control | **REFUTED** | drove the card's CTS input low (it IS connected, direct to `U354.5`): no change. |
| 6 | Helper-file two-stage download | **REFUTED** | AN14310: `helper_xxx.bin` is required for 88W8997, **not** for IW416. |

### ★★ And the decisive one

**NXP's own EdgeFast `a2dp_source`, built for `WIFI_IW416_BOARD_MURATA_1XK_M2`,
fails identically on this board.** It prints `Bluetooth A2dp Source demo
start...` and then nothing for 150 s. The CPU is healthy — `DHCSR 0x01010001`
(the documented healthy-running value), `CFSR`/`HFSR` both zero, no fault ever
taken — alive and blocked, waiting for a controller that never answers.
Rebuilt with `enableRxRTS`/`enableTxCTS = 0` in case the missing rework blocked
*their* flow control: identical.

**So the fault is the card, module or board — NOT this repo's driver.**

---

## 4. Two corrections to earlier claims in this programme

Recorded because this tree has been bitten before by conclusions that outlived
their evidence:

* **"The combo image is WLAN + BT concatenated"** — WRONG, it was a coincidence
  (279,164 + 131,840 ≈ 411,064). Byte-checked: the combo neither starts with the
  WLAN image nor ends with the BT image — it is not a concatenation.
  ★ **"nor contains it at all", as this line first read, was itself wrong**:
  the combo carries BOTH build IDs (`w8978o-V0` WLAN and `w8978d-V0 … BT_UART`,
  both `16.92.21.p155.2`, timestamps identical to the standalone images). It
  contains both radios' firmware, LZMA-compressed rather than appended.
* **"EdgeFast is not armgcc-buildable"** — WRONG, I used the wrong example tree.
  See §6.

---

## 5. ★ THE RECOMMENDED ROUTE: Bluetooth over a USB dongle

**You already own most of this.** `USBHost_t36` (your own fork, **MIT**) has
`bluetooth.cpp` (2,606 lines) and `BluetoothConnection.cpp` (2,222 lines)
implementing:

* **HCI over USB** — the whole of BT-1's job;
* **link management** — inquiry, remote name, pairing, connection;
* **L2CAP** — connect/config/disconnect/command-reject signalling;
* **SDP** — service discovery.

i.e. **BT-1 and most of BT-2, already written and licence-clean.** Absent:
AVDTP, A2DP, SBC — BT-3/BT-4 remain ours, which is where the audio value is.

Why it is the best move:
* it **replaces the component that is actually broken** rather than working around it;
* USB Bluetooth is a **spec-defined class** (0xE0/0x01/0x01) — no vendor blob,
  no V3 download, none of the licence friction;
* ★ it **removes the coexistence problem entirely** — a separate radio, so BT
  audio no longer contends with Wi-Fi on the IW416's shared 1×1 antenna.

Cautions:
* ★ **Pick a self-contained dongle.** Many cheap ones (RTL8761B) upload a
  firmware patch at enumeration — which puts you straight back in the hole you
  are in. A CSR8510-class controller needs none, and BT 4.0 classic is
  sufficient (A2DP is BR/EDR).
* `bluetooth.cpp` looks **unverified on RT1176** — Teensy 4.x heritage. Budget
  bring-up.
* `USB_OTG2_ID` is header pin A5: an OTG adapter in the second port clamps A5
  low and kills header I²C.

**First move, an afternoon:** a `usb_descriptor_survey`-style probe — enumerate
the dongle, confirm class `E0/01/01`, and read `HCI_Read_Local_Version_Information`
off the wire. That would be the first answered HCI command of this programme.

### The other two routes

* **Buy a Murata 1XK (EAR00385)** — same IW416 silicon, but the module NXP
  validate. Turns "is it the card or the board?" into a controlled single-variable
  experiment, and NXP validate it for `a2dp_source`/`a2dp_sink` (the JODY-W5 is
  **not** in any A2DP example — do not buy that one).
* **A support case to NXP or u-blox** — the evidence in §2/§3 is strong and
  reproducible.

---

## 6. Recipes that cost hours to find

**Build NXP's EdgeFast reference (outside this repo — LA_OPT, never vendor it):**

    cd ~/Development/mcuxsdk-ws/mcuxsdk && export ARMGCC_DIR=/Applications/ARM_10
    west build -b evkbmimxrt1170 middleware/edgefast_open/examples/a2dp_source \
      --toolchain armgcc --config flexspi_nor_debug -d build_efo_a2dp -p always -- \
      -Dcore_id=cm7 \
      -DCONFIG_MCUX_COMPONENT_component.wifi_bt_module.IW61X=n \
      -DCONFIG_MCUX_COMPONENT_component.wifi_bt_module.board_murata_2el_m2=n \
      -DCONFIG_MCUX_COMPONENT_component.wifi_bt_module.IW416=y \
      -DCONFIG_MCUX_COMPONENT_component.wifi_bt_module.board_murata_1xk_m2=y

★ **`middleware/edgefast_open/examples/`, NOT `examples/edgefast_bluetooth_examples/`** —
only the former ships GCC linker scripts. ★ **`--config flexspi_nor_debug`, NOT
`debug`** — the example's `cm7/reconfig.cmake` registers its armgcc linker-script
swap for TARGETS `flexspi_nor_debug`/`release`; any other config silently uses the
generic device script, leaves the iterable sections undefined and overflows 255 KB
of text by 4×, which reads exactly like "GCC is unsupported" and is not.

**Licence audit from a worktree:** the variable is `LICENSE_AUDIT_EVKB`, not
`EVKB`. Without it the script audits `~/Development/rt1170/evkb` and reports
this tree's gates as `MISSING BUILD` while never looking at them.

**Sweep from a short path:** four gates open a UNIX socket and macOS caps
`sun_path` at 104 bytes. `/tmp/ev` points at a **different checkout** — make your
own symlink and check where it points before trusting a sweep taken through one.

**`display/pxp_draw_bench` needs a second build dir** — `cmake -B build-32
-DDRAW_BENCH_32=ON`. No generic build-everything loop makes it, and its absence
presents as "no UART capture", which reads like dead firmware.

---

## 7. STATE OF THE PINS — ✅ RESOLVED 2026-08-24, nothing to do here

Both sibling repos are **pushed** and both pins are **bumped**. This section is
kept because it was open when the handoff was written, and because the check
that closed it is the reusable part.

| repo | pin now | was | carries |
|---|---|---|---|
| `M2Radio` | **6ff9ade** | 300d32b | `hci/` — the whole BT-1 stack (13 commits) |
| `cores` | **36e480d** | fcd22b0 | `addMemoryForRead/Write` (2 commits) |

★ **How it was verified, which is the point.** Not by comparing SHAs — by
running the fresh-user path. `-DEVKB_FORCE_FETCH=ON` in a scratch build
directory cloned both repos from GitHub at the new pins and compiled clean; then
**both gates were run against that fetched-source ELF** (`build` symlinked to
it, restored afterwards) and both PASSED. A successful configure only proves the
subdirectory resolves; only a gate run proves the fetched code behaves. The
sweep no longer has a SKIP to hide anything in.

---

## 8. Hardware facts established (all in `docs/m2-evkb-revc3.md`)

* **A complete J54 pin map** — all 75 pins, card ⇄ board ⇄ RT1176 ball/pad,
  generated from the netlist + BOM + `pstchip.dat`, not transcribed.
* **A full DNP audit**: the Bluetooth data path is **complete**; `BT_UART_TXD`
  and `BT_UART_RTS` reach the level shifter with **no series resistor at all**.
  Nothing is missing.
* **NXP's rework list reconciled**: `R404`/`R1901` done; `R1902` + removing
  `R1816` still open (needed only for flow control, and they must be done
  together); **removing `R183`** is worth doing anyway — it puts a Macronix SPI
  flash's data output on the PDn net.
* ★ **J54 pins 38/44/46/48 are the IW416's own JTAG** (`TDO`/`TDI`/`TCK`/`TMS`),
  landing on test points `TP47`/`TP51`/`TP50`/`TP49`. An unused debug port into
  the chip.

---

## 9. If you want a second board

The **RT1060-EVKB has no M.2 socket** — it needs a Murata uSD-M.2 adapter
(microSD slot for SDIO, BT UART on flying leads). But: **`Serial4` in the
teensy4 core is already LPUART3**, which *is* that board's BT UART, and
`CORE_PIN16/17` are already `AD_B1_07`/`AD_B1_06`, exactly the pads it needs.
Bluetooth needs **no SDIO at all**, so a BT-only probe there is ~1–2 days.
Full Wi-Fi parity is 3–5 (SdioHost hardcodes the RT1176 USDHC base and pad names).
It also has **real CTS/RTS** (no Ethernet PHY collision) and a switchable SD rail.

---

## 10. Method notes that earned their place

* **Bracket every reading.** The B0 probe's quiet-window-before-command is what
  makes `reply=none` an honest negative rather than an absence of listening.
* **A gate never shown to fail is decoration.** Every gate here was demonstrated
  RED, and the `[fwdnld]` byte-check demonstration is the argument for the whole
  design: serving every chunk from offset 0 left acks, chunk count, byte count,
  retx, crc_err *and* the subsequent HCI all green — only the byte check caught it.
* **Read `DHCSR` before believing a lockup.** `0x01010001` is healthy-running.
* **The Linux `btnxpuart` driver implements this same V3 protocol — it is
  GPL-2.0, was NOT read, and must not be.** This tree is permissive-only.
  Everything here came from NXP documentation and their LA_OPT sources.
