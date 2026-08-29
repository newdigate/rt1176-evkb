# M.2 Bluetooth on the MIMXRT1170-EVKB — A2DP source and sink, as Audio-library nodes

> ## ★★ STATUS 2026-08-29 — SUPERSEDES THE 2026-08-25 BLOCK BELOW
>
> **The programme is UNBLOCKED and BT-2 is COMPLETE on silicon.** The 08-25
> block's two headline conclusions are both obsolete:
>
> * **The fault was the u-blox MAYA-W161 MODULE, not the design** — proven by
>   substitution 2026-08-28: a genuine Embedded Artists Murata Type 1XK
>   (EAR00385, same IW416 silicon) in the same J54 socket runs the controller
>   with the same stock firmware (`bt.init` → `Bluetooth initialized`). u-blox
>   case CA-276115 tracks the MAYA-W161 provisioning question.
> * **The USB-dongle recommendation is WITHDRAWN.** Our clean-room
>   `M2Radio/hci` transport works on silicon on the Murata card. Root cause of
>   the residual silence was the HOST RTS (`-DM2_BT_ASSERT_CTS=ON`), not the
>   baud — the controller runs HCI at the download rate.
> * **BT-1 validated on silicon (NEW-21); BT-2 COMPLETE 2026-08-29 (NEW-6,
>   commit `5c27e40`)**: authenticated+encrypted BR/EDR link, L2CAP basic
>   mode, SDP — `sdp_avdtp_version=0x0103` read from two real headsets, AVDTP
>   Discover returning 2 audio-SNK SEPs. The 08-28 "silent peer" verdict was
>   retracted: it was our L2CAP cfg-rsp SCID bug. Next: BT-3 (NEW-9).
>
> ★ **LE Audio on this card is RULED OUT (NEW-22 rejected 2026-08-29).** The
> IW416's LE feature set stops at LE 2M / long range / PAST — no isochronous
> channels (an *optional* 5.2 feature), no LC3. NXP positions LE Audio on the
> IW61x tier ("supported utilizing Isochronous channels", LC3 on the host over
> HCI), and the MCUXpresso SDK's own LE Audio examples for this exact board
> (`unicast_media_sender` etc.) require the Embedded Artists **2EL (IW612)**
> module — the 1XK/IW416 is not listed for any of them. Any future LE work on
> this card is control-plane only (GAP/GATT); there is no LE audio path.

> ## ★ STATUS 2026-08-25 — READ BEFORE PLANNING FROM THIS DOCUMENT
>
> **BT-1 (the HCI transport) is BUILT, GATED AND DONE.** BT-2/3/4 are **blocked
> on hardware, not on this design.**
>
> The M2-MAYA-W161 **never answers HCI**. Everything host-side works: the V3
> UART firmware download delivers all 131,840 bytes with a real CRC retransmit
> recovered, and the transport is proven bidirectional against a fake
> controller. The card then transmits **nothing** — at 3 Mbaud, 921600, 460800
> or 115200, on either firmware path, with or without flow control, all
> measured on silicon (`framing=0` and zero bytes at every rate, so it is not a
> decode failure).
>
> **NXP's own EdgeFast `a2dp_source`, built for this board, fails identically** —
> and its `CONFIG_BT_IND_DNLD` was already set (auto-selected by Kconfig,
> verified by reading the generated config), so it used the same independent
> UART path with the same image.
>
> Refuted on silicon: the UART-config block, hardware flow control (re-tested
> with `CON[7]` sampled correctly), the baud rate (four rates, `framing=0`), and
> NXP's boot-sleep wake pulse (implemented; the card *reacts* — `start_inds`
> 2→3 — and the outcome is unchanged). Weakened: "wrong image". Surviving and
> untestable from the host: secure boot / signature.
>
> ★ **Two corrections worth carrying into any future plan.** The combo image
> over SDIO does **not** bring Bluetooth up on this card — measured in bytes:
> the BT core emits a fresh ROM start indication after the combo download
> succeeds. And the two downloads are **not** alternatives; that was the wrong
> Wi-Fi image. `CONFIG_BT_IND_DNLD` pairs `sdIW416_wlan.bin` over SDIO with
> `uartIW416_bt.bin` over UART, and with that pairing **both succeed**.
>
> ★ **The fault is after the jump**: between accepting a CRC-validated image and
> running a working controller. The ROM stops asking once it has the image
> (the combo path shows what rejection looks like — it keeps announcing), so
> delivery, transport and arrival are all proven.
>
> **The recommended route is a USB Bluetooth dongle** — `USBHost_t36` already
> carries HCI-over-USB, L2CAP and SDP under MIT. The layering below (§'s on
> A2DP/AVDTP/SBC as Audio nodes) is unaffected by that change of transport and
> is still the design to build against; only the HCI transport underneath it
> would differ.
>
> Full account: `examples/networking/m2_hci_probe/transcript_hw_evkb.txt` and
> `docs/superpowers/handoff/2026-08-24-w21-bluetooth-next-session.md`.

