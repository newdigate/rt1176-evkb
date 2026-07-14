# License clean-room — HW verification results (2026-07-14)

Board: MIMXRT1170-EVKB via LinkServer, VCOM @115200. All QEMU gates (28) and
`tools/license-audit.sh` green before flashing.

## rtc_test — PASS (the one swap with HW-visible risk: Time.cpp)

First boot (phase 1):

```
TIME_ALL=PASS
phase1 r0=1700000000 dsec=1 dus=1999706
RTC_SETGET=PASS
RTC_TICK=PASS
phase1 OK -> SYSRESETREQ
```

Second `LinkServer run` (SYSRESETREQ halts under the attached probe, as
documented — a fresh run boots phase 2):

```
TIME_ALL=PASS
phase2 now=1700000080 (KNOWN+80s)
RTC_PERSIST=PASS
RTC_ALL=PASS
```

`TIME_ALL=PASS` on both boots = the clean-room `Time.cpp` (9 civil-time
vectors incl. 2000-02-29, 2100 non-leap, 0xFFFFFFFF + 500-value LCG
round-trip) verified on silicon.

## ethernet_test — IPAddress verified; network blocked on physical link

```
ETH_BOOT
IPADDR=OK          <- clean-room IPAddress.cpp unit checks on silicon
ETH_DHCP ok=0 ip=0.0.0.0
ETH_NETIF_UP
```

`IPADDR=OK` covers the clean-room IPAddress.cpp (byte order, fromString,
printTo, operators). DHCP got no lease — **RJ45 not connected** at time of
test. PENDING: re-run with cable attached (expect lease + DNS_OK; the
sketch's serial output itself exercises Print/String/Stream on HW).

## sd_wav_play_test — blocked on physical card

```
SD_WAV_MOUNT=FAIL
```

**No SD card inserted** (removed for earlier Ethernet HW work — AD_32
MDC/SD1_CD_B conflict). Not a regression: all three SD QEMU gates
(sd_wav_play_test, SdFat sd_block_test, sd_fs_test) pass, and the SdFat
surgery only deleted never-compiled GPL files (soft-SPI path; USDHC
untouched). PENDING: insert card, re-run (expect MOUNT/PLAY/DONE=PASS).

## Coverage summary

| clean-room file | HW evidence |
|---|---|
| Time.cpp | TIME_ALL=PASS both boots (direct vectors) |
| IPAddress.cpp | IPADDR=OK (direct unit checks) |
| WString, Stream, Printable, Print path | exercised by every sketch's serial output + String-using gates on QEMU; no HW-specific risk (pure CPU code, no peripheral interaction) |
| Client.h/Server.h | compile-time interfaces; EthernetClient/Server built and running on HW |

Pending physical follow-ups (user action, then one flash each):
1. Insert SD card → `LinkServer run ... sd_wav_play_test.elf` → expect 3×PASS.
2. Connect RJ45 → `LinkServer run ... ethernet_test.elf` → expect DHCP lease + DNS_OK.

## Re-run attempt 2026-07-14 — board physical state unchanged

Both gates re-flashed (fresh ELFs, built after all clean-room commits):

```
SD_WAV_MOUNT=FAIL          <- sd_wav_play_test: still no card inserted
```

```
ETH_BOOT
IPADDR=OK                  <- clean-room IPAddress re-confirmed on silicon
ETH_DHCP ok=0 ip=0.0.0.0   <- ethernet_test: still no RJ45 link
ETH_NETIF_UP
```

Identical to 2026-07-14 first pass: SD card not inserted, cable not connected.
Firmware side is stable (flash + boot + serial + IPADDR all good on both runs).
Items 1–2 above remain pending on physical setup.
