# RT1176 Ethernet milestone 4 — FNET + NativeEthernet Arduino `Ethernet` API (design)

**Date:** 2026-07-12
**Status:** approved design → ready for implementation plan
**Builds on:** milestone 1 (`cores/imxrt1176/enet.c` — FEC MAC + RTL8201 PHY, HW-verified), milestones 2–3 (lwIP + `newdigate/Ethernet`, HW-verified — the API contract and gate harness to mirror).

## Goal

Bring up the Arduino `Ethernet` socket API (`EthernetClient`/`EthernetServer`/`EthernetUDP` + `EthernetClass`, DHCP + static, DNS) on the MIMXRT1176-EVKB over **FNET** (Apache-2.0 TCP/IP stack) + **NativeEthernet** (MIT Arduino wrapper over FNET BSD sockets) — a more-permissively-licensed alternative to the shipped lwIP-based `newdigate/Ethernet` (lwIP BSD-3). End state = feature parity with milestone 3: 5 QEMU gates green (boot/server/udp/client/dns) **and** HW-verified on the RT1170-EVKB.

## Motivation

License-driven: this stack gives an MIT top layer over an Apache-2.0 stack, alongside the existing lwIP-BSD option. Functional end state is deliberately identical to milestone 3.

## Architecture (summary)

```
sketch → NativeEthernet (MIT; Wiznet-style socket array, IntervalTimer 1 kHz fnet_poll)
       → FNET BSD sockets → FNET core (TCP/UDP/IPv4/ARP + DHCP-client + DNS services)
       → fnet_fec.c generic FEC datapath (legacy BDs, sw byte-swap, interrupt-driven RX)
       → ENET @ 0x40424000 (IRQ 137 / vector 153) ↔ RTL8201 PHY @ MDIO addr 3
```

- **Timebase:** `FNET_CFG_TIMER_ALT=1` — no HW timer; app supplies `timer_get_ms = millis()`. NativeEthernet already wires this.
- **Service pump:** NativeEthernet starts an `IntervalTimer` calling `fnet_poll()` at 1 kHz (Teensy-4.1-proven model; our PIT-backed IntervalTimer is HW-verified). RX additionally arrives via the ENET ISR, installed by FNET through `SCB->VTOR` into the core's RAM vector table (`_VectorsRam` — same mechanism as every core driver).
- **DHCP:** driven by FNET's link-up callback inside NativeEthernet; `begin(mac,timeout)` busy-waits (bounded) for the lease. **DNS:** NativeEthernet's `DNSClient` over FNET's DNS service.

## Approach decision (driver strategy)

**Chosen — A: native FNET port.** FNET's mimxrt "driver" is ~200 lines of board hooks (`eth_cpu_init`, `eth_cpu_phy_init`) over the shared, platform-generic `src/port/netif/fec/fnet_fec.c` MAC driver. Our `enet.c` datapath (legacy BDs, software byte-swap, ECR.DBSWP clear) was ported *from* `fnet_fec.c` in milestone 1 and HW-verified — so FNET's datapath conventions are already proven on this silicon and against QEMU's `imx.enet` model. We keep `fnet_fec.c` untouched-in-architecture, add RT1176 config plumbing, and write the RT1176 board hooks by **transplanting the HW-verified bring-up logic from `enet.c`** (self-contained — the FNET fork must not include core headers). `enet.c` stays frozen; the FNET fork remains publishable/standalone for any RT1176 user.

