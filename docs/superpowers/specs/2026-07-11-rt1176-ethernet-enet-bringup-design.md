# RT1176 Ethernet — 10/100 ENET (FEC) MAC + RTL8201 PHY + ARP/ICMP ping — Design

**Status:** approved (design), ready for implementation plan
**Date:** 2026-07-11

## Goal

Bring up the **10/100 ENET (Freescale FEC)** MAC and its **RTL8201 RMII PHY** on the
MIMXRT1176-EVKB **CM7** so the board can put a frame on the wire and **answer a ping**.
v1 success = a host on a real RJ45 link `ping`s the board's static IP and gets replies (weaker
pass: the board's ARP reply for its own IP is visible on the wire), **and** the matching QEMU
gate drives the same ARP + ICMP exchange end-to-end.

Everything is hand-rolled in the core's bare-register idiom (a new `cores/imxrt1176/enet.c`),
**gate-first in QEMU, then HW-verified** — reusing QEMU's existing `imx.enet` FEC model (no new
device model). The responder is a **~250-line hand-rolled ARP + ICMP-echo responder** on the raw
MAC TX/RX path — **not** lwIP. lwIP (raw-API + `ethernetif`) and the Arduino `Ethernet` socket
API are explicit **later milestones** on this same foundation, out of scope for v1.

The two Ethernet ports we are **not** touching: **ENET_1G** (RGMII, RTL8211F, `0x40420000`,
IRQ 141 — deferred) and **ENET_QOS** (Synopsys DWC EQOS, `0x4043C000`, a different IP QEMU only
stubs — ignore).

## Why this shape (exploration findings)

Five parallel read-only recon passes over the SDK, QEMU, FNET/NativeEthernet, the core/gate
plumbing, and lwIP/PHY established the design. Highlights, with citations:

- **The MAC + RMII + PHY-reset sequence is fully specified by the SDK example for THIS board** —
  `_boards/evkbmimxrt1170/driver_examples/enet/txrx_transfer/` (`EXAMPLE_ENET = ENET`, the 10/100
  FEC). Clock: SysPLL1 (1 GHz) → Div2 (500 MHz) → `kCLOCK_Root_Enet1` (mux=4 = SysPll1Div2, div=10)
  → **50 MHz** (`hardware_init.c:21-29`); MDIO source clock = the **Bus root** freq, fed to
  `ENET_SetSMI` and as `srcClock_Hz` to init. Ref clock driven **out**:
  `IOMUXC_GPR->GPR4 |= ENET_REF_CLK_DIR (bit 1, mask 0x2)` (`hardware_init.c:57-58`). ERR050396:
  `IOMUXC_GPR->GPR28 &= ~CACHE_ENET (bit 7, mask 0x80)` (`hardware_init.c:60-64`). PHY reset:
  `GPIO12.IO12` low → **10 ms** → high → **150 ms** settle (`hardware_init.c:66-69`). PHY addr
  **0x03**, `phyrtl8201_ops` (`app.h:17-23`). The example is **polled** — it never touches
  `ENET_IRQn` (grep confirms no `EnableIRQ(ENET_IRQn)`).

- **QEMU is genuine reuse — the SoC already instantiates and realizes the 10/100 `enet`** at
  `0x40424000` / IRQ **137**, with MAC + 1588-timer IRQ lines OR-combined onto 137, and
  `qemu_configure_nic_device(...)` so a host backend binds (`hw/arm/fsl-imxrt1170.c:385, 897-917`).
  The `imx.enet` model (`hw/net/imx_fec.c`) is **register-complete** for the ARP/ping path
  (ECR/EIR/EIMR/RDAR/TDAR/MMFR/MSCR/RCR/TCR/PALR/PAUR/RDSR/TDSR/MRBR + the legacy **and** enhanced
  descriptor DMA engine + RXF/TXF→IRQ). SLIRP (`-netdev user`) answers ARP + ICMP echo. **This model
  runs Linux networking on i.MX6/7 — it is well-tested.**

