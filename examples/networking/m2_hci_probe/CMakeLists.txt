cmake_minimum_required(VERSION 3.24)
project(m2_hci_probe)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# sdio + iw416 for the board preamble and the combo blob download that brings
# the Bluetooth block up; hci for the transport.  lwip/arduino are NOT imported:
# this example never touches the Wi-Fi data path.
import_evkb_library(M2Radio sdio iw416 hci)

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

# ★ WHICH FIRMWARE PATH.  The two are ALTERNATIVES, not additions -- measured
# 2026-08-24: after a BT-only UART download the WLAN SDIO download fails
# (fw_status=0x0), matching NXP's own #ifdef CONFIG_BT_IND_DNLD / #else.
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
option(M2_BT_WAKE_PULSE "Pulse the boot-sleep wake line before the BT firmware download" ON)
if(M2_BT_WAKE_PULSE)
    add_definitions(-DM2_BT_WAKE_PULSE=1)
endif()

option(M2_BT_ASSERT_CTS "Assert the card's CTS input (holds the ENET PHY in reset)" OFF)
if(M2_BT_ASSERT_CTS)
    add_definitions(-DM2_BT_ASSERT_CTS=1)
endif()

teensy_add_executable(m2_hci_probe m2_hci_probe.cpp ${M2_FW_SRC} ${M2_BT_FW_SRC})
teensy_target_link_libraries(m2_hci_probe cores M2Radio)
target_include_directories(m2_hci_probe.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(m2_hci_probe.elf stdc++)