**Rejected:**
- **B — glue FNET onto `enet.c` raw TX/RX.** Requires a custom driver against FNET *private* internals (`fnet_netif_api_t`, input/ISR-lock semantics — not a designed seam like lwIP's `ethernetif`), bypasses the FEC driver that is the point of "native FNET", and couples the public fork to our core. The proven-ness advantage is illusory: `enet.c`'s datapath *is* FNET's.
- **C — A but calling init helpers exported from `enet.c`.** Zero duplication, but unfreezes `enet.c` (helpers are statics), couples the fork to our core, splits board-truth across repos.

**Also chosen:** Arduino base classes (`Client.h`, `Server.h`, `Udp.h`, `IPAddress.{h,cpp}`) are **vendored into NativeEthernet** (copied from `~/Development/Ethernet/src`, which vendored them from cores/teensy4; headers preserved). The two Ethernet libs can never link together (both define `EthernetClient` etc.), so duplication is harmless. Promoting them into the core was rejected for this milestone (touches the shipped lib + core simultaneously).

## Scope

**In:** `EthernetClient` (connect by IP + by hostname, write/read/peek/flush/stop/connected), `EthernetServer` (begin/available/accept/write), `EthernetUDP` (begin/beginPacket ×2/endPacket/parsePacket/read/…), `EthernetClass` (`begin(mac[,timeout[,responseTimeout]])` DHCP, `begin(mac, ip[, dns[, gateway[, subnet]]])` static, `maintain`, `linkStatus`, addresses/setters), DNS via `DNSClient`.

**Deferred:** multicast (`beginMulticast`), mDNS (`EthernetMDNS` — needs multicast), TLS (`FNET_CFG_TLS=0`), 1588/adjustable-timer timestamps, MAC-from-OCOTP-fuses helper (gates pass an explicit locally-administered MAC `02:00:00:00:00:01`), HW checksum offload (see Component 1).

## Component 1 — FNET fork (`~/Development/FNET`, the real work)

### Config plumbing (new `FNET_CFG_CPU_MIMXRT1176` target)
- `src/fnet_config.h` — extend the board→CPU select: `defined(__IMXRT1176__)` (defined by our core/toolchain) → `FNET_MIMXRT (1)` + `FNET_CFG_CPU_MIMXRT1176 (1)`.
- `src/port/cpu/fnet_cpu_config.h` — declare `FNET_CFG_CPU_MIMXRT1176` (default 0); add the include block → `fnet_mimxrt1176_config.h`, `FNET_CPU_STR "MIMXRT1176"`.
- **New** `src/port/cpu/mimxrt/fnet_mimxrt1176_config.h` — clone of the 1062 file with: `FNET_CFG_CPU_ETH0_PHY_ADDR = 3` (RTL8201, HW-verified), clock constants for the MSCR/MDC divider (bus root 240 MHz → MDC ≤ 2.5 MHz; `enet.c` uses divider value 47 — FNET's formula must land on the equivalent), RX/TX buffer counts (start with 1062 defaults), **all HW checksum-offload flags = 0** (`FNET_CFG_CPU_ETH_HW_TX_IP_CHECKSUM` etc.): QEMU's FEC model does not implement TX checksum insertion, so offloaded (zeroed) checksums would be silently dropped by SLIRP and the gates could never pass. Software checksums everywhere; offload is a later HW-only experiment.
- `src/port/cpu/mimxrt/fnet_mimxrt.h` — per-CPU `FNET_FEC0_BASE_ADDR`: `0x40424000` for 1176 (the 10/100 ENET; ENET_1G/ENET_QOS are out of scope). Keep `0x402D8000` for 1062.
- `src/port/cpu/mimxrt/fnet_mimxrt_config.h` — per-CPU `FNET_CFG_CPU_ETH0_VECTOR_NUMBER`: **153** (= 16 + IRQ 137) for 1176.
- `src/port/netif/fec/fnet_fec.h` + `fnet_fec.c` — extend every `FNET_CFG_CPU_MIMXRT1052 || FNET_CFG_CPU_MIMXRT1062` guard with `|| FNET_CFG_CPU_MIMXRT1176` (register-map reserved-word layout and MSCR formula; the RT1176 10/100 ENET is the same FEC IP — layout verified against our generated `imxrt1176.h` during implementation). Same for the two guards in `fnet_mimxrt_eth.c`.
- Buffer placement: `FNET_AT_NONCACHEABLE_SECTION` must resolve to the core's OCRAM/DMAMEM section — override FNET's section-name config (default `"NonCacheable"`) to **`.dmabuffers`** (the core's `DMAMEM` section: OCRAM, zero-initialized by startup, `imxrt1176.h:840` / `imxrt1176.ld:84-87`). Rationale: D-cache is **off** in this core (so OCRAM is coherent without cache maintenance, which `fnet_fec.c` never does), and erratum ERR050396 forbids ENET DMA into TCM regardless.

### Board hooks — new `#if FNET_CFG_CPU_MIMXRT1176` branch in `fnet_mimxrt_eth.c`
Transplanted (logic-identical) from HW-verified `enet.c`, with register #defines carried self-contained in the FNET port:
- `eth_cpu_init`: SysPll1 bring-up via the ANATOP AI protocol (`SYS_PLL1_CTRL 0x4000 → 0x62002000`; LDO enable → bypass → divider 41 → power-up → STABLE lock → clk-out → ungate → /2 tap → bypass off; **all AI polls bounded** so QEMU — which lacks an AI model — proceeds); ENET1 clock root 51 → SysPll1Div2 ÷ 10 = 50 MHz + LPCG112 ungate; RMII pin mux (MDC/MDIO `AD_32/33` ALT3, TXD0/1/TX_EN `DISP_B2_02/03/04`, REF_CLK `DISP_B2_05` ALT2+SION, RXD0/1 `DISP_B2_06/07` +SION, RX_EN/ER `DISP_B2_08/09`, the 6 `ENET_*_SELECT_INPUT` daisy regs, `GPR4[ENET_REF_CLK_DIR]=1` out, `GPR28[CACHE_ENET]` clear); PHY hardware reset on GPIO12_IO12 (`GPIO_LPSR_12` ALT 0xA, ≥10 ms low / ≥150 ms release).
- `eth_cpu_phy_init`: mirror the proven generic clause-22 sequence — ANAR `0x01E1` (advertise 100F/H+10F/H) + autoneg restart. No RTL8201 vendor registers needed (HW-verified in milestone 1; Zephyr uses the generic driver too).
- `fnet_netif_t`/`fnet_eth_if_t` instances: same shape as the 1062 ones (`fnet_fec_api`, `fnet_fec0_if`, `eth_output = fnet_fec_output`).

### What is *new risk* vs milestone 1 (gate-first targets)
1. **Interrupt-driven RX** — first use of the ENET interrupt on this board and in our QEMU machine (milestone 1 masked all MAC IRQs). QEMU's `imx.enet` model does raise interrupts (Linux guests rely on it); the boot gate exercises this immediately.
2. MSCR/MDC divider via FNET's formula (wrong value ⇒ MDIO dead ⇒ no link).
3. FEC register-layout guard correctness for 1176.

## Component 2 — NativeEthernet fork (`~/Development/NativeEthernet`, minimal)

- Vendor `Client.h`, `Server.h`, `Udp.h`, `IPAddress.h`, `IPAddress.cpp` into `src/` (byte-copied from `~/Development/Ethernet/src`; upstream license headers preserved).
- Target: **zero changes to existing `.cpp` logic.** All board specifics live in FNET. If a change proves unavoidable, it must be `#if`-guarded and justified in the plan.
- Core-surface dependencies (all verified present in `cores/imxrt1176`): `IntervalTimer`, `millis`/`delay`, `elapsedMillis`, `DMAMEM`, `Serial.send_now()` (usb_serial.h:137), weak `yield`.
- `library.json` `"platforms": "teensy"` may be widened (cosmetic; Arduino/CMake builds ignore it).
- README: note the RT1176 target and record the mixed-license inventory (see Licensing).

## Component 3 — QEMU gate harness (`evkb/native_ethernet_test/`, new)

Clone of `ethernet_test/` conventions:
- **Sketch** `native_ethernet_test.cpp`: `ETH_BOOT` → `Ethernet.begin(mac, 15000)` (DHCP) → `ETH_DHCP ok=… ip=…` → `server.begin()` (port 7) + `udp.begin(7)` → `ETH_NETIF_UP`; `loop()`: TCP echo via `server.available()`, UDP echo via `parsePacket`, one-shot outbound client (`connect(10.0.2.100, 7)`, send `ETHCLI-PROBE`, assert echo → `CLIENT_ECHO=PASS`), one-shot DNS (`DNSClient::getHostByName("example.com")` → `DNS_OK ip=…`). Markers identical to milestone 3 so the runner logic clones over. No `Ethernet.loop()` exists/is needed — the IntervalTimer pumps FNET.
- **Runner** `run_qemu_native_ethernet.sh`: phases **boot / server / udp / client / dns**; `PHASE` exported **before** `gate_init` (gate-lib re-exec drops argv); `-nic "$NICVAL"` as one quoted arg (the client phase's `guestfwd …-cmd:python3 …` contains a space); per-phase NIC values copied from milestone 3 (`hostfwd=tcp::5555-:7`, `hostfwd=udp::5556-:7`, `guestfwd=tcp:10.0.2.100:7-cmd:…`, plain SLIRP for boot/dns; DNS server = 10.0.2.3). Peers `ethernet_peer.py` + `guestfwd_echo.py` copied in.
- **CMakeLists.txt**: `import_arduino_library(cores …/cores/imxrt1176)`; `import_arduino_library(fnet $ENV{HOME}/Development/FNET <enumerated subdirs>)` — flat non-recursive globs, so every FNET source dir is listed explicitly (`src`, `src/stack`, `src/service` and its needed service subdirs — at minimum `dhcp`, `dns`, plus whatever `fnet_service.c`'s own dir requires — `src/port`, `src/port/compiler`, `src/port/cpu`, `src/port/cpu/mimxrt`, `src/port/netif/fec`; the exact list is enumerated in the plan after checking which dirs exist — every FNET file is preprocessor-gated, so over-inclusion only costs compile time, but a listed-but-missing dir is a fatal CMake error); `import_arduino_library(nativeethernet $ENV{HOME}/Development/NativeEthernet src src/utility)`. The lwIP `Ethernet` lib is **not** imported (class-name collision by design). Fresh `rm -rf build` on first configure (new-library import rule).
- QEMU model reuse: `imx.enet` (base 0x40424000, IRQ 137, `FSL_IMXRT1170_ENET_PHY_NUM=3` already patched in milestone 1). Expected model delta: none; if the interrupt path exposes a model gap, fix `qemu2` (as with PHY_NUM).

## Error handling

- `Ethernet.begin(mac, timeout)` returns 0 on DHCP timeout (bounded busy-wait; the 1 kHz ISR keeps `fnet_poll` running). Static fallback stays **sketch-level**, parity with milestone 3.
- All transplanted AI-protocol polls bounded (QEMU proceeds without an AI model, exactly like `enet.c`).
- FNET heap (64 KB default) + per-socket buffers (8 × 2 KB × 2) come from `new[]` on the OCRAM heap — sized well within our free OCRAM; failure is a visible allocation fault, not silent.
- Gates run under `qrun` (gtimeout + 100 MB log cap) + `gate-lib.sh` teardown; every phase asserts explicit `…=PASS` / `DNS_OK` / `ETH_NETIF_UP` markers.

## Testing (gate-first TDD; HW is the arbiter)

1. **QEMU gates, in order:** boot (FNET init, ISR install, DHCP lease `10.0.2.15` over SLIRP) → server → udp → client → dns. Each written to fail first (no FNET 1176 target yet), then made green.
2. **HW verification on the RT1170-EVKB:** LinkServer flash (`MIMXRT1176:MIMXRT1170-EVKB`), VCOM via pyserial @115200 (reader started before flash), 10/100 RJ45 to the router, **SD card removed** (AD_32 = MDC muxes with SD1_CD_B). Checklist: real DHCP lease, ping from the Mac, `EthernetServer` TCP echo :7, `EthernetUDP` echo :7, DNS resolve, outbound `EthernetClient` HTTP GET to example.com:80 (Mac firewall blocks inbound to a Mac peer). Results recorded in `native_ethernet_test/HW-RESULTS.md` + a project memory note.
3. Independent re-run of each gate by the coordinating session after subagent implementation (two-stage review per the superpowers workflow).

## Licensing (record accurately)

- **FNET**: Apache-2.0 (`fnet_license.txt` retained). Add a `NOTICE` documenting the RT1176 port modifications (© attribution, date); all new/modified files carry Apache-2.0 headers.
- **NativeEthernet**: MIT © 2020 Tino Hernandez (top-level LICENSE preserved). Known mixed-license contents to record in the README, not alter: `NativeDns.{h,cpp}` = Apache-2.0 (MCQN Ltd), `src/utility/NativeW5100.h` = GPLv2/LGPLv2.1 (constants-only header, upstream lineage), several files © Paul Stoffregen / Bjoern Hartmann (MIT-style). Vendored base classes keep their upstream headers.
- Both repos already have `newdigate` remotes; **push only when asked**.

## Repo / commit discipline

- `~/Development/FNET`, `~/Development/NativeEthernet`: commit to `master`; `git add -A` acceptable.
- `evkb`: stage **only** `native_ethernet_test/` paths + this spec/plan (never `git add -A`; shared tree, nested `cores/` repo).
- `cores/imxrt1176`: **untouched this milestone** — `enet.c` frozen, no core changes needed (verified: base classes vendored, all NativeEthernet core dependencies already exist).
- Build: `-DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake`; gates via `./run_qemu_native_ethernet.sh <phase>`.

## Definition of done

FNET + NativeEthernet `Ethernet` API on the RT1176: 5 QEMU gates green (boot/server/udp/client/dns) **and** HW-verified on the RT1170-EVKB (router DHCP lease, ping, TCP+UDP echo on :7, DNS resolve, outbound `EthernetClient` GET) — parity with `newdigate/Ethernet` — recorded in `HW-RESULTS.md` and a memory note. Push only on request.