- **★ The one mandatory QEMU fix — PHY MDIO address.** The SoC pins the emulated PHY at MDIO
  address **2** (`FSL_IMXRT1170_ENET_PHY_NUM = 2`, `include/hw/arm/fsl-imxrt1170.h:173`), but the
  real RTL8201 answers at **3**. `imx_phy_read` computes `phy = reg/32` and returns **`0xffff`** for
  any non-matching address (`imx_fec.c:240-260`). `0xffff` has the BMSR link bit (0x0004) **set**, so
  a naive driver reads "link up" while every real PHY register is garbage — the exact
  **circular false-pass** trap that bit us on `attachInterrupt` ([[rt1176-gpio-irq-cm7-trap]]).
  Fix = one line, **SoC header, not the device model**: `FSL_IMXRT1170_ENET_PHY_NUM` 2 → 3.

- **★ The PHY-driver choice avoids a second QEMU change.** The SDK `phyrtl8201` driver **validates
  the OUI** — `PHY_RTL8201_Init` reads ID regs 2/3, compares to `PHY_DEVICE_ID = 0x001CC816`, retries
  1000× (`fsl_phyrtl8201.c:80-102`). QEMU's model reports **LAN9118** IDs (`0x0007/0xc0d1`,
  `lan9118_phy.c` + `mii.h:118-119`), so that driver would fail the ID check unless we *also* teach
  the model the Realtek ID. But **Zephyr's EVKB overlay drives this same 10/100 PHY with the generic
  clause-22 driver** (`ethernet-phy`/`phy_mii.c` — BMCR/BMSR/ANAR only, no vendor pages, no OUI check;
  `mimxrt1170_evk_mimxrt1176_cm7_B.overlay:88-96`). So a **~30-line hand-rolled clause-22 link
  bring-up** needs zero RTL8201-specific writes, works on silicon, **and** works against QEMU's
  LAN9118 model unchanged. The vendor page-7/INER dance is only for the link-change **interrupt**,
  which a polled v1 does not use.

- **The port reference is FNET's FEC driver — same IP, CPU-agnostic.** NativeEthernet
  (`~/Development/NativeEthernet`) is the Teensy-native Arduino Ethernet lib; it never touches
  hardware — all MAC init is inside **FNET** behind a `netif_init`→`eth_cpu_init` hook. FNET's
  `src/port/netif/fec/fnet_fec.c` is the generic FEC MAC driver (**identical IP on 1062 and 1176**);
  its register map + legacy 8-byte BD struct (`fnet_fec.h`), ring-setup (`fnet_fec.c:201-281`), and
  the two MDIO functions (`_fnet_fec_phy_read/_write`, `:925/:984`) port with only base-address +
  PHY-address changes. FNET's **board layer** (`src/port/cpu/mimxrt/fnet_mimxrt_eth.c`, 239 LOC) is
  100% RT1060-specific (KSZ8081 @ addr 2, EMC pins) — it is **rewritten** for 1176, not ported. FNET's
  raw TX/RX (`fnet_fec_output :674`, `_fnet_fec_input :534`) are written against FNET's `netbuf` mbuf
  and `_fnet_eth_input` — the two coupling seams we **cut**, replacing them with flat-buffer `memcpy`,
  which a hand-rolled responder wants anyway. (Vendoring the SDK `fsl_enet.c` — 3798 LOC, callback
  style, pulls in `fsl_phy` — was considered and rejected: too foreign to this core, which has never
  vendored an SDK driver.)

- **Core plumbing is well-understood.** The core is flat, one `.c` per peripheral; ENET lands in a new
  `cores/imxrt1176/enet.c` following the `semc.c` shape (clock root + LPCG gate → pins → controller →
  rings). `DMAMEM __attribute__((section(".dmabuffers")))` → OCRAM @ `0x20240000` is the DMA-reachable
  region; DTCM/ITCM are **off-limits** to the ENET bus-master — exactly ERR050396 and the same
  discipline the USB/SD bus-masters follow ([[rt1176-sd-usdhc]], [[rt1176-edma-dmachannel]]). D-cache is
  **off**, so no cache maintenance. ENET is **absent** from `imxrt1176.h`, the generator's `WANTED`,
  the `IOMUXC_GPR` set (no GPR4/GPR28), and `core_pins.h`'s `IRQ_NUMBER_t` — all four need additions.

## Scope

