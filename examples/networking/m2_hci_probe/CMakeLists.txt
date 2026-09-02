cmake_minimum_required(VERSION 3.24)
project(m2_hci_probe)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# sdio + iw416 for the board preamble and the combo blob download that brings
# the Bluetooth block up; hci for the transport.  lwip/arduino are NOT imported:
# this example never touches the Wi-Fi data path.
# ★ rt1062 links the BLUETOOTH SUBDIR ONLY.  M2Radio's sdio/ and iw416/ target
# the RT1176's USDHC and are not ported; hci/ is board-independent and drives
# the M.2 BT UART through Serial2 on both boards (LPUART2 on the 1176,
# LPUART3 on the 1062 -- the same physical M.2 pins in each case).
if(EVKB_BOARD STREQUAL "rt1062")
    import_evkb_library(M2Radio hci)
else()
    import_evkb_library(M2Radio sdio iw416 hci)
endif()

# --- IW416 firmware blob (NOT vendored) -------------------------------------
# Same rule as every other m2_* example: NXP ship the blob under
# LA_OPT_NXP_Software_License -- redistributable BINARY, not open source -- so
# it MUST NOT be committed.  Point this at a local MCUXpresso SDK copy:
#
#   cmake -B build -DM2RADIO_IW416_FW=/path/to/fw_bin/inc/IW416/sduartIW416_wlan_bt.bin.inc
#
# The Bluetooth firmware rides the COMBO image (sd-uart: sdio-wlan +
# uart-bt); the Wi-Fi-only sdIW416_wlan.bin would leave the BT block silent.
# Without a blob the example still builds, reports the download as skipped,
# and still runs the HCI sequence -- which is what both QEMU gates want.
set(M2RADIO_IW416_FW "" CACHE FILEPATH "IW416 firmware .bin.inc from an NXP SDK")
if(M2RADIO_IW416_FW AND EXISTS "${M2RADIO_IW416_FW}")
    message(STATUS "IW416 firmware: ${M2RADIO_IW416_FW}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/iw416_fw.cpp"
"#include <stdint.h>\n"
"// Generated at configure time from an NXP SDK copy. Never committed.\n"
"extern const uint8_t iw416_fw[];\n"
"extern const uint32_t iw416_fw_len;\n"
"// .progmem keeps this in FLASH: the imxrt1176 linker folds .rodata* into\n"
"// .data (DTCM), which a 400 KB array would overflow.\n"
"const uint8_t iw416_fw[] __attribute__((section(\".progmem\"), used)) = {\n#include \"${M2RADIO_IW416_FW}\"\n};\n"
"const uint32_t iw416_fw_len = sizeof(iw416_fw);\n")
    set(M2_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_fw.cpp")
    add_definitions(-DHAVE_IW416_FW=1)
else()
    message(STATUS "IW416 firmware: not supplied -- download will be skipped")
    set(M2_FW_SRC "")
endif()

# --- BT-only UART firmware image (NOT vendored, and a SECOND blob) ----------
# ★ The combo blob above brings up the WLAN core ONLY.  Measured on silicon
# 2026-08-23: the SDIO download stops 8,776 bytes short (sent=402288/411064)
# and the BT core never answers an HCI command -- it sits in its own UART
# bootloader sending a V3 start indication and waiting.  Feeding it THIS image
# over LPUART2 is what brings Bluetooth up.  Same licence rule as every other
# NXP blob in this tree: LA_OPT binary, redistributable but NOT open source,
# so it MUST NOT be committed.  Supply it at configure time:
#
#   cmake -B build -DM2RADIO_IW416_BT_FW=/path/to/fw_bin/inc/IW416/uartIW416_bt.bin.inc
#
# Absent -> the download is compiled out and the probe reports it as skipped,
# so both QEMU gates and a no-SDK build are unaffected.
set(M2RADIO_IW416_BT_FW "" CACHE FILEPATH "IW416 BT-only UART firmware .bin.inc from an NXP SDK")
if(M2RADIO_IW416_BT_FW AND EXISTS "${M2RADIO_IW416_BT_FW}")
    message(STATUS "IW416 BT firmware: ${M2RADIO_IW416_BT_FW}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp"
"#include <stdint.h>\n"
"// Generated at configure time from an NXP SDK copy. Never committed.\n"
"extern const uint8_t iw416_bt_fw[];\n"
"extern const uint32_t iw416_bt_fw_len;\n"
"// .progmem keeps this in FLASH, as for the WLAN blob: the imxrt1176 linker\n"
"// folds .rodata* into .data (DTCM), which a 129 KB array would crowd.\n"
"const uint8_t iw416_bt_fw[] __attribute__((section(\".progmem\"), used)) = {\n#include \"${M2RADIO_IW416_BT_FW}\"\n};\n"
"const uint32_t iw416_bt_fw_len = sizeof(iw416_bt_fw);\n")
    set(M2_BT_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp")
    add_definitions(-DHAVE_IW416_BT_FW=1)
else()
    # ★ NO REAL BLOB -> compile in a SMALL SYNTHETIC IMAGE instead, so the V3
    # download protocol is GATEABLE with no NXP-licensed file anywhere near the
    # gate.  Without this the downloader would be compiled out of every build a
    # gate can run, and the only code path proving it works would be a silicon
    # transcript -- exactly the situation the m2_* gates exist to avoid.
    #
    # It is 1 KB of a deterministic pattern, and the sketch PRINTS
    # `bt_fw_source=synthetic` whenever it is in use, so no transcript can ever
    # confuse it with the real image.  A real card would refuse to authenticate
    # it, which is the correct outcome: this build is for QEMU.
    message(STATUS "IW416 BT firmware: not supplied -- using a 1 KB SYNTHETIC image (gate builds)")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp"
"#include <stdint.h>\n"
"// Generated at configure time. NOT NXP firmware -- a deterministic pattern\n"
"// so the V3 download protocol can be gated without a licensed blob.\n"
"extern const uint8_t iw416_bt_fw[];\n"
"extern const uint32_t iw416_bt_fw_len;\n"
"const uint8_t iw416_bt_fw[] __attribute__((section(\".progmem\"), used)) = {\n"
"#define R16(n) (uint8_t)(n),(uint8_t)(n+1),(uint8_t)(n+2),(uint8_t)(n+3),(uint8_t)(n+4),(uint8_t)(n+5),(uint8_t)(n+6),(uint8_t)(n+7),(uint8_t)(n+8),(uint8_t)(n+9),(uint8_t)(n+10),(uint8_t)(n+11),(uint8_t)(n+12),(uint8_t)(n+13),(uint8_t)(n+14),(uint8_t)(n+15)\n"
"#define R256(n) R16(n),R16(n+16),R16(n+32),R16(n+48),R16(n+64),R16(n+80),R16(n+96),R16(n+112),R16(n+128),R16(n+144),R16(n+160),R16(n+176),R16(n+192),R16(n+208),R16(n+224),R16(n+240)\n"
"R256(0), R256(1), R256(2), R256(3)\n"
"};\n"
"const uint32_t iw416_bt_fw_len = sizeof(iw416_bt_fw);\n")
    set(M2_BT_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp")
    add_definitions(-DHAVE_IW416_BT_FW=1 -DBT_FW_IS_SYNTHETIC=1)
endif()

# ★ WHICH FIRMWARE PATH.  ★★ These are NOT alternatives -- corrected 2026-08-25.
# It looked that way because the wrong Wi-Fi image was in use: after a BT UART
# download the COMBO image over SDIO fails (fw_download=cmd-timeout, card=0),
# but the WLAN-ONLY image succeeds (fw_download=ok, card=1).  That is exactly
# NXP's CONFIG_BT_IND_DNLD mode, whose wlan_bt_fw.h includes BOTH
# sdIW416_wlan.h AND uartIW416_bt.h -- one image per bus, both downloaded.
# So: supply sdIW416_wlan.bin.inc here, not sduartIW416_wlan_bt.bin.inc, when
# M2_BT_UART_DNLD is ON.
#   ON  (default) -- the BT-only V3 UART download this tree built in BT-1.
#   OFF           -- u-blox's documented path (MAYA-W1 SIM UBX-21010495 R09
#                    s4.4.3): the COMBO image goes over SDIO and carries BOTH
#                    radios, with no BT UART download at all.  Then attach HCI
#                    -- at 3 Mbaud, per s4.4.6.
# Default ON so every gate and every existing build is byte-identical.
option(M2_BT_UART_DNLD "Run the BT-only V3 UART firmware download (OFF = u-blox's combo-over-SDIO path)" ON)
if(NOT M2_BT_UART_DNLD)
    message(STATUS "BT firmware path: COMBO over SDIO (BT UART download skipped)")
    add_definitions(-DM2_BT_NO_UART_DNLD=1)
endif()

# Bring-up switch for the 2026-08-24 hypothesis: inject the bootloader's
# UART-configuration block ahead of the image.  Off by default -- it changes
# what goes on the wire, so it must be asked for.
option(M2_BT_INJECT_UART_CFG "Inject the V3 UART-config block before the BT image" OFF)
if(M2_BT_INJECT_UART_CFG)
    add_definitions(-DM2_BT_INJECT_UART_CFG=1)
endif()

# Drive J54 pin 36 (the card's CTS input) low, so the controller is always
# clear to send.  Off by default: it holds the gigabit Ethernet PHY in reset.
# NXP's wakeUpControllerFromBootSleep(): a 10 ms LOW pulse on GPIO_DISP_B2_13
# before the firmware download, with the pad then returned to LPUART2_RTS_B.
# ON by default -- it is part of the vendor's own download sequence, and a gate
# whose example was configured without it would report SKIP rather than FAIL.
# The RX-line continuity / PDn-swing probe.  Re-muxes the BT UART's RX pad to
# GPIO before the transport takes it, so it is a BENCH instrument, not part of
# normal operation.
# ★ Default ON for rt1062 and OFF for rt1176: the 1062 is where the electrical
# question is open, and the 1176's two gates assert an exact line sequence that
# extra probe output would disturb for no benefit.
if(EVKB_BOARD STREQUAL "rt1062")
    option(M2_CONTINUITY_PROBE "Probe the BT RX line and PDn electrically" ON)
else()
    option(M2_CONTINUITY_PROBE "Probe the BT RX line and PDn electrically" OFF)
endif()
if(M2_CONTINUITY_PROBE)
    add_definitions(-DM2_CONTINUITY_PROBE=1)
endif()

option(M2_BT_WAKE_PULSE "Pulse the boot-sleep wake line before the BT firmware download" ON)
if(M2_BT_WAKE_PULSE)
    add_definitions(-DM2_BT_WAKE_PULSE=1)
endif()

# ★ REQUIRED FOR HCI ON HARDWARE (found 2026-08-28 on the Murata 1XK / IW416).
# The IW416 firmware gates its HCI transmit on the host RTS line; with it
# UNdriven the card downloads perfectly and then never answers HCI (measured:
# hci_reset=no_response at every baud, framing=0).  Driving BT_UART_RTS
# (GPIO_DISP_B2_13) low = the card's CTS asserted = clear to send, and the
# WHOLE handshake then works on attempt 1 at 115200: HCI_Reset CC, Read Local
# Version (mfr 0x0048 Marvell), BD_ADDR, Buffer Size, BR/EDR inquiry.  The rate
# was never the issue -- the controller runs HCI at the download rate.
# ★ Kept an EXPLICIT OPTION, default OFF: the QEMU gates model no UART flow
# control, so the default build stays byte-identical.  Bench build sets
# -DM2_BT_ASSERT_CTS=ON.  Caveat: the assert holds the 1 Gb ENET PHY in reset
# via R1866 -- BT-only, not alongside 1 G Ethernet.  For A2DP media rates,
# revisit dynamic hardware RXRTSE; this static assert suffices for BT-1/BT-2.
option(M2_BT_ASSERT_CTS "Assert host RTS=card CTS -- REQUIRED for HW HCI; holds the 1G ENET PHY in reset" OFF)
if(M2_BT_ASSERT_CTS)
    add_definitions(-DM2_BT_ASSERT_CTS=1)
endif()

# BT-2 . B4: after inquiry, connect to the first Audio/Video device, do SSP
# Just-Works pairing (IO cap NoInputNoOutput, general bonding) and turn on
# encryption.  Bench-only (needs a real headset in pairing mode); default OFF so
# the QEMU gates are byte-identical.  Requires M2_BT_ASSERT_CTS on hardware.
option(M2_BT_CONNECT "B4: ACL Create_Connection + SSP Just-Works pairing + encryption to a discovered device" OFF)
if(M2_BT_CONNECT)
    add_definitions(-DM2_BT_CONNECT=1)
endif()

# Experiment: run the B6 SDP query on the UNENCRYPTED link, right after
# Create_Connection and BEFORE authentication -- some devices only serve SDP
# pre-bond.  Isolates the pre-pairing SDP path and stops there.  Requires
# M2_BT_CONNECT; default OFF so the gates are byte-identical.
option(M2_BT_SDP_BEFORE_PAIRING "B6 experiment: query SDP before pairing/encryption (unencrypted link)" OFF)
if(M2_BT_SDP_BEFORE_PAIRING)
    add_definitions(-DM2_BT_SDP_BEFORE_PAIRING=1)
endif()

# B6-alt: after the encrypted link, open an L2CAP CO channel to the AVDTP PSM
# (0x0019) and send an AVDTP Discover -- an A2DP sink MUST answer with its Stream
# End Points.  Replaces the SDP query (probeSdp) with probeAvdtp.  Requires
# M2_BT_CONNECT; default OFF so the gates are byte-identical.
option(M2_BT_AVDTP_DISCOVER "B6-alt: AVDTP Discover on the encrypted link (reads the sink's stream endpoints)" OFF)
if(M2_BT_AVDTP_DISCOVER)
    add_definitions(-DM2_BT_AVDTP_DISCOVER=1)
endif()

# Bench knob: with several headsets discoverable at once, prefer the inquiry hit
# whose resolved name contains this substring (else first Audio/Video hit).
set(M2_BT_TARGET_NAME "" CACHE STRING "Prefer the A/V inquiry hit whose name contains this substring")
if(NOT M2_BT_TARGET_NAME STREQUAL "")
    add_definitions(-DM2_BT_TARGET_NAME="${M2_BT_TARGET_NAME}")
endif()

# Bench knob: the AuthRequirements byte in our IO_Capability_Request_Reply
# (default 0x04 = general bonding, MITM not required; 0x00 = no bonding).
set(M2_BT_AUTH_REQ "" CACHE STRING "SSP AuthRequirements byte, e.g. 0x00 or 0x04")
if(NOT M2_BT_AUTH_REQ STREQUAL "")
    add_definitions(-DM2_BT_AUTH_REQ=${M2_BT_AUTH_REQ})
endif()

# Bench knob: do NOT send Authentication_Requested ourselves -- open the AVDTP
# channel unauthenticated and let the PEER's service security policy initiate
# SSP (the sink drives the pairing).  Pair with M2_BT_AVDTP_DISCOVER=ON.
option(M2_BT_PEER_AUTH "Let the peer initiate authentication (skip our Authentication_Requested)" OFF)
if(M2_BT_PEER_AUTH)
    add_definitions(-DM2_BT_PEER_AUTH=1)
endif()

# Bench knob: enable Secure Connections HOST support (HCI 0x0C7A) before pairing.
option(M2_BT_SC_HOST "Write_Secure_Connections_Host_Support=1 before pairing" OFF)
if(M2_BT_SC_HOST)
    add_definitions(-DM2_BT_SC_HOST=1)
endif()

# Bench knob: Create_Connection with Allow_Role_Switch=0 (stay master; refuse the
# peer's switch at setup).  Bluedroid requests a role switch during connection
# setup, which no headset did; a switch colliding with SSP is a pairing killer.
option(M2_BT_NO_ROLE_SWITCH "Create_Connection: do not allow role switch" OFF)
if(M2_BT_NO_ROLE_SWITCH)
    add_definitions(-DM2_BT_NO_ROLE_SWITCH=1)
endif()

# Bench knob: disable Simple Pairing on our side so pairing falls back to
# legacy PIN (we answer PIN_Code_Request with 1234; the ESP32 sink does too).
option(M2_BT_LEGACY_PIN "Write_Simple_Pairing_Mode=0: legacy PIN pairing (1234)" OFF)
if(M2_BT_LEGACY_PIN)
    add_definitions(-DM2_BT_LEGACY_PIN=1)
endif()

# Phase 0 (BT-3): after identity at the download rate, switch the controller
# to M2_BT_FAST_BAUD_RATE with the NXP vendor set-baud command (0xFC09, uint32
# LE), re-baud LPUART2 and re-validate.  Default OFF: the gates run at 115200
# (a chardev has no baud) except [baud], which asserts the SEQUENCE.
option(M2_BT_FAST_BAUD "Switch HCI to M2_BT_FAST_BAUD_RATE after identity" OFF)
set(M2_BT_FAST_BAUD_RATE "921600" CACHE STRING "Rate for M2_BT_FAST_BAUD")
if(M2_BT_FAST_BAUD)
    add_definitions(-DM2_BT_FAST_BAUD=${M2_BT_FAST_BAUD_RATE})
endif()

# Phase 0 (BT-3): after the baud switch, put the controller in LOCAL LOOPBACK
# (Write_Loopback_Mode 0x1802) and count N ACL packets echoed back -- the
# loss measurement this board's one-sided flow control demands.  Silicon-only.
option(M2_BT_LOOPBACK "After the baud switch: local loopback, N ACL packets echoed" OFF)
if(M2_BT_LOOPBACK)
    add_definitions(-DM2_BT_LOOPBACK=1)
endif()

teensy_add_executable(m2_hci_probe m2_hci_probe.cpp ${M2_FW_SRC} ${M2_BT_FW_SRC})
teensy_target_link_libraries(m2_hci_probe cores M2Radio)
target_include_directories(m2_hci_probe.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(m2_hci_probe.elf stdc++)
