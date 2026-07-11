# ENET 10/100 — hardware verification results

**Date:** 2026-07-11 · **Board:** MIMXRT1176-EVKB · **PHY:** RTL8201 (10/100 RMII) · **Result: ✅ board answers ping on real silicon.**

## Outcome
- `ping 192.168.1.50` from a host on the same LAN → **replies, ~2 ms RTT, 100% steady-state** (the only drops are the first few packets during the board's boot window).
- `arp -n 192.168.1.50` → `2:0:0:0:0:1` — the board's ARP reply resolves.
- VCOM: `ENET_BOOT`, `ENET_INIT_DONE`, `ENET_PHYID=1C:C816` (**real RTL8201** — vs QEMU's LAN9118 `07:C0D1`), `ENET_LINK=PASS`, `ENET_PHYID_OK=PASS`, `ENET_PING=PASS` (per echo).

## Topology used
Board's **10/100 RJ45** → home router LAN port; host is a Mac on **Wi-Fi through the same router** (no Mac Ethernet needed — the router bridges Wi-Fi↔LAN into one L2 domain). The responder's static IP (`192.168.1.50`) is a free address on the router's `192.168.1.0/24` subnet, so the host reaches it directly. (The 1G RJ45 is a *different* MAC/PHY — deferred; this firmware won't link on it.)

## The one HW-only blocker found + fixed (the QEMU gate could not see it)
`ENET_RXSTATS ok=0 drop=0` (an RX-boundary counter) showed the MAC receiving **zero** frames — which **ruled out the cable** (a bad cable gives `drop>0`). A register read then found **`SYS_PLL1_CTRL=0x4000`**: the 1 GHz **SysPll1 was entirely powered down** (the boot ROM brings up SYS_PLL2 but not SysPll1). ENET's 50 MHz RMII ref-clock is `SysPll1Div2 ÷10`, so with SysPll1 down there was no ref-clock — MDIO + link worked (IPG clock) but RMII RX/TX were dead.

**Fix (in `cores/imxrt1176/enet.c`, `enet_clock_init`):** the full SysPll1 bring-up via the analog-interface (AI) protocol, ported exactly from the SDK `CLOCK_InitSysPll1({.pllDiv2En=true})` and verified vs `fsl_clock.c`/`fsl_anatop_ai.c`. `SYS_PLL1_CTRL: 0x4000 → 0x62002000` (locked + Div2), RX came alive, ping answered.

## Pre-flight checklist (for the next HW session)
1. Cable in the **10/100 RJ45** (not the 1G port).
2. **Remove the SD card** (EVKB REVC muxes `AD_32`=ENET_MDC with `SD1_CD_B`).
3. Set the sketch's `ENET_IP` to a free address on your test LAN's subnet.
4. `pkill -9 LinkServer redlinkserv`; `LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/enet_test.elf`; read VCOM `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200 with pyserial.
5. From a host on that subnet: `ping <ENET_IP>` → expect replies.