**In scope (v1):**
1. ENET register block + `IOMUXC_GPR_GPR4`/`GPR28` + ENET clock-root/LPCG-gate defs in
   **both** `imxrt1176.h` and `tools/gen_imxrt1176_h.py`; `IRQ_ENET = 137` in `core_pins.h`.
2. `cores/imxrt1176/enet.c`: clock/gate, RMII pin mux, GPR4 ref-clk-out, GPR28 cache-clear, PHY
   reset, MAC config, TX/RX descriptor rings in `DMAMEM`, raw `enet_send_frame`/`enet_read_frame`,
   MDIO read/write, generic clause-22 `enet_phy_link_up`.
3. A hand-rolled ARP + ICMP-echo responder (static IP + MAC, polled).
4. The QEMU `FSL_IMXRT1170_ENET_PHY_NUM` 2 → 3 fix.
5. A single QEMU gate (`evkb/enet_test/`) accumulating TX/RX, link, and ping tokens; HW verification.

**Explicitly deferred (YAGNI):** lwIP (raw-API + `ethernetif`) — **milestone 2**; the Arduino
`Ethernet` socket API (client/server/UDP) — **milestone 3**; DHCP client; interrupt-driven RX/TX
(IRQ 137 is defined but v1 **polls**, matching the SDK example); the enhanced/1588 buffer descriptor +
IEEE-1588 timestamping; multicast hash filtering + MIB stats; ENET_1G (RGMII/RTL8211F) and ENET_QOS;
silicon-ID-derived MAC (v1 uses a fixed locally-administered MAC); the RTL8201 link-change interrupt
(page-7/INER).

## Decisions (resolved during brainstorming)

- **Responder = hand-rolled ARP/ICMP (Path B), not lwIP.** Isolates exactly the silicon + model
  bring-up with zero third-party-stack variables; deterministic known-frame-in → exact-bytes-out gate;
  only the phy-num fix on the QEMU side. lwIP/sockets sequence after, on the same MAC/PHY foundation.
- **MAC ring source = port FNET `fnet_fec` register/ring/MDIO logic**, hand-rolling flat-buffer raw
  TX/RX on top — not vendoring `fsl_enet.c`. The SDK `enet` example is ground truth for the 1176
  board bring-up (pins/clock/ref-clk-dir/PHY-reset) that FNET lacks. This is the established
  hybrid-port pattern ([[rt1176-spi-library-move]], [[rt1176-wire-library-move]]).
- **PHY bring-up = generic clause-22** (BMCR reset → ANAR advertise → autoneg restart → poll BMSR) —
  **no OUI check, no vendor pages** (Zephyr-proven sufficient for this PHY; keeps QEMU's LAN9118 model
  usable unchanged).
- **Init = lazy** (`enet_init()` called by the sketch, later `Ethernet.begin()`), **not** boot —
  the 160 ms of PHY-reset settle must not tax sketches that never use Ethernet. (SEMC inits at boot
  only because SDRAM must exist before `main`; USB already inits on demand.)
- **Static IP + fixed locally-administered MAC** for v1 (no DHCP). The QEMU peer and the HW host both
  target `ENET_IP`.
- **Gate topology = `-nic socket,listen=…,model=imx.enet` + a Python peer** that injects inbound
  frames and asserts exact reply bytes (mirrors the HW test: host pings board), plus
  `-object filter-dump…frames.pcap` for offline TX byte-assertion. Same scaffold as `usb_data_test`
  (external backend + Python driver + `gate_tmp`).
- **QEMU fix = `phy-num` 2 → 3** (mandatory; see the false-pass note above).

## Architecture — components

