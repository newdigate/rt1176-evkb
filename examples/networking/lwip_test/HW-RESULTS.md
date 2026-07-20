# lwIP milestone 2 — hardware verification results

**Date:** 2026-07-12 · **Board:** MIMXRT1176-EVKB · **PHY:** RTL8201 (10/100 RMII) · **Stack:** lwIP 2.2.1 (`NO_SYS`) over the frozen `cores/imxrt1176/enet.c`. **Result: ✅ full IP stack verified on real silicon — DHCP + ping + UDP echo + TCP echo.**

## Outcome (all four, one flash of `build/lwip_test.elf`)
- **DHCP** — the board pulled a **real lease from the home router**: VCOM shows `DHCP_OK ip=192.168.1.101` ~2 s after boot (a full DORA against the actual router, not QEMU SLIRP's 10.0.2.15). On HW the sketch takes the **DHCP-primary branch** and never reaches the 5 s static fallback.
- **Ping** (lwIP auto ARP+ICMP) — `ping -c 6 192.168.1.101` → **6/6 received, 0.0% loss, RTT min/avg/max = 1.09 / 3.02 / 8.11 ms**. `arp -n 192.168.1.101` → `2:0:0:0:0:1` (the sketch's `g_mac` `02:00:00:00:00:01` resolves on the wire).
- **UDP echo** (raw-API server, port 7) — `printf 'LWIP-UDP-…\n' | nc -u -w3 192.168.1.101 7` → the exact token echoed back → **UDP_ECHO=PASS**.
- **TCP echo** (raw-API server, port 7) — `printf 'LWIP-TCP-…\n' | nc -w3 192.168.1.101 7` → the exact token echoed back → **TCP_ECHO=PASS**.

VCOM boot sequence (`/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200):
```
LWIP_BOOT
LWIP_NETIF_UP
DHCP_OK ip=192.168.1.101
```

## Topology used
Board's **10/100 RJ45** → home router LAN port; host is a Mac on **Wi-Fi through the same router** (the router bridges Wi-Fi↔LAN into one L2 domain, so the Mac reaches the board's DHCP address directly — no Mac Ethernet needed). Same bench as the ENET v1 ping. (The 1G RJ45 is a different MAC/PHY — this firmware won't link on it.)

## What HW proved that the QEMU gate could not
The 5 QEMU phases were green, but each used a synthetic peer; the silicon run replaces every one with the real wire:
- **DHCP** — QEMU used SLIRP's built-in DHCP server (lease `10.0.2.15`). HW proves lwIP's DHCP client completes DORA against a **real consumer router** and installs a real lease/gw on the actual `192.168.1.0/24` subnet.
- **Ping** — QEMU injected raw ARP/ICMP from a Python socket-peer. HW proves real **ARP resolution + ICMP echo** over physical Ethernet from a stock `ping`, through the **RTL8201 PHY on the SysPll1 50 MHz RMII ref-clock** (the one HW-only blocker fixed in ENET v1 — see that milestone's HW-RESULTS).
- **UDP/TCP echo** — QEMU used SLIRP `hostfwd` to a loopback socket. HW proves real **UDP and TCP** segments over the wire to the board's raw-API echo servers on port 7.

No firmware change was needed for HW: the same gate-verified ELF runs, and the DHCP-first-with-static-fallback logic simply takes the DHCP branch on a real LAN. The core `enet.c` was untouched (frozen since ENET v1).

## Pre-flight checklist (for the next HW session)
1. Cable in the **10/100 RJ45** (not the 1G port).
2. **Remove the SD card** (EVKB REVC muxes `AD_32`=ENET_MDC with `SD1_CD_B`).
3. Your test LAN must have a **DHCP server** (any home router). The board self-assigns via DHCP; read the leased IP from VCOM (`DHCP_OK ip=…`).
4. `pkill -9 LinkServer redlinkserv`; start the VCOM reader (pyserial @115200, **not** `cat`) *before* the flash; `LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/lwip_test.elf`.
5. From a host on that LAN: `ping <lease>`; `printf 'x\n' | nc -u -w3 <lease> 7` (UDP echo); `printf 'x\n' | nc -w3 <lease> 7` (TCP echo).
