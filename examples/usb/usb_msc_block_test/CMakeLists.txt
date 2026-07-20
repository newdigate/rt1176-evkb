cmake_minimum_required(VERSION 3.24)
project(usb_msc_block_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../../../cores/imxrt1176)

# Unlike the HID/MIDI gates (SdFat include-only), the MSC driver USES SdFat:
# USBDrive : public FsBlockDeviceInterface and USBFilesystem holds an FsVolume.
# So SdFat is COMPILED+LINKED here.  It is a 1.5-format lib; the teensy macro globs
# each listed dir non-recursively, so enumerate every src subdir (same list the
# sd_fs_test gate proved links on this core, incl. src/SpiDriver).
import_arduino_library(SdFat $ENV{HOME}/Development/SdFat
    src src/common src/SdCard src/FatLib src/ExFatLib src/FsLib src/iostream src/SpiDriver)
import_arduino_library(SPI $ENV{HOME}/Development/SPI)

teensy_add_executable(usb_msc_block_test usb_msc_block_test.cpp)
teensy_target_link_libraries(usb_msc_block_test cores SdFat SPI)

# USBHost_t36 MSC set: ehci/enumeration/hub/memory + MassStorageDriver.  NO
# bluetooth.cpp (MassStorageDriver references 0 BluetoothController symbols -- the
# MIDI-gate simplification).  Compiled onto the .elf so they inherit __IMXRT1176__.
set(USBHOST $ENV{HOME}/Development/USBHost_t36)
target_sources(usb_msc_block_test.elf PRIVATE
    ${USBHOST}/ehci.cpp ${USBHOST}/enumeration.cpp ${USBHOST}/hub.cpp
    ${USBHOST}/memory.cpp ${USBHOST}/MassStorageDriver.cpp)
target_include_directories(usb_msc_block_test.elf PRIVATE
    ${USBHOST} ${USBHOST}/utility
    $ENV{HOME}/Development/SdFat/src
    $ENV{HOME}/Development/SPI)

target_link_libraries(usb_msc_block_test.elf stdc++)
