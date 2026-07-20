cmake_minimum_required(VERSION 3.24)
project(usb_msc_fs_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(SPI EVKB_SPI_DIR)
evkb_library_dir(SdFat EVKB_SDFAT_DIR)
evkb_library_dir(USBHost_t36 EVKB_USBHOST_T36_DIR)


import_evkb_library(SdFat
    src src/common src/SdCard src/FatLib src/ExFatLib src/FsLib src/iostream src/SpiDriver)
import_evkb_library(SPI)

teensy_add_executable(usb_msc_fs_test usb_msc_fs_test.cpp)
teensy_target_link_libraries(usb_msc_fs_test cores SdFat SPI)

set(USBHOST ${EVKB_USBHOST_T36_DIR})
target_sources(usb_msc_fs_test.elf PRIVATE
    ${USBHOST}/ehci.cpp ${USBHOST}/enumeration.cpp ${USBHOST}/hub.cpp
    ${USBHOST}/memory.cpp ${USBHOST}/MassStorageDriver.cpp
    ${USBHOST}/USBFilesystemFormatter.cpp)
target_include_directories(usb_msc_fs_test.elf PRIVATE
    ${USBHOST} ${USBHOST}/utility
    ${EVKB_SDFAT_DIR}/src
    ${EVKB_SPI_DIR})

target_link_libraries(usb_msc_fs_test.elf stdc++)
