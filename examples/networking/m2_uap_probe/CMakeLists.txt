cmake_minimum_required(VERSION 3.24)
project(m2_uap_probe)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_library(M2Radio sdio iw416)

# --- IW416 firmware blob (NOT vendored) -------------------------------------
# Same rule as every other m2_* example: NXP ship the blob under
# LA_OPT_NXP_Software_License -- redistributable BINARY, not open source -- so
# it MUST NOT be committed.  Point this at a local MCUXpresso SDK copy:
#
#   cmake -B build -DM2RADIO_IW416_FW=/path/to/fw_bin/inc/IW416/sduartIW416_wlan_bt.bin.inc
#
# ★ WHICH BLOB MATTERS HERE, more than in any other example.  The board
# normally downloads sduartIW416_wlan_bt.bin (Wi-Fi + Bluetooth, the full
# image); the SDK also ships sdIW416_wlan.bin (Wi-Fi only).  This example's
# whole question is what a given blob's command surface contains, so a run is
# only interpretable together with the blob it ran.  Configure a second build
# directory against the other .bin.inc to A/B them -- that pair IS the
# experiment, not a variation on it.
#
# Without a blob the example still builds and reports the download as skipped,
# so the QEMU card-absent gate is unaffected on a machine with no SDK.
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
"// overflows the region and pushes .bss out.\n"
"const uint8_t iw416_fw[] __attribute__((section(\".progmem\"), used)) = {\n#include \"${M2RADIO_IW416_FW}\"\n};\n"
"const uint32_t iw416_fw_len = sizeof(iw416_fw);\n")
    set(M2_FW_SRC "${CMAKE_CURRENT_BINARY_DIR}/iw416_fw.cpp")
    add_definitions(-DHAVE_IW416_FW=1)
else()
    message(STATUS "IW416 firmware: not supplied -- download will be skipped")
    set(M2_FW_SRC "")
endif()

# --- BSS_START: RF emission, opt in only -------------------------------------
# BSS_START with no preceding SYS_CONFIGURE starts a BSS on whatever the
# firmware defaults to -- an unconfigured AP beaconing on the bench.  The
# read-only probes answer the Phase-0 question without transmitting, so this
# is off unless asked for:
#
#   cmake -B build-bssstart ... -DM2_UAP_PROBE_BSS_START=ON
option(M2_UAP_PROBE_BSS_START "Also probe BSS_START/BSS_STOP (starts beaconing)" OFF)
if(M2_UAP_PROBE_BSS_START)
    message(STATUS "uAP probe: BSS_START enabled -- this image TRANSMITS")
    add_definitions(-DM2_UAP_PROBE_BSS_START=1)
endif()

teensy_add_executable(m2_uap_probe m2_uap_probe.cpp ${M2_FW_SRC})
teensy_target_link_libraries(m2_uap_probe cores M2Radio)
target_include_directories(m2_uap_probe.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(m2_uap_probe.elf stdc++)
