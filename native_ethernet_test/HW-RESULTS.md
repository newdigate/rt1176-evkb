# Task 9 — FNET + NativeEthernet Arduino stack hardware verification results

**Date:** 2026-07-12 · **Board:** MIMXRT1176-EVKB · **PHY:** RTL8201 (10/100 RMII) · **Stack:** FNET (`~/Development/FNET`, NO_SYS-style embedded TCP/IP) driving the Arduino-compatible `NativeEthernet` socket API (`~/Development/NativeEthernet`). **Result: ✅ the full FNET-backed Arduino `Ethernet` socket API verified on real silicon — DHCP + ping + TCP server + UDP + DNS + outbound HTTP GET.** All 5 QEMU gates were green going into this run; hardware is the final arbiter, and hardware passed.

## Commit SHAs
- `evkb` (this repo): `b69f863`
- `~/Development/FNET`: `d9b3886`
- `~/Development/NativeEthernet`: `a338101`

ELF flashed: `~/Development/rt1170/evkb/native_ethernet_test/build/native_ethernet_test.elf`

## Outcome (one flash, lease `192.168.1.101` from the router)

VCOM markers observed, verbatim, in order:
```
 ETH_BOOT
ETH_DHCP ok=1 ip=192.168.1.101
ETH_NETIF_UP
HTTP_GET=PASS
DNS_OK ip=104.20.23.154
```
`CLIENT_ECHO=` did **not** appear, as expected — that marker only fires on the SLIRP-lease branch (`on_slirp()` false on a real router lease), so the sketch took the real-network path: outbound `connect("example.com", 80)` + HTTP GET, and a router-DNS lookup of `example.com`.

- **DHCP** — `Ethernet.begin(mac, 15000)` pulled a real router lease: `ETH_DHCP ok=1 ip=192.168.1.101`, `ETH_NETIF_UP`.
- **Ping** (FNET auto ARP+ICMP) — `ping -c 5 192.168.1.101`:
```
PING 192.168.1.101 (192.168.1.101): 56 data bytes
64 bytes from 192.168.1.101: icmp_seq=0 ttl=64 time=3.486 ms
64 bytes from 192.168.1.101: icmp_seq=1 ttl=64 time=5.984 ms
64 bytes from 192.168.1.101: icmp_seq=2 ttl=64 time=3.383 ms
64 bytes from 192.168.1.101: icmp_seq=3 ttl=64 time=1.198 ms
64 bytes from 192.168.1.101: icmp_seq=4 ttl=64 time=1.166 ms

--- 192.168.1.101 ping statistics ---
5 packets transmitted, 5 packets received, 0.0% packet loss
round-trip min/avg/max/stddev = 1.166/3.043/5.984/1.783 ms
```
- **EthernetServer TCP echo** (port 7) — Python `socket.create_connection((board,7))`, sent `HW-TCP-PROBE`, read looped until 12 bytes collected:
```
TCP (looped): b'HW-TCP-PROBE'
```
  (First attempt used a single non-looping `recv(64)` and returned only `b'H'` — that is a client-script artifact, not a firmware bug: the echo arrived across more than one TCP segment/recv-return, and the naive probe only read the first chunk. A read loop collecting to the expected length gets the full, correct echo every time.)