**Date:** 2026-08-23
**Hardware:** u-blox `M2-MAYA-W161-00C` (NXP **IW416**) in EVKB socket J54, with
the two hand bridges (`R1901`, `R404`) fitted on 2026-08-18.
**Supersedes:** the parked *Track B* of
`2026-08-17-m2-maya-w161-design.md`. That track was parked because `R1901` was
DNP; it is not any more, and this document replaces its B1–B3 rows.
**Brief:** `docs/superpowers/handoff/2026-08-22-w20-bluetooth-explore.md`.

## Goal — the north star, decided 2026-08-23

**Bluetooth audio both ways, integrated with the Audio library**:

* `AudioOutputBluetooth` — the audio graph streamed to Bluetooth headphones or a
  speaker (**A2DP source**).
* `AudioInputBluetooth` — a phone's or the Mac's audio brought into the graph
  (**A2DP sink**).

Decisions taken in the brainstorm, in the order they were taken:

| Question | Decision | Why |
|---|---|---|
| End state | **C — Bluetooth audio**, not BLE-only, not transport-and-park | the audio tree (Acid Box, the codec, the UAC work) is where this board's value is |
| "Audio input" | **A2DP sink only** — no HFP/HSP, no SCO | one profile family both ways, one codec; SCO would need PCM pads that run to the SDRAM bus on this board, or SCO-over-HCI, an unknown |
| Direction first | **Source first** (EVKB → headphones) | reaches an audible oracle with the fewest new protocol pieces and no clock recovery; the sink role adds an SDP server, probably AVRCP, a decoder and a clock servo before first sound |
| Stack shape | **clean-room, layered, bare-metal, in `M2Radio`**; YAGNI: L2CAP basic mode only, SBC only, AVRCP deferred | no permissive BR/EDR A2DP host exists to port (NimBLE is LE-only, Zephyr's classic is kernel-bound, bluedroid is Apache-2.0 and enormous, BTstack is dual-licensed) |
| SBC codec | **clean-room from the public A2DP spec** | BlueZ `libsbc` is LGPL; bluedroid's is Apache-2.0 and the tree retired its one Apache exception |
| Gating | Python fake controller + scripted peer on LPUART2 via `-serial unix:` — **zero qemu2 changes** | `serial_hd(1)` is LPUART2 and the LPUART model implements chardev RX; the GPL firewall is untouched |
| Core | **CM7**, decoupled from W19 | the UART path is tiny and has none of the CM4's blob/bss problem |

**Assumption, flagged:** a Bluetooth headphone or speaker is available on the
bench for BT-3. If it is not, BT-3 and BT-4 swap and the Mac becomes the peer.

---

## 1. What the card is, and what is already proven

NXP IW416 (88W8978 lineage): Wi-Fi 4 dual-band **1×1** + dual-mode Bluetooth
**BR/EDR + LE 5.2**. WLAN over SDIO (uSDHC1), Bluetooth over UART (H4, LPUART2 =
`Serial2`). One antenna, one radio.

| Surface | Status |
|---|---|
| SDIO enumeration, 1.8 V pads (`manfid=0x2DF cardid=0x9158`) | proven |
| Combo blob download over SDIO (`sduartIW416_wlan_bt.bin`, 411,064 B) | proven, every boot |
| Scan / WPA2-PSK STA / lwIP / DHCP / TCP / aggregation / uAP / Arduino `WiFi` facade | proven + gated, sweep 119 (121 after BT-1) |
| **Bluetooth HCI — any command** | **never answered**; the only reading predates `R404` |
| BT PCM (SCO/eSCO) | unavailable — pads run to the SDRAM data bus |
| LPUART2 flow control | unusable — RTS is the gigabit PHY's reset (`R1866`), CTS its interrupt (`R1816`) |

### Facts established by this exploration (checked, not recalled)

1. **LPUART2 is confirmed by NXP's own M.2 configuration.** `board.h` says
   `LPUART7`, but that is the non-M.2 default. The
   `WIFI_IW416_BOARD_MURATA_1XK_M2` branch of
   `controller_hci_uart_get_configuration()` uses **instance 2, default
   115200, running 3,000,000, RTS/CTS on**. The handoff's predicted
   "disagreement somewhere" resolves in `Serial2`'s favour.
2. **The bring-up sequence is short.** After the combo SDIO download — which
   the Wi-Fi driver already performs — Bluetooth is simply *up* on the UART at
   115200. There is no separate BT download on this path (`CONFIG_BT_IND_DNLD`
   is the alternative, BT-only-over-UART path, which we do not need).
   ★ **THIS ASSUMPTION DID NOT HOLD (2026-08-23, silicon).** Bluetooth was NOT
   up after the combo download, so BT-1 built the `CONFIG_BT_IND_DNLD` path
   after all — and it delivers the whole image, and the card is still silent.
   Both paths have now been tried and neither yields an HCI reply; they are
   also mutually exclusive (a BT UART download leaves the later WLAN SDIO
   download at `fw_download=cmd-timeout`). NXP waits
   100 ms plus 60–260 ms, sends vendor opcode **`0xFC09`** (OGF 0x3F, OCF 0x09)
   with the new rate as a 4-byte little-endian parameter, reads a 7-byte
   Command Complete, waits 500 ms and re-opens the UART at the new rate.
3. **The licence boundary is narrower than the brief says.** Only the
   `middleware/edgefast_bluetooth` wrapper is BSD-3-Clause. The host stack under
   it (**EtherMind**) and the controller bring-up file itself
   (`middleware/wireless/ethermind/port/pal/mcux/bluetooth/controller/controller_wifi_nxp.c`)
   are **NXP LA_OPT** — proprietary. They inform protocol facts; nothing is
   transcribed from them.
4. **A bidirectional Bluetooth gate needs no qemu2 change.** In
   `hw/arm/fsl-imxrt1170.c`, `serial_hd(1)` is LPUART2; `hw/char/imxrt_lpuart.c`
   implements chardev receive with a FIFO and `RDRF`. A second `-serial` plus a
   host-side Python process puts a fake controller on the card's HCI port.
5. **Two stale comments, not one.** `m2_sdio_probe.cpp` (flagged by the brief)
   and the core's `imxrt1176/HardwareSerial2.cpp` ("RX IS DEAD AS BUILT") both
   still say `R1901` is DNP.
6. **`Serial2`'s RX ring is 64 bytes on a 24 MHz clock root**, and
   `HardwareSerialIMXRT` has no `addMemoryForRead()`. Fine at 115200 for short
   events; an Extended Inquiry Result event is 257 bytes.
7. **The 26 post-PDn edges were counted as edges because the probe re-muxes
   the RX pad to GPIO for that window** — which is also why no bytes were
   captured. With `R1901` bridged the pad should stay on LPUART2.

---

## 2. What the north star commits us to

| Layer | Needed | Licence reality |
|---|---|---|
| HCI H4 transport; BR/EDR + LE command set | yes | clean-room, small |
| **Baud switch without flow control** | **a requirement** — SBC 44.1 kHz joint-stereo at bitpool 53 is 328 kbps; with RTP/L2CAP/ACL/H4 headers and UART start/stop bits that is ~420–450 kbaud. 115200 cannot carry it; 921600 is the floor, 3 M the comfortable rate | core work (clock root, eDMA) |
| BR/EDR link: inquiry, page, SSP Just-Works pairing, link keys | yes | clean-room |
| L2CAP basic mode (signalling, CO channels, configuration, segmentation) | yes | clean-room |
| SDP server + a one-query SDP client | yes | clean-room |
| AVDTP signalling + RTP media transport; A2DP SBC negotiation | yes | clean-room |
| **SBC encoder and decoder** | yes | clean-room from the spec |
| AVRCP (play/pause, absolute volume) | phones expect it; sinks work without it | deferred, optional |
| **Clock recovery** (sink direction) | the phone's 44.1 kHz ≠ our I2S 44.1 kHz; the UAC1 −86 ppm class of problem with no feedback endpoint | the one new algorithmic piece |

---

## 3. The programme map

Four sub-projects, **each its own spec → plan → silicon cycle**, because each
one's shape depends on what the card said in the one before (the Wi-Fi track's
rule). Every phase carries an un-fakeable assertion. Nothing below BT-1 is
designed here; the rows are the commitments, not the designs.

