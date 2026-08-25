# W20 prompt: explore and brainstorm the M.2 card's **Bluetooth** capabilities

**Exploration brief. Brainstorm before planning, plan before coding** — use
`superpowers:brainstorming` first.

The module is the u-blox **M2-MAYA-W161** (NXP IW416): **WLAN over SDIO,
Bluetooth/LE 5.2 over UART**. The Wi-Fi side is finished and gated (sweep 119).
Bluetooth has never been driven — but, and this is the thing to know before you
plan anything, **the hard part is already done.**

## ★ The hardware precondition is ALREADY MET — do not re-do it

`docs/m2-evkb-revc3.md` says the BT UART is transmit-only as built, because
`R1901` (module→MCU RXD) is DNP. **That was fixed on 2026-08-18.** Both hand
bridges are fitted and both were verified, not assumed
(`examples/networking/m2_sdio_probe/transcript_hw_evkb.txt`):

| bridge | what it gave us | evidence |
|---|---|---|
| **`R1901`** (0402 0 Ω, module→MCU RXD) | the card's BT UART TX became *visible* | `r1901_bridge: pullup_reads=0 pulldown_reads=0 -> DRIVEN LOW` — actively driven, so the bridge conducts |
| **`R404`** (GPIO_AD_31 → PDn chain) | the radio can be power-cycled, and the card boots at all | `*** SUCCESS *** — R404 bridged, the card ENUMERATES` |

And after the PDn fix, the BT line came alive:

```
bt_wake(pin20): pullup_reads=1 pulldown_reads=1 -> HELD HIGH externally
after_pdn_cycle:  rx_any_high=1  rx_edges=26
```

**26 edges on the card's UART TX.** Before R404 every card→host signal was
stuck at logic zero (pin 20 low, pin 22 low, no CMD5 response) — the signature
of a module held in power-down. It is not held any more.

## What is actually open

**No HCI command has ever been answered.** The only `hci_probe` reading in the
transcript is

```
hci_probe: before[high=0 edges=0] after[high=0 edges=0] serial2_rx: total=1 last=0x0
```

and it **predates R404** — i.e. it was taken while the module was powered down
and everything was low. It says nothing about today's card.

★ **First move, and it is nearly free: re-run that probe now.** The code is
already in `m2_sdio_probe.cpp` (`HCI_RESET[] = {0x01,0x03,0x0C,0x00}`,
`m2RxContinuity()`, `m2WatchRxLine()`, `m2BtWakeSample()`), Serial2 is already
brought up at 115200, and the card now boots. This is the Bluetooth analogue of
W17's Phase 0: one cheap question whose answer reshapes everything after it.

★ **While you are there, fix a stale comment.** `m2_sdio_probe.cpp` still says
*"R1901 is DNP so the card CANNOT reach us"* next to the Serial2 drain. That was
true when written and is false now; the next reader will trust it.

## Constraints that have not changed

* **No hardware flow control on LPUART2, ever.** `R1866` ties BT_UART_**RTS** to
  the gigabit PHY's **reset** line (asserting it holds the RTL8211FDI-CG in
  reset) and `R1816` puts **CTS** on the PHY interrupt net. On this board RTS is
  not a UART signal. Enabling flow control breaks `networking/enet_test` and
  friends. `R1902` is deliberately left DNP for this reason.
* **Header collisions.** `R2`/`R3`/`R8` are fitted, so Arduino **D0–D2 collide
  with Bluetooth**; and `GPIO_AD_31` (the R404/PDn line) is also **D12/MISO**,
  so power-cycling the radio disturbs header SPI.
* **You cannot power the module down through software alone** beyond PDn —
  `WL_3V3` has no switch; physical removal is the only full isolation.
