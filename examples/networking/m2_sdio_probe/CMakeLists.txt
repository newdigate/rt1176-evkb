cmake_minimum_required(VERSION 3.24)
project(m2_sdio_probe)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_library(M2Radio sdio iw416)

# --- IW416 firmware blob (NOT vendored) -------------------------------------
# The IW416 has no on-board flash: firmware must be downloaded over SDIO every
# boot. NXP ship it under LA_OPT_NXP_Software_License -- a redistributable
# BINARY licence, not an open-source one -- so it MUST NOT be committed to this
# repo. Point this at a local MCUXpresso SDK copy instead:
#
#   cmake -B build -DM2RADIO_IW416_FW=/path/to/fw_bin/inc/IW416/sdIW416_wlan.bin.inc
#
# Without it the example still builds and reports the download as skipped, so
# the QEMU gate is unaffected on a machine that has no SDK.
set(M2RADIO_IW416_FW "" CACHE FILEPATH "IW416 firmware .bin.inc from an NXP SDK")
if(M2RADIO_IW416_FW AND EXISTS "${M2RADIO_IW416_FW}")
    message(STATUS "IW416 firmware: ${M2RADIO_IW416_FW}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/iw416_fw.cpp"
"#include <stdint.h>\n"
"// Generated at configure time from an NXP SDK copy. Never committed.\n"
"extern const uint8_t iw416_fw[];\n"
"extern const uint32_t iw416_fw_len;\n"
"// .progmem keeps this in FLASH. The imxrt1176 linker script folds .rodata*\n"
"// into .data, which lives in DTCM (256K) -- a 273 KB const array there\n"
"// overflows the region and pushes .bss out. (Note FLASHMEM is #defined empty\n"
"// in AudioStream.h and does NOT do this job.)\n"
"const uint8_t iw416_fw[] __attribute__((section(\".progmem\"), used)) = {\n#include \"${M2RADIO_IW416_FW}\"\n};\n"
"const uint32_t iw416_fw_len = sizeof(iw416_fw);\n")
    set(M2_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_fw.cpp")
    add_definitions(-DHAVE_IW416_FW=1)
else()
    message(STATUS "IW416 firmware: not supplied -- download will be skipped")
    set(M2_FW_SRC "")
endif()

# --- WPA2 credentials (NEVER committed) -------------------------------------
# W6 associates to a WPA2 network via the IW416's embedded supplicant: the
# firmware derives the PMK from the passphrase, so the host never handles WPA2
# crypto. The SSID and PSK are a credential and are supplied ONLY at configure
# time, exactly like the firmware blob above:
#
#   cmake -B build ... -DM2RADIO_WIFI_SSID="MyNet" -DM2RADIO_WIFI_PSK="secret"
#
# They are written into a generated header in the BUILD directory (gitignored,
# same as iw416_fw.cpp) so arbitrary passphrase characters are handled cleanly
# and the value never touches a tracked file. Absent -> the WPA2 attempt is
# compiled out and the probe stops after the W5 monitor block, so the QEMU gate
# and the no-SDK build are unaffected. The PSK is never echoed by CMake.
set(M2RADIO_WIFI_SSID "" CACHE STRING "WPA2 SSID to associate to (W6)")
set(M2RADIO_WIFI_PSK  "" CACHE STRING "WPA2 passphrase -- NEVER committed")
if(M2RADIO_WIFI_SSID AND M2RADIO_WIFI_PSK)
    message(STATUS "W6 WPA2 target SSID: ${M2RADIO_WIFI_SSID} (PSK supplied, not shown)")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/wifi_creds.h"
"// Generated at configure time from -DM2RADIO_WIFI_*. NEVER committed.\n"
"#pragma once\n"
"#define M2_WIFI_SSID \"${M2RADIO_WIFI_SSID}\"\n"
"#define M2_WIFI_PSK  \"${M2RADIO_WIFI_PSK}\"\n")
    add_definitions(-DHAVE_WIFI_CREDS=1)
endif()

teensy_add_executable(m2_sdio_probe m2_sdio_probe.cpp ${M2_FW_SRC})
teensy_target_link_libraries(m2_sdio_probe cores M2Radio)
target_include_directories(m2_sdio_probe.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(m2_sdio_probe.elf stdc++)
