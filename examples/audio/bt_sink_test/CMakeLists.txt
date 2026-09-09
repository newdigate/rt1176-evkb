cmake_minimum_required(VERSION 3.24)
project(bt_sink_test)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# sdio + iw416 + hci + bt, same manifest as bt_tone_test/m2_hci_probe (NEW-41 links
# bt/'s A2dpSink/BtSinkSession/SbcDecoder, which import_evkb_library globs in
# automatically -- no M2Radio manifest edit needed). Audio via the full-fork
# importer, same as bt_tone_test/transport_test.
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

# NEW-34 piece 2 bench knob: link supervision timeout in ms (0/empty = controller default ~20 s).
set(M2_BT_SUPERVISION_MS "" CACHE STRING "Write_Link_Supervision_Timeout in ms (empty = controller default)")
if(NOT M2_BT_SUPERVISION_MS STREQUAL "")
    add_definitions(-DM2_BT_SUPERVISION_MS=${M2_BT_SUPERVISION_MS})
endif()

# Bench knob (NEW-34): wipe the bond store at boot (bonds_forgotten=1), so a run can prove that a
# headset out of pairing mode REFUSES us without a bond -- the control arm of the stored-key claim.
# OFF by default; the gates and the default build load the store as usual.
option(M2_BT_FORGET_BONDS "Bench: wipe the bonded-device store at boot" OFF)
if(M2_BT_FORGET_BONDS)
    add_definitions(-DM2_BT_FORGET_BONDS=1)
endif()

# NEW-34 piece 3: AVCTP (0x0017) is accepted UNCONDITIONALLY since M2Radio 8e22082 -- the AVRCP target answers the
# headset's RegisterNotification -- so the M2_BT_ACCEPT_AVCTP capture option of 2026-09-06 is gone.

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

# NO M2_BT_LEGACY_PIN here, deliberately -- bt_tone_test and m2_hci_probe both carry that knob and this
# example does not.  A sink's peers are PHONES, which all do SSP; the legacy-PIN path exists for the ESP32 sink
# bt_tone_test pairs with as a SOURCE.  Re-add it (BtLink::setLegacyPin, as bt_tone_test does) if a legacy-PIN
# source ever has to be paired at the bench.
#
# NEW-41: the sink's own identity.  Written to the controller in BtLink's PREPARE (Write_Local_Name), so this
# is the name a phone's Bluetooth settings list shows.
set(M2_BT_SINK_NAME "EVKB-SINK" CACHE STRING "The sink's Bluetooth name (Write_Local_Name)")
add_definitions(-DM2_BT_SINK_NAME="${M2_BT_SINK_NAME}")

# BtSinkSession stops inquiry scan once a bond exists (a phone that knows us PAGES, it does not search).  This
# keeps the sink discoverable anyway, so a second phone can be paired at the bench without wiping the store.
option(M2_BT_SINK_ALWAYS_DISCOVERABLE "Stay discoverable even with a bond stored (bench: pair a second phone)" OFF)
if(M2_BT_SINK_ALWAYS_DISCOVERABLE)
    add_definitions(-DM2_BT_SINK_ALWAYS_DISCOVERABLE=1)
endif()

# NEW-42: the PCM ring's depth and target (blocks) and the START pre-fill.  Defaults are the CHANGE arm
# (32 / 16 / on), which is what the gate builds.  The bench's CONTROL arm -- the NEW-41 behaviour, 16 / 8 / off --
# is `-DBT_SINK_RING=16 -DBT_SINK_TARGET=8 -DBT_SINK_PREFILL=OFF`.  The DelayReport the sink sends follows
# BT_SINK_TARGET automatically (bt_sink_test.cpp derives it), so a resized target is reported to the phone.
set(BT_SINK_RING "32" CACHE STRING "PCM ring depth in audio blocks (AudioInputBluetooth::RING)")
set(BT_SINK_TARGET "16" CACHE STRING "Ring fill the servo holds and START pre-fills to, in blocks (TARGET)")
option(BT_SINK_PREFILL "Pre-fill the ring to TARGET at START and re-prime it when it runs dry" ON)
if(BT_SINK_PREFILL)
    set(BT_SINK_PREFILL_DEF 1)
else()
    set(BT_SINK_PREFILL_DEF 0)
endif()

teensy_add_executable(bt_sink_test bt_sink_test.cpp AudioInputBluetooth.cpp ${M2_FW_SRC} ${M2_BT_FW_SRC})
target_compile_definitions(bt_sink_test.elf PRIVATE
    BT_SINK_RING=${BT_SINK_RING} BT_SINK_TARGET=${BT_SINK_TARGET} BT_SINK_PREFILL=${BT_SINK_PREFILL_DEF})
# Wire covers Wire2 = LPI2C5, the WM8962's control bus: import_evkb_audio_full() creates the target but does
# not link it, and unlike bt_tone_test this example drives the codec (AVRCP absolute volume).
teensy_target_link_libraries(bt_sink_test cores M2Radio Audio Wire)
target_include_directories(bt_sink_test.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(bt_sink_test.elf stdc++)
target_link_libraries(bt_sink_test.elf m)