### 1. Register plumbing (`imxrt1176.h` + `tools/gen_imxrt1176_h.py` + `core_pins.h`)
Auto-generated header → add to **both** the `.h` and the generator, or a regenerate silently drops it.
- Add `"ENET":"ENET"` to the generator `WANTED` set → auto-emits `#define ENET_BASE 0x40424000u`.
- Hand-author the ENET register block (SEMC flat `(*(volatile uint32_t*)(ENET_BASE+off))` idiom),
  offsets mirrored from `RT1170/periph/PERI_ENET.h` (the register ground truth — the cm7 COMMON
  header only carries `ENET_BASE`/IRQs): `EIR 0x004, EIMR 0x008, RDAR 0x010, TDAR 0x014, ECR 0x024,
  MMFR 0x040, MSCR 0x044, MIBC 0x064, RCR 0x084, TCR 0x0C4, PALR 0x0E4, PAUR 0x0E8, OPD 0x0EC,
  TFWR 0x144, RDSR 0x180, TDSR 0x184, MRBR 0x188, TACC 0x1C0, RACC 0x1C4`. Bit fields:
  `ECR: RESET(bit0) ETHEREN(bit1) EN1588(bit4) DBSWP(bit8)`; `EIR/EIMR: MII(bit23) RXF(bit25)
  TXF(bit27)`; legacy BD status: TX `R(bit15) TO1(14) W(13) TO2(12) L(11) TC(10)`, RX
  `E(bit15) RO1(14) W(13) RO2(12) L(11)` + error bits.
- Add `IOMUXC_GPR_GPR4` (`0x400E4010`, `ENET_REF_CLK_DIR = 1<<1`) and `IOMUXC_GPR_GPR28`
  (`0x400E4070`, `CACHE_ENET = 1<<7`).
- Add the ENET clock-root control + LPCG-gate `#define`s (mirror `semc.c`'s `CCM_CLOCK_ROOTn_CONTROL`
  / `CCM_LPCGnn_DIRECT` idiom). The `Enet1` root is index **51** (`kCLOCK_Root_Enet1`,
  `fsl_clock.h:852`) → `CCM_CLOCK_ROOT51_CONTROL` (mux=4 = SysPll1Div2, div=10 → 50 MHz); confirm the
  ENET LPCG gate number against the SDK before writing.
- Add `IRQ_ENET = 137` to `IRQ_NUMBER_t` (`core_pins.h`). Verify every offset/value against the cm7
  header (`MIMXRT1176_cm7_COMMON.h`) before writing.

### 2. `cores/imxrt1176/enet.c` — MAC bring-up
`void enet_init(const uint8_t mac[6])` (lazy; called from the sketch / future `begin()`):
1. **Clock:** route `Enet1` root from SysPLL1-Div2 ÷10 = 50 MHz; ensure SysPLL1-Div2 enabled; ungate
   the ENET LPCG. ⚠️ Per the SDRAM lesson ([[rt1176-sdram-semc]] — SYS_PLL2 + PFDs are boot-ROM-up,
   do **not** re-init), the plan first checks what SysPLL1 currently drives so enabling Div2 doesn't
   regress another peripheral; read the SDK `BOARD_InitModuleClock` for the exact `CLOCK_InitSysPll1`
   call and only add what's missing.
2. **Pins** (`IOMUXC_SetPinMux` + `SetPinConfig` written as direct register writes):
   | Signal | Pad | ALT | SION | pad ctl |
   |---|---|---|---|---|
   | MDC | GPIO_AD_32 | ENET_MDC | 0 | default |
   | MDIO | GPIO_AD_33 | ENET_MDIO | 0 | default |
   | TXD0/1, TX_EN | GPIO_DISP_B2_02/03/04 | ENET_TX_DATA00/01, TX_EN | 0 | 0x02 |
   | REF_CLK | GPIO_DISP_B2_05 | ENET_REF_CLK | **1** | 0x03 (slow slew) |
   | RXD0/1 | GPIO_DISP_B2_06/07 | ENET_RX_DATA00/01 | **1** | 0x06 |
   | RX_EN, RX_ER | GPIO_DISP_B2_08/09 | ENET_RX_EN, RX_ER | 0 | 0x06 |
3. `GPR4 |= ENET_REF_CLK_DIR` (drive 50 MHz out); `GPR28 &= ~CACHE_ENET` (ERR050396).
4. **PHY reset:** mux `GPIO_LPSR_12` → `GPIO12_IO12`; drive low, 10 ms, high, 150 ms settle.
5. **MAC config:** program `PALR/PAUR` (MAC); `MSCR` MDC divider (≤ 2.5 MHz off the MDIO/Bus-root
   clock); RMII / 100M / full-duplex (`RCR`/`TCR`); **clear `ECR.EN1588`** (legacy BDs) and **set
   `ECR.DBSWP`** (little-endian descriptors — silicon-correct *and* the QEMU checkpoint); program
   `RDSR/TDSR/MRBR`; set `ECR.ETHEREN`; write `RDAR` to arm RX.