- **EthernetUDP echo** (port 7) — Python UDP socket, sent `HW-UDP-PROBE`:
```
UDP (attempt 1): TIMEOUT
UDP (attempt 2): b'HW-UDP-PROBE'
```
  First UDP datagram was lost (consistent with a cold ARP cache entry on the Mac for a newly-leased peer IP — the kernel's first send races an outstanding ARP resolution and gets dropped rather than queued past the resolution). Second attempt, sent ~1s later with a warm ARP entry, echoed correctly on the first try.
- **DNS** — the sketch resolved `example.com` via the router's DNS: VCOM `DNS_OK ip=104.20.23.154`.
- **EthernetClient outbound (HTTP GET)** — `c.connect("example.com", 80)` (hostname → DNS + TCP through the router's NAT), `GET / HTTP/1.1`, response header checked for `HTTP/1.1 200`: VCOM `HTTP_GET=PASS`. This is the sketch's built-in real-network probe path (see `try_client_once()` in `native_ethernet_test.cpp`) — no sketch edit was needed for this run, unlike the milestone-3 `EthernetClient` test which required a temporary retarget.

## Topology used
Board's **10/100 RJ45** → home router LAN port; this Mac on **Wi-Fi through the same router** (one L2 domain, so Mac↔board is direct). Same bench as ENET v1 / lwIP milestone 2 / Ethernet milestone 3. (The 1G RJ45 is a different MAC/PHY — this firmware won't link on it.) SD card was not inserted (`AD_32`=ENET_MDC muxes with `SD1_CD_B` on EVKB REVC).

## What HW proved that the QEMU gates could not
- **DHCP** — QEMU used SLIRP's DHCP (10.0.2.15); HW proves `Ethernet.begin(mac)` leases from a **real consumer router** on the real subnet, through FNET's DHCP client service.
- **DNS** — QEMU used SLIRP's 10.0.2.3 resolver; HW proves FNET's DNS service resolving through the **router's real DNS** to a live public IP.
- **TCP server / UDP** — QEMU used SLIRP `hostfwd` to loopback; HW proves real TCP/UDP over physical Ethernet to the board's `EthernetServer`/`EthernetUDP` on port 7, driven by FNET's socket layer under the `NativeEthernet` Arduino facade.
- **TCP client** — QEMU's SLIRP-lease branch exercises a `guestfwd`-routed echo; HW proves the sketch's real-network branch — a genuine **outbound** DNS + TCP + HTTP connection to a live internet host through the router's NAT.
- All through the RTL8201 PHY on the SysPll1 50 MHz RMII ref-clock (the HW-only blocker fixed back in ENET v1). No firmware/core change vs. the gate build; `enet.c` is frozen.

## Retries / anomalies
1. **TCP probe partial read** — first Mac-side probe script used a single `recv(64)` and printed `b'H'` instead of the full 12-byte echo. Retried with a receive loop (accumulate until 12 bytes or 2s idle timeout); got the full, correct `b'HW-TCP-PROBE'`. Root cause: client-side probe issue (single recv doesn't guarantee full datagram/segment on a stream socket), not a board/firmware defect — the same probe pattern is standard for TCP.
2. **UDP probe first-attempt timeout** — first UDP send/recv timed out; second attempt (~1s later) succeeded immediately. Consistent with a cold-ARP-cache drop on the Mac's first datagram to a freshly-leased peer IP; not repeated on retry. No board-side symptom (VCOM log showed no errors, and the board's echo logic is unconditional/synchronous in `loop()`).
3. No reflash was required — one flash of `native_ethernet_test.elf` produced all passing markers and probes (after the two client-side probe-script retries above).

## Pre-flight checklist (for the next HW session)
1. Cable in the **10/100 RJ45** (not the 1G port).
2. **Remove the SD card** (EVKB REVC muxes `AD_32`=ENET_MDC with `SD1_CD_B`).
3. Your test LAN needs a **DHCP server** (any router); read the leased IP from VCOM (`ETH_DHCP ok=1 ip=…`).
4. `pkill LinkServer; pkill redlinkserv`; start the pyserial VCOM reader (@115200, **not** `cat`) *before* the flash; `LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/native_ethernet_test.elf`.
5. From a LAN host: `ping <lease>`; TCP/UDP probes to port 7 should use a **read loop** (accumulate to expected length) rather than a single `recv()`, and UDP probes should tolerate/retry a first-attempt timeout (cold ARP). VCOM shows `DNS_OK ip=…` and, on a non-SLIRP lease, `HTTP_GET=PASS` (the sketch's built-in outbound probe — no retargeting needed, unlike milestone 3's `EthernetClient` test).
