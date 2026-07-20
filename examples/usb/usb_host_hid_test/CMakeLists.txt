cmake_minimum_required(VERSION 3.24)
project(usb_host_hid_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../../../cores/imxrt1176)

teensy_add_executable(usb_host_hid_test usb_host_hid_test.cpp)
teensy_target_link_libraries(usb_host_hid_test cores)

# --- USBHost_t36 (Sub-project A: HID keyboard + mouse) ------------------------
# The USBHost_t36 sources are compiled straight into the .elf (teensy_add_executable
# names the target <name>.elf) so they pick up teensy_flags -> __IMXRT1176__ + the
# core include dirs.  Sub-project A = the HID set; NO MassStorageDriver / joystick /
# midi / serial / antplus / adk / rawhid / digitizer / SerEMU / USBFilesystemFormatter.
# bluetooth.cpp is LINKED-BUT-UNUSED: keyboard.cpp/mouse.cpp hard-reference
# BluetoothController (driver_ready_for_bluetooth / sendL2CapCommand), so it is a
# required link dep even though the firmware instantiates no BluetoothController
# (dead code, dropped by --gc-sections) -- same pattern as SPI-SD in the SD fs gate.
set(USBHOST $ENV{HOME}/Development/USBHost_t36)
target_sources(usb_host_hid_test.elf PRIVATE
    ${USBHOST}/ehci.cpp ${USBHOST}/enumeration.cpp ${USBHOST}/hub.cpp
    ${USBHOST}/memory.cpp ${USBHOST}/hid.cpp ${USBHOST}/keyboard.cpp ${USBHOST}/mouse.cpp
    ${USBHOST}/bluetooth.cpp)

# USBHost_t36.h unconditionally does `#include <FS.h>` (resolves from the core,
# present from the SD work) AND `#include <SdFat.h>`.  SdFat.h uses relative
# internal includes, so its `src` root alone resolves the whole tree; the only
# external header it drags in is SPI.h (SdSpiDriver.h, default SPI_DRIVER_SELECT).
# These are INCLUDE dirs only -- no SdFat/SPI sources are compiled or linked: the
# USBHost_t36 sources we build reference no SdFat/SPI symbols (only the excluded
# MassStorageDriver.cpp does), so the header just needs to parse.
target_include_directories(usb_host_hid_test.elf PRIVATE
    ${USBHOST} ${USBHOST}/utility
    $ENV{HOME}/Development/SdFat/src
    $ENV{HOME}/Development/SPI
    $ENV{HOME}/Development/EEPROM)   # bluetooth.cpp #includes <EEPROM.h> (core links eeprom.c)

target_link_libraries(usb_host_hid_test.elf stdc++)
