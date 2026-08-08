cmake_minimum_required(VERSION 3.24)
project(usb_descriptor_survey)

# Fallback only: the toolchain file sets this FORCE-fully, so a toolchain-driven
# build wins. A bare `cmake -B build .` with no toolchain still selects 117.
if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(EEPROM EVKB_EEPROM_DIR)
evkb_library_dir(SPI EVKB_SPI_DIR)
evkb_library_dir(SdFat EVKB_SDFAT_DIR)
evkb_library_dir(USBHost_t36 EVKB_USBHOST_T36_DIR)

teensy_add_executable(usb_descriptor_survey usb_descriptor_survey.cpp)
teensy_target_link_libraries(usb_descriptor_survey cores)

# --- USBHost_t36 (transport core only) ---------------------------------------
# The survey driver never binds and never transfers, so it needs only the
# enumeration path: no class drivers at all.  ehci_iso.cpp is required because
# ehci.cpp calls sitd_skip_iso() from add_qh_to_periodic_schedule().
#
# usb_audio.cpp is deliberately absent: USBHost_t36.h declares USBAudioOut, but
# nothing here instantiates it, so no symbol is referenced and --gc-sections
# drops it.
set(USBHOST ${EVKB_USBHOST_T36_DIR})
target_sources(usb_descriptor_survey.elf PRIVATE
    ${USBHOST}/ehci.cpp ${USBHOST}/ehci_iso.cpp ${USBHOST}/enumeration.cpp
    ${USBHOST}/hub.cpp ${USBHOST}/memory.cpp ${USBHOST}/print.cpp)

# print.cpp is listed even though this example does not normally print: its
# ENTIRE body is inside `#ifdef USBHOST_PRINT_DEBUG`, so with debug off it
# compiles to an object defining no symbols and costs nothing. Omitting it is
# what made USBHOST_PRINT_DEBUG unusable tree-wide -- the other .cpp files call
# print_/print_device_descriptor/... under that same #ifdef, so switching debug
# on produced a wall of undefined references instead of debug output. Build the
# diagnostic image with:
#   cmake -B build-dbg -DEVKB_BOARD=rt1062 \
#     -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1062-evkb.toolchain.cmake \
#     -DCMAKE_CXX_FLAGS="-DUSBHOST_PRINT_DEBUG -DUSBHDBGSerial=Serial6"
# USBHDBGSerial defaults to `Serial` (the USB *device* CDC), which is not the
# console on either board -- name the board's console UART explicitly.

# USBHost_t36.h unconditionally includes <FS.h> (from the core) and <SdFat.h>.
# Include dirs only -- no SdFat/SPI sources are compiled or linked, since only
# the excluded MassStorageDriver.cpp references those symbols.
target_include_directories(usb_descriptor_survey.elf PRIVATE
    ${USBHOST} ${USBHOST}/utility
    ${EVKB_SDFAT_DIR}/src
    ${EVKB_SPI_DIR}
    ${EVKB_EEPROM_DIR})

target_link_libraries(usb_descriptor_survey.elf stdc++)