* **The BT firmware is already on the card.** The board downloads the *combo*
  blob `sduartIW416_wlan_bt.bin` (the name is literally **sd**io-wlan +
  **uart**-bt), 411,064 bytes, every boot. A W17 A/B against the Wi-Fi-only
  `sdIW416_wlan.bin` found 53 capture lines each, **zero differing** — they
  differ by the appended BT image, not the Wi-Fi build. Nobody has ever tried to
  talk to that BT image.
  ★ **CORRECTION 2026-08-25: "they differ by the appended BT image" is WRONG.**
  Byte-checked: the combo image neither starts with the WLAN image, nor ends
  with the BT image — the size arithmetic that suggested a concatenation was a
  coincidence. (An earlier version of this correction said "nor contains it at
  all"; that overreached. The combo carries both build IDs, `16.92.21.p155.2`
  WLAN and BT alike, LZMA-compressed rather than appended — so the original
  bullet's substance was closer to right than its mechanism.) u-blox describe the combo image as covering
  BOTH radios (SIM UBX-21010495 R09 §4.4.3), which is a different claim.
  Anything inferred from the concatenation idea is void.

## Licence — this one genuinely constrains the design

This tree is **permissive-only**; `tools/license-audit.sh` fails on copyleft.
**Most open-source Bluetooth host stacks are GPL** — BlueZ above all — and are
therefore unusable here in any form.

What *is* usable: NXP ships **`middleware/edgefast_bluetooth`** under
**BSD-3-Clause**. Treat it exactly as the Wi-Fi work treated `wifi_nxp` —
**a reference for clean-room re-implementation, never vendored** — and never
commit the firmware blob (NXP LA_OPT binary licence).

## Questions worth brainstorming

1. **Does the card answer `HCI_Reset`?** If yes, everything downstream is
   ordinary work. If no, the next question is whether the BT core needs its own
   bring-up (a vendor HCI sequence, a separate download, or a baud change)
   rather than riding the SDIO blob download.
2. **What baud, and does it change?** IW416 parts commonly boot at 115200 and
   are switched up by a vendor command. Triangulate `edgefast_bluetooth`
   against the MAYA-W1 datasheet and expect a disagreement somewhere — that is
   itself a probe trigger.
3. **What is the smallest honest capability?** Suggestion, not a decision:
   `HCI_Reset` → `HCI_Read_Local_Version_Information`, printing the
   manufacturer and LMP version **off the wire**. An un-fakeable oracle: a
   version string this firmware cannot invent.
4. **Coexistence.** One radio, one antenna path, 1×1. What BT activity does to
   uAP throughput is unknown and is a *measurement*, not a guess — and the uAP
   soak harness already exists to measure against.
5. **Where would it run?** W19 queues moving the Wi-Fi stack to the CM4. If BT
   lands on LPUART2 and Wi-Fi on USDHC1, the peripheral-to-core assignment
   question applies to both — see the `cm4-bringup` skill.

## Gating reality

**qemu2 has no Bluetooth model at all** — `hw/sd/iw416-sdio.c` models the SDIO
side only, and nothing models a UART-attached HCI controller. So a BT gate needs
either new qemu2 modelling (itself a risk-trigger under `cm4-bringup`) or an
honest card-absent/loopback gate plus a silicon transcript, which is how every
`m2_*` example started.

★ Note the **J9 pin 2 ↔ pin 4 jumper is a real TX↔RX loopback for LPUART2**,
and with `R1901` now bridged the module *can* drive that pad — so unlike before,
**check for contention** before relying on the loopback. The old note saying
there is none is out of date.

## Method — the parts this repo has already paid for

* Ask the firmware **one cheap question** first (W17 Phase 0 settled a whole
  design direction in an afternoon).
* **Bracket every reading** — a control before *and* after, or it is not
  evidence. W17 retracted conclusions twice for want of that.
* **A gate never shown to fail is decoration.** Demonstrate RED against a
  deliberately re-broken driver and quote the output in the gate header.
* **A missing field is not a zero**, and a counter that merely climbs is not an
  accounting. Print both ends of any link test.
* **Silicon wins.** And re-read stale comments before trusting them — this
  brief exists partly because one said `R1901` was DNP four days after it was
  bridged.

## Bench notes

* MCU-Link VCOM `/dev/cu.usbmodem5DQ2DDHVWO5EI3`; flash VCOM-free. The debug
  port drops after repeated `LinkServer run` + `pkill` cycles and needs a
  **physical repower** — the board stays alive and heartbeating, so check VCOM
  before blaming firmware.
* Run long gate sets in **batches**: a gate that "fails" in a long loop may just
  have been in flight when the outer command's own timeout fired.
