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

teensy_add_executable(m2_sdio_probe m2_sdio_probe.cpp ${M2_FW_SRC})
teensy_target_link_libraries(m2_sdio_probe cores M2Radio)

target_link_libraries(m2_sdio_probe.elf stdc++)
