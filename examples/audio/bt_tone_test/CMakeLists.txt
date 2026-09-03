cmake_minimum_required(VERSION 3.24)
project(bt_tone_test)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# sdio + iw416 + hci + bt, same manifest as m2_hci_probe (BT-3 phase 4 links
# bt/'s new Sbc/MediaPacketizer/A2dpSource, which import_evkb_library globs in
# automatically -- no M2Radio manifest edit needed). Audio via the full-fork
# importer, same as transport_test/acid_bass_test.
import_evkb_library(M2Radio sdio iw416 hci bt)
import_evkb_audio_full()

# --- IW416 firmware blob (NOT vendored) -------------------------------------
# Same rule as every other m2_* example: NXP ship the blob under
# LA_OPT_NXP_Software_License -- redistributable BINARY, not open source -- so
# it MUST NOT be committed.
#   cmake -B build -DM2RADIO_IW416_FW=/path/to/fw_bin/inc/IW416/sduartIW416_wlan_bt.bin.inc
# Unused by this example (no SDIO/Wi-Fi bring-up here), kept only so the
# firmware-blob CMake machinery matches every other m2_* example byte-for-byte.
set(M2RADIO_IW416_FW "" CACHE FILEPATH "IW416 firmware .bin.inc from an NXP SDK")
if(M2RADIO_IW416_FW AND EXISTS "${M2RADIO_IW416_FW}")
    message(STATUS "IW416 firmware: ${M2RADIO_IW416_FW}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/iw416_fw.cpp"
"#include <stdint.h>\n"
"// Generated at configure time from an NXP SDK copy. Never committed.\n"
"extern const uint8_t iw416_fw[];\n"
"extern const uint32_t iw416_fw_len;\n"
"const uint8_t iw416_fw[] __attribute__((section(\".progmem\"), used)) = {\n#include \"${M2RADIO_IW416_FW}\"\n};\n"
"const uint32_t iw416_fw_len = sizeof(iw416_fw);\n")
    set(M2_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_fw.cpp")
    add_definitions(-DHAVE_IW416_FW=1)
else()
    message(STATUS "IW416 firmware: not supplied -- download will be skipped")
    set(M2_FW_SRC "")
endif()

# --- BT-only UART firmware image (NOT vendored, and a SECOND blob) ----------
# Same rule and same synthetic fallback as m2_hci_probe:
#   cmake -B build -DM2RADIO_IW416_BT_FW=/path/to/fw_bin/inc/IW416/uartIW416_bt.bin.inc
set(M2RADIO_IW416_BT_FW "" CACHE FILEPATH "IW416 BT-only UART firmware .bin.inc from an NXP SDK")
if(M2RADIO_IW416_BT_FW AND EXISTS "${M2RADIO_IW416_BT_FW}")
    message(STATUS "IW416 BT firmware: ${M2RADIO_IW416_BT_FW}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp"
"#include <stdint.h>\n"
"// Generated at configure time from an NXP SDK copy. Never committed.\n"
"extern const uint8_t iw416_bt_fw[];\n"
"extern const uint32_t iw416_bt_fw_len;\n"
"const uint8_t iw416_bt_fw[] __attribute__((section(\".progmem\"), used)) = {\n#include \"${M2RADIO_IW416_BT_FW}\"\n};\n"
"const uint32_t iw416_bt_fw_len = sizeof(iw416_bt_fw);\n")
    set(M2_BT_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_bt_fw.cpp")
    add_definitions(-DHAVE_IW416_BT_FW=1)
else()
    # No real blob -> compile in a small SYNTHETIC image, same 1 KB deterministic
    # pattern as m2_hci_probe, so the V3 download protocol is gateable with no
    # NXP-licensed file anywhere near the gate.
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

# u-blox's combo-over-SDIO path vs. the BT-only V3 UART download this tree
# built in BT-1.  Default ON, same as m2_hci_probe, so every gate and every
# existing build is byte-identical.
option(M2_BT_UART_DNLD "Run the BT-only V3 UART firmware download (OFF = u-blox's combo-over-SDIO path)" ON)
if(NOT M2_BT_UART_DNLD)
    message(STATUS "BT firmware path: COMBO over SDIO (BT UART download skipped)")
    add_definitions(-DM2_BT_NO_UART_DNLD=1)
endif()

option(M2_BT_WAKE_PULSE "Pulse the boot-sleep wake line before the BT firmware download" ON)
if(M2_BT_WAKE_PULSE)
    add_definitions(-DM2_BT_WAKE_PULSE=1)
endif()

# REQUIRED FOR HCI ON HARDWARE -- see m2_hci_probe/CMakeLists.txt for the full
# rationale.  Default OFF: the QEMU gate models no UART flow control, so the
# default build stays byte-identical.  Bench build sets -DM2_BT_ASSERT_CTS=ON.
option(M2_BT_ASSERT_CTS "Assert host RTS=card CTS -- REQUIRED for HW HCI; holds the 1G ENET PHY in reset" OFF)
if(M2_BT_ASSERT_CTS)
    add_definitions(-DM2_BT_ASSERT_CTS=1)
endif()

# Hardware RXRTSE flow control on LPUART2 (preferred over the static M2_BT_ASSERT_CTS
# GPIO hack for A2DP media rates -- see docs/superpowers/specs/2026-09-03-bt3-rxrtse-*).
# Also holds/toggles the 1G ENET PHY reset (R1866); BT-only builds only.
option(M2_BT_RTS_FLOW "Enable hardware RXRTSE flow control on LPUART2 (Serial2.attachRts)" OFF)
set(M2_BT_RTS_WATER "1" CACHE STRING "MODIR RTSWATER watermark for M2_BT_RTS_FLOW (0..3)")
if(M2_BT_RTS_FLOW)
    add_definitions(-DM2_BT_RTS_FLOW=1 -DM2_BT_RTS_WATER=${M2_BT_RTS_WATER})
endif()

# Bench knob: with several sinks discoverable at once, prefer the inquiry hit
# whose resolved name contains this substring (else first hit).
set(M2_BT_TARGET_NAME "" CACHE STRING "Prefer the A2DP sink whose inquiry name contains this substring")
if(NOT M2_BT_TARGET_NAME STREQUAL "")
    add_definitions(-DM2_BT_TARGET_NAME="${M2_BT_TARGET_NAME}")
endif()

# Bench knob: disable Simple Pairing on our side so pairing falls back to
# legacy PIN (we answer PIN_Code_Request with 1234).
option(M2_BT_LEGACY_PIN "Write_Simple_Pairing_Mode=0: legacy PIN pairing (1234)" OFF)
if(M2_BT_LEGACY_PIN)
    add_definitions(-DM2_BT_LEGACY_PIN=1)
endif()

# Bench knob: A2dpSource::connect() is one-shot, and the BR/EDR remote-name step
# races on some peers (a headset intermittently returns no_name_event).  With
# this ON, setup() defers the connect and loop() retries it every 5 s until it
# succeeds -- no manual reset dance.  OFF by default so the gates and the default
# build keep the byte-identical setup()-time one-shot connect.
option(M2_BT_CONNECT_RETRY "Bench: retry A2dpSource::connect() from loop() until it succeeds" OFF)
if(M2_BT_CONNECT_RETRY)
    add_definitions(-DM2_BT_CONNECT_RETRY=1)
endif()

# Bench diagnostic: dump every in/out ACL packet on the link as hex over the VCOM
# (feed tools/acl-trace-to-btsnoop.py). OFF by default; gates/default build unchanged.
option(M2_BT_ACL_TRACE "Trace every ACL packet (L2cap::onAclTrace) over the console" OFF)
if(M2_BT_ACL_TRACE)
    add_definitions(-DM2_BT_ACL_TRACE=1)
endif()

# Phase 0 (BT-3): after identity at the download rate, switch the controller
# to M2_BT_FAST_BAUD_RATE with the NXP vendor set-baud command (0xFC09, uint32
# LE), re-baud LPUART2 and re-validate.  Default OFF, same as m2_hci_probe.
option(M2_BT_FAST_BAUD "Switch HCI to M2_BT_FAST_BAUD_RATE after identity" OFF)
set(M2_BT_FAST_BAUD_RATE "921600" CACHE STRING "Rate for M2_BT_FAST_BAUD")
if(M2_BT_FAST_BAUD)
    add_definitions(-DM2_BT_FAST_BAUD=${M2_BT_FAST_BAUD_RATE})
endif()

teensy_add_executable(bt_tone_test bt_tone_test.cpp AudioOutputBluetooth.cpp ${M2_FW_SRC} ${M2_BT_FW_SRC})
teensy_target_link_libraries(bt_tone_test cores M2Radio Audio)
target_include_directories(bt_tone_test.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(bt_tone_test.elf stdc++)
target_link_libraries(bt_tone_test.elf m)