- **Rings:** 4 TX + 4 RX legacy 8-byte BDs and 4 × 1518-byte buffers, all
  `DMAMEM __attribute__((aligned(64)))` → OCRAM. Never DTCM/ITCM.
- **Raw TX:** `int enet_send_frame(const uint8_t *frame, uint16_t len)` — free-BD spin (R clear) with
  timeout → `memcpy` into the BD buffer → set length + `L|TC|R` → memory barrier → kick `TDAR`. MAC
  appends the CRC.
- **Raw RX:** `int enet_read_frame(uint8_t *buf, uint16_t *len)` — return 0 if the RX BD is still
  DMA-owned (`E` set); else copy out, check error bits (drop on error), re-arm `E`, advance/wrap, kick
  `RDAR`.
- **MDIO:** `uint16_t enet_mdio_read(uint8_t phy, uint8_t reg)` / `void enet_mdio_write(...)` — write
  `MMFR` (`ST|OP|PA|RA|TA`), poll `EIR.MII`, bounded timeout. (Ported from FNET's `_fnet_fec_phy_*`.)

### 3. `cores/imxrt1176/enet.c` — PHY link (generic clause-22, PHY addr 3)
`int enet_phy_link_up(uint32_t timeout_ms)`: BMCR(reg0) = `0x8000` soft-reset → poll self-clear;
ANAR(reg4) = `0x01E1` (advertise 100F/100H/10F/10H + 802.3 selector); BMCR |= `0x1200` (autoneg +
restart); poll BMSR(reg1) link bit (0x0004), read **twice** (latching-low). Returns link + resolved
speed/duplex (decoded from BMCR). No vendor pages, no OUI gate.

### 4. ARP/ICMP responder (`enet.c` or a small `enet_responder.c`)
Static `ENET_MAC[6]`, `ENET_IP[4]`. `void enet_poll(void)` drains `enet_read_frame` and dispatches on
EtherType: `0x0806` → `handle_arp`, `0x0800` → `handle_ipv4`.
- **`handle_arp`:** ARP request (oper 1) with target-IP == `ENET_IP` → build reply in place (oper 2,
  sender↔target swapped, our MAC/IP as sender, Ethernet dst = requester), pad to 60 bytes, send.
- **`handle_ipv4`:** proto ICMP(1), ICMP type 8 (echo), dest == `ENET_IP` → swap Ethernet MACs, swap
  IP src/dst, ICMP type 8→0, recompute IP + ICMP checksums, send.
- **Checksum:** one RFC-1071 ones-complement `inet_checksum(ptr, len)`.

### 5. QEMU refinement — `phy-num` 2 → 3
`include/hw/arm/fsl-imxrt1170.h:173`: `FSL_IMXRT1170_ENET_PHY_NUM` 2 → 3, so the emulated PHY answers
MDIO at the RTL8201's real address. Rebuild `~/Development/qemu2/build` (`ninja qemu-system-arm`).
No device-model change. (Optional, **not** in v1: teach `lan9118_phy` to report the RTL8201 ID
`0x001CC816` — only needed if a future milestone adopts the OUI-validating `phyrtl8201` driver.)

## Data flow / behavior

Deliberately **symmetric** across QEMU and silicon so the gate predicts the board:
- **QEMU gate:** Python peer → socket netdev (4-byte BE length + raw frame) → `imx.enet` RX ring →
  `enet_read_frame` → responder → `enet_send_frame` → TX ring → socket netdev → peer asserts exact
  reply bytes.
- **Hardware:** host `ping <ENET_IP>` → RJ45 → RTL8201 → RMII → ENET RX ring → responder → TX → PHY →
  host sees the echo reply; `tcpdump` shows ARP-req → board ARP-reply → echo-req → board echo-reply.

Same code path, same frame bytes, both ends — what makes the gate a real predictor rather than a
circular check.

## Testing

**One QEMU gate harness `evkb/enet_test/`** (clone `usb_data_test/` — external backend + Python driver
+ `gate_tmp` + `gate-lib.sh` lifecycle; [[rt1170-gate-lib]]). Runner preamble unchanged
(`QEMU=…/tools/qrun`, `-M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel <elf>`,
`-serial file:$VCOM`, `-d guest_errors -D $DBG`; [[rt1170-qemu]]). The **new** ingredient no gate has
yet: a NIC backend — `-nic socket,listen=127.0.0.1:$PORT,model=imx.enet` + `enet_peer.py`, and
`-object filter-dump,id=d0,netdev=<id>,file=$DIR/frames.pcap` for TX capture. The runner greps VCOM
tokens **and** asserts on the peer's result file (like `usb_data` checks `$RES`). Tokens accumulate as
the three increments land:
- **Increment 1 (MAC):** peer injects a frame → `enet_read_frame` → `ENET_RX=PASS`; sketch
  `enet_send_frame` → pcap/peer confirms bytes → `ENET_TX=PASS`.
- **Increment 2 (PHY):** `enet_phy_link_up()` → `ENET_LINK=PASS`, **plus** read PHYID regs 2/3 →
  `ENET_PHYID=<hex>`. Assert PHYID ∉ {`0xffff`,`0x0000`} — the concrete defeat of the false-pass (a
  real register value only returns if the MDIO address, now 3, matches). QEMU returns the LAN9118 ID;
  silicon returns the Realtek `0x001C…` — a useful QEMU-vs-HW tell.
- **Increment 3 (responder):** peer sends an ARP request for `ENET_IP` and asserts the board's ARP
  reply, then an ICMP echo request and asserts the echo reply (type 0, valid checksum, payload
  echoed) → `ENET_ARP=PASS`, `ENET_PING=PASS`.

**★ Adding `enet.c` is a NEW core source → the CMake `file(GLOB)` trap applies** (no
`CONFIGURE_DEPENDS`): the gate dir needs `rm -rf build && cmake -B build -S . && cmake --build build`,
not an incremental rebuild — the same gotcha as the EEPROM new-file bring-up ([[rt1176-eeprom]]).
Editing `imxrt1176.h`/`startup.c`/`core_pins.h` in place does not.

**Hardware (final arbiter):** `pkill -9 LinkServer/redlinkserv`, then
`LinkServer run MIMXRT1176:MIMXRT1170-EVKB enet_test.elf`; read VCOM @115200 with pyserial
([[rt1170-evkb-flashing]], [[macos-serial-capture]]). RJ45 from the board's 10/100 ENET port
(**confirm the physical connector on this board — brief flags "J43 area"**) to a host/switch on the
`ENET_IP` subnet. Watch the link LED + VCOM (`ENET_LINK=PASS`, `ENET_PHYID=001C…`); from the host
`ping <ENET_IP>` → replies; `tcpdump`/Wireshark shows the ARP + ICMP exchange; `arp -a` shows the
board's MAC. **Success = host ping gets replies.**

## Risks

- **SD-card ↔ MDC pin conflict.** On EVKB **REVC**, `GPIO_AD_32` (ENET_MDC) is muxed with `SD1_CD_B`;
  if `R1926/R136` are populated and a card is inserted, MDIO to the PHY is blocked (SDK
  `example_board_readme.md`). Mitigation: first ENET HW test with **no card inserted**; check the board
  rev + resistors; verify whether the SdioTeensy driver even uses hardware card-detect (it likely
  polls — [[rt1176-sd-usdhc]]). Document as a board coexistence limitation if real.
- **PHY negotiation + MDIO/RMII timing are where QEMU can't be trusted.** The LAN9118 model always
  links and models no MDC/ref-clock timing, so a wrong `MSCR` MDC divider or `GPR4` ref-clock direction
  passes in QEMU yet fails on silicon. Trust the wire (link LED, `tcpdump`, the real PHYID) over the
  gate; if HW link fails where QEMU passed, look first at the MDC divider, the 50 MHz ref-clk-out
  (`GPR4`), and the RTL8201 reset timing.
- **`ECR.DBSWP` endianness** — reset default byte-swaps legacy BDs (assumes big-endian); a
  little-endian guest that doesn't set `DBSWP` has its rings misread and TX/RX silently fail. This one
  the gate **does** catch (wrong → Increment-1 gate fails), and it's silicon-correct too — but it's a
  classic silent-corruption trap, so it's called out explicitly.
- **Circular false-pass** — the gate is built to our own model, so it proves consistency, not
  correctness. HW is the arbiter for real PHY auto-negotiation, RMII clocking, and the actual ping. The
  phy-num 2→3 fix + the PHYID≠0xffff assertion are the specific guards against the model and firmware
  silently agreeing on the wrong thing.
- **Auto-generated header** — ENET defs must go in **both** `imxrt1176.h` and
  `tools/gen_imxrt1176_h.py`, or a regenerate drops them.
- **SysPLL1 re-init hazard** — do not blindly re-init SysPLL1; confirm what it already drives before
  enabling Div2 (the [[rt1176-sdram-semc]] SYS_PLL2 lesson).
- **Board connector + revision** — confirm the 10/100 RJ45 connector designator and the board rev on
  the bench before wiring.

## References

- **SDK (read first, before any register):**
  `_boards/evkbmimxrt1170/driver_examples/enet/txrx_transfer/{cm7/hardware_init.c,cm7/app.h,pin_mux.c}`
  + shared `driver_examples/enet/txrx_transfer/enet_txrx_transfer.c` (pins/clock/GPR4/GPR28/PHY-reset/
  rings/init order); `_boards/evkbmimxrt1170/lwip_examples/lwip_ping/` (the milestone-2 reference —
  note `BOARD_NETWORK_USE_100M_ENET_PORT` must be `1` to select the 10/100 port + RTL8201 + addr 0x03);
  `components/phy/device/phyrtl8201/fsl_phyrtl8201.c` (OUI check, why we hand-roll instead);
  `drivers/enet/fsl_enet.h` (API surface, considered-and-rejected base); cm7 header
  `MIMXRT1176_cm7_COMMON.h` (`ENET_BASE 0x40424000`, `ENET_IRQn 137`); `RT1170/periph/PERI_ENET.h`
  (register offsets) + `PERI_IOMUXC_GPR.h` (GPR4/GPR28 fields). Cross-check Zephyr
  `drivers/ethernet/eth_nxp_enet.c`, `drivers/phy/phy_mii.c`, `mimxrt1170_evk_mimxrt1176_cm7_B.overlay`.
- **Port reference:** `~/Development/NativeEthernet` (Arduino API, milestone 3); `~/Development/FNET`
  `src/port/netif/fec/fnet_fec.{c,h}` (register/BD/ring/MDIO to port),
  `src/port/cpu/mimxrt/fnet_mimxrt_eth.c` (1060 board layer to **rewrite** for 1176).
- **Core:** new `cores/imxrt1176/enet.c` (+ optional `enet.h`); `imxrt1176.h` +
  `tools/gen_imxrt1176_h.py` (no ENET today — extend both); `core_pins.h` (`IRQ_NUMBER_t`, add
  `IRQ_ENET`); `imxrt1176.ld` (`.dmabuffers`→OCRAM, unchanged); `semc.c` (the bring-up shape to mirror);
  `usb_serial.c` (the `DMAMEM aligned` bus-master idiom).
- **qemu2:** `include/hw/arm/fsl-imxrt1170.h:171-173` (the one-line phy-num fix); `hw/net/imx_fec.c`
  (`imx_phy_read` masking `:240-260`), `include/hw/net/imx_fec.h` (register enum/BD), `hw/net/lan9118_phy.c`
  (emulated PHY IDs); `hw/arm/fsl-imxrt1170.c:385, 897-917` (SoC wiring, unchanged).
- **Gate template:** `evkb/usb_data_test/` (external backend + Python driver + `gate_tmp`);
  `evkb/tools/{qrun,gate-lib.sh}`. Method / HW: gate-first TDD ([[rt1170-qemu]]), LinkServer + VCOM
  ([[rt1170-evkb-flashing]], [[macos-serial-capture]]), hybrid-port precedent ([[rt1176-spi-library-move]],
  [[rt1176-wire-library-move]]), DMA-buffer discipline ([[rt1176-sd-usdhc]], [[rt1176-edma-dmachannel]]),
  new-core-file GLOB reconfigure ([[rt1176-eeprom]]), circular-false-pass precedent
  ([[rt1176-gpio-irq-cm7-trap]]).
```