### BT-1 · HCI transport & identity — designed in §4

| Phase | Deliverable | Un-fakeable assertion |
|---|---|---|
| **B0** | In `m2_sdio_probe`: `HCI_Reset` **bracketed** before and after the blob download; the post-PDn ROM bytes in hex; both stale comments fixed | a `04 0E 04 01 03 0C 00` Command Complete, and *when* it first appears |
| **B1** | `M2Radio/hci/`; `networking/m2_hci_probe`: Reset → Read_Local_Version → Read_BD_ADDR → Read_Buffer_Size | LMP version, manufacturer (0x0025 NXP or 0x0048 Marvell) and BD_ADDR off the wire |
| **B2** | BR/EDR **Inquiry + Remote_Name_Request** — on the path (we need the headphones' address); replaces the earlier BLE beacon/scan idea, which becomes an optional side quest | the headphones' name and BD_ADDR off the wire |
| **B3** | Transport at speed: `0xFC09`; LPUART2 clock-root decision; **eDMA ring RX**; loss accounting under the uAP soak via the controller's local loopback mode | N ACL packets sent = N echoed at 3 Mbaud with Wi-Fi running, both ends printed |

### BT-2 · BR/EDR link, L2CAP, SDP

| Phase | Deliverable | Un-fakeable assertion |
|---|---|---|
| **B4** | ACL `Create_Connection`; SSP Just-Works (IO capability NoInputNoOutput); link key persisted in the EEPROM emulation; reconnect with key | `Encryption_Change`; the headphones' own "connected" prompt |
| **B5** | L2CAP basic mode: signalling channel, connect/config/disconnect, segmentation from `Read_Buffer_Size`, `Number_Of_Completed_Packets` flow | **L2CAP Echo Request → Echo Response** from the sink — spec-mandated round trip |
| **B6** | SDP client (one query: the sink's AVDTP version and features) + SDP server (our A2DP source record) | the sink's AVDTP version off the wire |

### BT-3 · A2DP source + SBC encoder — first half of the capstone

| Phase | Deliverable | Un-fakeable assertion |
|---|---|---|
| **B7** | AVDTP initiator: discover, get-capabilities, set-configuration (SBC, 44.1 kHz, joint stereo, 8 subbands, 16 blocks, loudness, bitpool ≤ 53), open, start | the sink's SEP list and capabilities off the wire; `START` accepted |
| **B8** | Clean-room SBC encoder; host-clang unit tests; `.sbc` output decoded by ffmpeg on the Mac (a test tool only — nothing links it) | bit-exact frame headers; audible decode |
| **B9** | RTP media transport paced from the audio ISR; **`AudioOutputBluetooth`** (`AudioStream`, 128-sample blocks at 44.1 kHz); coexistence measured against the uAP soak | **the Acid Box audible in the headphones**; Wi-Fi throughput delta quantified |

### BT-4 · A2DP sink + clock recovery — second half

| Phase | Deliverable | Un-fakeable assertion |
|---|---|---|
| **B10** | Acceptor role: `Write_Scan_Enable`, SDP sink record (+ an AVRCP target stub if iOS demands it), accept pairing, AVDTP acceptor | the phone offers the EVKB as an audio output |
| **B11** | SBC decoder; buffer-level servo driving a resampler or SAI clock trim | drift in ppm over minutes, corroborated by a second clock |
| **B12** | **`AudioInputBluetooth`** | phone/Mac audio out of the WM8962 jack and into the graph |

### Gating, for all four

* **Tier 0 — card-absent fallback** (`run_qemu.sh`): LPUART2 on a null
  chardev. Asserts a *positive* token — a reason code — and a later heartbeat,
  never an absence. Each goes into `tools/gate-vacuity.test.sh`.
* **Tier 1 — `[hci]`** (`run_qemu_hci.sh`): `-serial unix:` on slot 1 and a
  Python fake controller *and scripted peer*, fixtures captured on silicon and
  mutated, never invented.
* **Tier 2 — silicon** `transcript_hw_evkb.txt`: the only place a claim about
  the card lives.
* Every regression gate is **demonstrated RED** against a deliberately re-broken
  driver before it is trusted, and the demonstration is quoted in its header.

---

## 4. BT-1 design — HCI transport & identity

**Plan granularity:** the first implementation plan covers **B0–B2**. B3 gets
its own plan once B2's silicon result is in (its shape is fixed below). BT-2,
BT-3 and BT-4 each get their own spec.

### Where it lives

`M2Radio/hci/`, a new selectively-imported subdirectory beside `sdio/`,
`iw416/`, `lwip/` and `arduino/`. A Bluetooth sketch imports
`import_evkb_library(M2Radio hci)` and never compiles the SDIO layer; a Wi-Fi
sketch never compiles `hci/`. MIT; nothing vendored; `VENDORING.md` unchanged.
The `M2Radio` pin in `evkb.cmake` is bumped when `hci/` is pushed.

One new example, `examples/networking/m2_hci_probe/`. B0 lives in the existing
`examples/networking/m2_sdio_probe/`.

### Components

| Unit | Does | Depends on |
|---|---|---|
| `H4Parser` | byte stream → packets. Knows the two H4 types a host receives (`0x04` event, `0x02` ACL — loopback echoes arrive as Loopback Command *events*, not command packets; SCO is unused), takes lengths from the header, hands up complete packets. Any other type byte is a framing fault. A pure state machine with no I/O — host-testable. | nothing |
| `HciTransport` | owns `Serial2`: TX writes; RX drains the ring into `H4Parser`. Calls `addMemoryForRead()` for a 1 KB ring. In B3 its RX side becomes an eDMA ring behind the same interface. | core `Serial2` |
| `Hci` | command queue honouring `Num_HCI_Command_Packets`; Command Complete / Command Status matched by opcode; per-command timeout; callbacks for asynchronous events and ACL data for the layers above. `run(cmd, timeoutMs)` is the blocking helper for probes; the queued/callback API is what `bt/` will use. Fixed pools, no heap. | `H4Parser`, `HciTransport` |
| `HciPump` | a yield-attached `EventResponder`, one bounded pass per `yield()` — the Wi-Fi facade's exact shape (`WiFiClass::serviceEvent`), so both coexist and `delay()` services both. | core `EventResponder` |
| `m2_hci_probe` | B1: Reset → Read_Local_Version → Read_BD_ADDR → Read_Buffer_Size. B2: Inquiry (GIAC, `Inquiry_Length` 0x08 = 10.24 s, unlimited responses) then Remote_Name_Request per result. B3: `0xFC09`, re-open, loopback burst. Prints every field it received, then heartbeats. | all of the above |

### Data flow

Sketch → `Hci::submit()` → queue → `HciTransport` TX (when `ncmd > 0`).
RX: LPUART2 ISR → ring → pump pass → `H4Parser` → `Hci` → opcode match for
Command Complete/Status, callback for everything else. Nothing blocks outside
`run()`, and `run()` blocks only through `delay()`, which keeps both pumps
alive.

### Error handling — every exit named

The `lastError()` idiom from the Wi-Fi facade. H4 has no sync marker, so a lost
byte desyncs the stream for good, and with no flow control the card cannot be
paused. The parser therefore treats an implausible header — unknown packet
type, event length > 255, ACL length > the `Read_Buffer_Size` value once known
(> 1024 before it is known, a plausibility bound not a spec limit) — as a **framing fault**: discard until the line has been idle
50 ms, count it, and fail the in-flight command as `framing` rather than
`timeout`. `timeout`, `framing`, `ncmd_starved` and `queue_full` are distinct
counters, printed on every probe line. Print both ends of every exchange.

### B0, in the existing probe — no library yet

1. Keep the RX pad on LPUART2 through the post-PDn window and print the ROM's
   bytes in hex (`rom_bytes: n=.. hex=..`), not edges.
2. Send `HCI_Reset` **before** `iw416_begin()`/download and **after**. Each is
   bracketed by an equal-length quiet window so a ROM that transmits
   continuously is not read as a reply; print `quiet=<n> reply=<hex|none>`.
3. Never re-`begin()` `Serial2` mid-probe — that re-mux is what latched the
   spurious `0x00` in the existing reading.
4. Fix the stale `R1901` comments in `m2_sdio_probe.cpp` and in the core's
   `HardwareSerial2.cpp`.

Expected from NXP's sequence: nothing before the download, a Command Complete
≥100 ms after it. Either way the answer reshapes B1: if the ROM's bytes look
like a UART download request and nothing answers after the SDIO download, B1
grows a UART `firmware_download` for the BT-only image instead.

The existing `m2_sdio_probe` gates must stay green: the card-absent gate
asserts vacuity guards, so the new lines need a card-absent form
(`hci_pre_download: reply=none` with the quiet count, not an absent line).

### B3 — shape fixed now, design deferred to its own plan

Written after B2's silicon result, per the per-phase-group rule. Fixed now:

* `0xFC09` with the rate in little-endian, 7-byte reply, 500 ms, re-open.
* **Clock root by measurement**: the 24 MHz root reaches 921600 at 16×
  oversampling but 3 M only at 8× (`OSR=7`, `SBR=1`). Re-rooting LPUART2 to a
  PLL-derived clock for 16× at 3 M is the alternative; the loss numbers decide.
* **eDMA ring RX** via the core's `DMAChannel` (main eDMA completion reaches
  the CM7), replacing `HciTransport`'s RX behind the same interface, so byte
  loss stops depending on ISR latency while the Wi-Fi driver blocks for a
  millisecond in a CMD53.
* **Loss accounting** uses the spec's `Write_Loopback_Mode` (local): the card
  echoes our ACL bursts while the uAP soak runs; sent and echoed counts both
  printed. If the IW416 firmware rejects loopback mode, the measurement moves
  to B5 over a real ACL link — the number is still required before BT-3.
* That number is **silicon-only**: QEMU's chardev has no baud rate.

### Testing

* **Card-absent** (`m2_hci_probe/run_qemu.sh`): asserts
  `hci_reset=timeout reason=no_response` **and** a later `hb` — positive
  tokens — and is added to `tools/gate-vacuity.test.sh`.
* **`[hci]`** (`m2_hci_probe/run_qemu_hci.sh`): appends
  `-serial unix:/tmp/m2hci_<pid>_<phase>.sock,server` after `gate_console`'s
  slot-0 `-serial file:` so it lands on LPUART2, backgrounds QEMU through
  `qrun`, runs `hci_peer.py` (in the example directory, the `lwip_peer.py`
  shape: connect with retry, run a named phase, write a `.result`), then
  reaps. Two details decided at plan time: the socket lives in `/tmp` because
  macOS caps `sun_path` at 104 bytes and a checkout path alone can exceed it
  (the `mon.sock` hazard, sidestepped rather than inherited); and `server`
  without `nowait`, so QEMU holds the guest until the peer is connected and
  the firmware's first byte cannot be lost — which is what lets every count
  the gate asserts be strict. `hci_peer.py` answers the four B1 commands with
  values the firmware cannot invent (e.g. manufacturer `0x1234`), injects
  Inquiry Result and Remote Name Request Complete events for B2, and is
  scriptable per phase to **drop a reply**, **inject garbage** and **hold
  `ncmd=0`** — so `timeout`, `framing` and `ncmd_starved` are gated, not only
  the happy path. One gate id, four QEMU runs.
* **Silicon** `transcript_hw_evkb.txt`.
* **Demonstrated RED**: change the fake's manufacturer and the `[hci]` gate
  must fail by name; break the opcode match and the card-absent gate must
  still pass while `[hci]` fails. Both quoted in the gate headers.
* `H4Parser` gets host-clang unit tests on byte fixtures.
  ★ **Amended after BT-1 shipped:** the intent was fixtures *captured on
  silicon* and then mutated (the UAC2 P2 lesson: never invent byte arrays), and
  that is NOT what happened — B0/B1 silicon is deferred, so every fixture in
  the host tests and in `hci_peer.py` is written from the specification. They
  are therefore a test of "does this match the spec as read", not "does this
  match the card". Task 3's first captured Command Complete is the point at
  which they should be re-based on real bytes.
* Sweep target **119 → 121**, re-measured by running the sweep.

### Core changes

* B1: `addMemoryForRead()` / `addMemoryForWrite()` on `HardwareSerialIMXRT`
  (the Teensy idiom). Gated by the existing serial examples.
* B0: the `HardwareSerial2.cpp` comment.
* Nothing else until B3 (clock root, eDMA), which gets its own plan.

### Constraints that have not changed

* **No flow control on LPUART2, ever.** `R1902` stays DNP.
* **Header D0–D2 are committed** to Bluetooth for the duration (`R2`/`R3`/`R8`
  fitted), and `GPIO_AD_31` (PDn) is D12/MISO.
* **The J9 pin 2 ↔ pin 4 loopback can now contend with the card** — check
  before relying on it.
* The Wi-Fi and Bluetooth halves share one firmware image, downloaded once
  over SDIO; the Bluetooth block rides that download.

### Risks

* ~~The module variant may expect the BT-only UART download path~~ —
  ★ **ANSWERED ON SILICON 2026-08-23, and the answer is YES.** The card never
  answers HCI over LPUART2, before or after the combo SDIO download. It
  transmits a five-byte pattern three times at power-up (`AB 01 72 00 47`,
  containing `0x7201` = the `hw_version` read independently over SDIO — the
  cross-check that proves the UART is correctly framed), then goes silent, and
  BT_WAKE_HOST never asserts. The combo download also stops 8,776 bytes short
  of the blob (`sent=402288/411064 last_req=0`). So **B1 grows a
  UART `firmware_download`** — NXP's `CONFIG_BT_IND_DNLD` path — and that is
  now the first task of the next phase, not a risk.
  ★ **CORRECTION 2026-08-25:** the words "consistent with the image being WLAN
  + an appended BT part that SDIO never delivers" stood here and were WRONG.
  Byte-checked: the combo image neither starts with the WLAN image nor ends
  with the BT image, and the 279,164 + 131,840 ≈ 411,064 arithmetic was a
  coincidence — it is not a concatenation.
  ★ **BUT THE ORIGINAL WORDING OF THIS CORRECTION OVERREACHED**, and is fixed
  here: it said the combo "nor contains it at all", which is a CONTENT claim
  the byte check never supported. The combo carries BOTH build IDs —
  `w8978o-V0` (WLAN, 2026/03/12 16:57:22) and `w8978d-V0 … BT_UART`
  (17:02:37), both `16.92.21.p155.2` — timestamps identical to the standalone
  images. It contains both radios' firmware; it is merely LZMA-compressed
  rather than concatenated. The short download is also deliberate on the
  card's side — widening the idle poll to 15 s left `sent` unchanged. And
  u-blox's own procedure (SIM §4.4.3) treats the combo image as covering BOTH
  radios, with no BT-only UART download at all. Evidence:
  `examples/networking/m2_sdio_probe/transcript_hw_evkb.txt` (B0 section) and
  `examples/networking/m2_hci_probe/transcript_hw_evkb.txt`.
* ~~The manufacturer may read Marvell `0x0048` rather than NXP `0x0025`~~ —
  **still unanswered, and now blocked**: no HCI command has been answered, so
  no identity has been read. It stays open behind the UART download.
* The IW416 firmware may not implement local loopback mode (B3 fallback above).
* Interrupt-driven RX at 3 M without flow control may still lose bytes under
  SDIO load even with eDMA; B3 measures, and 921600 or 1.5 M is the honest
  fallback — the A2DP rate budget above is met at 921600.

---

## 5. Bench notes carried forward

* MCU-Link VCOM `/dev/cu.usbmodem5DQ2DDHVWO5EI3`; flash VCOM-free. The debug
  port drops after repeated `LinkServer run` + `pkill` cycles and needs a
  physical repower — the board stays alive; check the VCOM before blaming
  firmware.
* Read `DHCSR` first on any apparent lockup.
* Run long gate sets in batches.

## Sources

* `docs/superpowers/handoff/2026-08-22-w20-bluetooth-explore.md`
* `docs/m2-evkb-revc3.md` — board map, populate status, the two bridges
* `examples/networking/m2_sdio_probe/transcript_hw_evkb.txt` — the bridge verification and the post-PDn reading
* `~/Development/mcuxsdk-ws/mcuxsdk/examples/_boards/evkbmimxrt1170/edgefast_bluetooth_examples/spp/cm7/hardware_init.c` — the M.2 HCI UART configuration (LPUART2, 115200 → 3 M)
* `~/Development/mcuxsdk-ws/mcuxsdk/middleware/wireless/ethermind/port/pal/mcux/bluetooth/controller/controller_wifi_nxp.c` — the bring-up sequence and `0xFC09` (LA_OPT: facts only)
* `~/Development/qemu2/hw/arm/fsl-imxrt1170.c`, `hw/char/imxrt_lpuart.c` — `serial_hd(1)` = LPUART2, chardev RX implemented
* Bluetooth Core Specification v5.2 Vol 4 Part A (H4), Vol 4 Part E (HCI); A2DP v1.3 §12 (SBC); AVDTP v1.3
