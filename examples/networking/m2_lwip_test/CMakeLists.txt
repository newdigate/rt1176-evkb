cmake_minimum_required(VERSION 3.24)
project(m2_lwip_test)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# lwip exactly as lwip_test imports it (port/ carries lwipopts.h + arch/;
# port/ethernetif.c also compiles but is inert here -- nothing calls it).
import_evkb_library(lwip
    src/include src/core src/core/ipv4 src/netif port port/arch)
# M2Radio: sdio + iw416 as the probe, PLUS the lwip netif glue subdir.
import_evkb_library(M2Radio sdio iw416 lwip)

# --- IW416 firmware blob (NOT vendored) -- same rules as m2_sdio_probe ------
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

# --- Wi-Fi credentials (NEVER committed) -- same pattern as m2_sdio_probe ---
# connectStation() needs an SSID + WPA2 passphrase to bring the station up.
# The SSID and PSK are a credential and are supplied ONLY at configure time,
# exactly like the firmware blob above:
#
#   cmake -B build ... -DM2RADIO_WIFI_SSID="MyNet" -DM2RADIO_WIFI_PSK="secret"
#
# They are written into a generated header in the BUILD directory (gitignored,
# same as iw416_fw.cpp) so arbitrary passphrase characters are handled cleanly
# and the value never touches a tracked file, a compile command line, or a
# make fragment. Absent -> the Wi-Fi connect attempt is compiled out and the
# example falls back to the alive= heartbeat, so the QEMU gate and the no-SDK
# build are unaffected. The PSK is never echoed by CMake.
set(M2RADIO_WIFI_SSID "" CACHE STRING "Target AP SSID")
set(M2RADIO_WIFI_PSK  "" CACHE STRING "Target AP WPA2 passphrase -- NEVER committed")
if(M2RADIO_WIFI_SSID AND M2RADIO_WIFI_PSK)
    message(STATUS "Wi-Fi target SSID: ${M2RADIO_WIFI_SSID} (PSK supplied, not shown)")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/wifi_creds.h"
"// Generated at configure time from -DM2RADIO_WIFI_*. NEVER committed.\n"
"#pragma once\n"
"#define M2_WIFI_SSID \"${M2RADIO_WIFI_SSID}\"\n"
"#define M2_WIFI_PSK  \"${M2RADIO_WIFI_PSK}\"\n")
    add_definitions(-DHAVE_WIFI_CREDS=1)
endif()

teensy_add_executable(m2_lwip_test m2_lwip_test.cpp ${M2_FW_SRC})
teensy_target_link_libraries(m2_lwip_test cores M2Radio lwip)
target_include_directories(m2_lwip_test.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(m2_lwip_test.elf stdc++)
