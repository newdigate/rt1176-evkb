# Ethernet milestone 3 (Arduino socket API) — hardware verification results

**Date:** 2026-07-12 · **Board:** MIMXRT1176-EVKB · **PHY:** RTL8201 (10/100 RMII) · **Library:** `~/Development/Ethernet` (EthernetClient/Server/UDP + EthernetClass over lwIP 2.2.1). **Result: ✅ the full Arduino `Ethernet` socket API verified on real silicon — DHCP + ping + TCP server + UDP + DNS + outbound TCP client.**

## Outcome (one flash of `build/ethernet_test.elf`, lease `192.168.1.101` from the router)
- **DHCP** — `Ethernet.begin(mac)` pulled a real router lease: VCOM `ETH_DHCP ok=1 ip=192.168.1.101`, `ETH_NETIF_UP`.
- **Ping** (lwIP auto ARP+ICMP) — `ping -c 5 192.168.1.101` → **5/5, 0.0% loss, RTT min/avg/max = 1.16 / 1.65 / 1.94 ms**.
- **EthernetServer** echo (port 7) — from the Mac `printf 'HWTCP-…\n' | nc -w3 192.168.1.101 7` → the exact token echoed back.
- **EthernetUDP** echo (port 7) — `printf 'HWUDP-…\n' | nc -u -w3 192.168.1.101 7` → the exact token echoed back.
- **DNS** — the sketch resolved `example.com` via the **router's DNS**: VCOM `DNS_OK ip=104.20.23.154`.
- **EthernetClient (outbound, WebClient)** — `c.connect("example.com", 80)` (hostname → DNS + TCP through the router's NAT), HTTP `GET /`, read the response: VCOM `CLIENT_OUT=PASS got="HTTP/1.1 200 OK"`. This exercises the DNS-integrated `connect(const char*, port)` + `write` + `read` end-to-end against a real internet server.

## Topology used
Board's **10/100 RJ45** → home router LAN port; this Mac on **Wi-Fi through the same router** (one L2 domain, so Mac↔board is direct). Same bench as ENET v1 / lwIP milestone 2. (The 1G RJ45 is a different MAC/PHY — this firmware won't link on it.)

## Note on the outbound-client test (why HTTP GET, not the echo)
The committed sketch's `try_client_once` targets `10.0.2.100:7` — a QEMU-SLIRP-`guestfwd` address used **only** by the `client` QEMU gate (unreachable on a real LAN). For the hardware run it was retargeted two ways: first to this Mac's LAN IP running a TCP echo, which **failed because the macOS application firewall (State = 1) silently dropped the inbound SYN** (the board's outbound connect was fine — the Mac never accepted it). Rather than change the Mac's security settings, the probe was retargeted to `connect("example.com", 80)` + an HTTP GET, which connects **outbound through the router's NAT** (no inbound-to-Mac, firewall-independent) and additionally exercises the DNS-integrated hostname path. The HW-test sketch edit was reverted after the run; the committed sketch remains the QEMU-gate version (all 5 gates re-verified green after the revert).

## What HW proved that the QEMU gates could not
- **DHCP** — QEMU used SLIRP's DHCP (10.0.2.15); HW proves `Ethernet.begin(mac)` leases from a **real consumer router** on the real subnet.
- **DNS** — QEMU used SLIRP's 10.0.2.3 resolver; HW proves resolution through the **router's real DNS** to a live public IP.
- **TCP server / UDP** — QEMU used SLIRP `hostfwd` to loopback; HW proves real TCP/UDP over physical Ethernet to the board's `EthernetServer`/`EthernetUDP` on port 7.
- **TCP client** — QEMU used SLIRP `guestfwd` -cmd echo; HW proves a real **outbound** connection (DNS + TCP + HTTP) to a real internet host through the router.
- All through the RTL8201 PHY on the SysPll1 50 MHz RMII ref-clock (the one HW-only blocker fixed back in ENET v1). No firmware change vs the gate build; the core `enet.c` is frozen.

## Pre-flight checklist (for the next HW session)
1. Cable in the **10/100 RJ45** (not the 1G port).
2. **Remove the SD card** (EVKB REVC muxes `AD_32`=ENET_MDC with `SD1_CD_B`).
3. Your test LAN needs a **DHCP server** (any router); read the leased IP from VCOM (`ETH_DHCP ok=1 ip=…`).
4. `pkill -9 LinkServer redlinkserv`; start the pyserial VCOM reader (@115200, **not** `cat`) *before* the flash; `LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/ethernet_test.elf`.
5. From a LAN host: `ping <lease>`; `printf 'x\n' | nc -w3 <lease> 7` (server); `printf 'x\n' | nc -u -w3 <lease> 7` (UDP). VCOM shows `DNS_OK ip=…`. For the outbound-client check, retarget `try_client_once` to a reachable server (a LAN echo — mind the host firewall — or `connect("example.com",80)`+HTTP GET) and reflash; revert before committing.
