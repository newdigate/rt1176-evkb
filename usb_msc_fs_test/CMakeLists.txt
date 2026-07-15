cmake_minimum_required(VERSION 3.24)
project(usb_msc_fs_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)

import_arduino_library(SdFat $ENV{HOME}/Development/SdFat
    src src/common src/SdCard src/FatLib src/ExFatLib src/FsLib src/iostream src/SpiDriver)
import_arduino_library(SPI $ENV{HOME}/Development/SPI)

teensy_add_executable(usb_msc_fs_test usb_msc_fs_test.cpp)
teensy_target_link_libraries(usb_msc_fs_test cores SdFat SPI)

set(USBHOST $ENV{HOME}/Development/USBHost_t36)
target_sources(usb_msc_fs_test.elf PRIVATE
    ${USBHOST}/ehci.cpp ${USBHOST}/enumeration.cpp ${USBHOST}/hub.cpp
    ${USBHOST}/memory.cpp ${USBHOST}/MassStorageDriver.cpp
    ${USBHOST}/USBFilesystemFormatter.cpp)
target_include_directories(usb_msc_fs_test.elf PRIVATE
    ${USBHOST} ${USBHOST}/utility
    $ENV{HOME}/Development/SdFat/src
    $ENV{HOME}/Development/SPI)

target_link_libraries(usb_msc_fs_test.elf stdc++)
