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

teensy_add_executable(m2_hci_probe m2_hci_probe.cpp ${M2_FW_SRC})
teensy_target_link_libraries(m2_hci_probe cores M2Radio)
target_include_directories(m2_hci_probe.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(m2_hci_probe.elf stdc++)
