cmake_minimum_required(VERSION 3.24)
project(usb_audio_duplex_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(EEPROM EVKB_EEPROM_DIR)
evkb_library_dir(SPI EVKB_SPI_DIR)
evkb_library_dir(SdFat EVKB_SDFAT_DIR)
evkb_library_dir(USBHost_t36 EVKB_USBHOST_T36_DIR)

teensy_add_executable(usb_audio_duplex_test usb_audio_duplex_test.cpp)
teensy_target_link_libraries(usb_audio_duplex_test cores)

# Duplex format. The INPUT alt must match an alternate setting the device
# advertises exactly -- the driver refuses to approximate. Duplex does not
# imply symmetric and both bench devices prove it: the MC200 plays 8ch/16 or
# 8ch/24 and captures 8ch/24 only; the dongle plays 2ch/16 and captures 1ch/16.
set(DUPLEX_RATE_HZ "" CACHE STRING "Rate for both directions; empty = 44100")
set(DUPLEX_IN_CHANNELS "" CACHE STRING "Device capture channels; empty = 8")
set(DUPLEX_IN_BITS "" CACHE STRING "Device capture bit resolution; empty = 24")
set(DUPLEX_TAKE_CHANNELS "" CACHE STRING "Channels delivered to the sketch; empty = 2")
# ECHO: play what arrives on IN back out on OUT, instead of the driver's tone
# generator. Off by default so the QEMU gate and the Stage C transcript's
# measurements keep describing the same image they were taken from.
option(DUPLEX_ECHO "Echo the input stream back out (monitor path)" OFF)
if(DUPLEX_ECHO)
    target_compile_definitions(usb_audio_duplex_test.elf PRIVATE DUPLEX_ECHO=1)
endif()

foreach(opt DUPLEX_RATE_HZ DUPLEX_IN_CHANNELS DUPLEX_IN_BITS DUPLEX_TAKE_CHANNELS)
    if(NOT ${opt} STREQUAL "")
        target_compile_definitions(usb_audio_duplex_test.elf PRIVATE ${opt}=${${opt}})
    endif()
endforeach()

# --- USBHost_t36 --------------------------------------------------------------
set(USBHOST ${EVKB_USBHOST_T36_DIR})
target_sources(usb_audio_duplex_test.elf PRIVATE
    ${USBHOST}/ehci.cpp ${USBHOST}/ehci_iso.cpp ${USBHOST}/enumeration.cpp ${USBHOST}/hub.cpp
    ${USBHOST}/memory.cpp
    ${USBHOST}/usb_audio.cpp ${USBHOST}/usb_audio_parse.cpp
    ${USBHOST}/usb_audio_fifo.cpp ${USBHOST}/usb_audio_feedback.cpp
    ${USBHOST}/usb_audio2_parse.cpp ${USBHOST}/usb_audio_ctrl.cpp)

# USBHost_t36.h unconditionally includes <FS.h> (from the core) and <SdFat.h>.
# Include dirs only -- no SdFat/SPI sources are compiled or linked, since only
# the excluded MassStorageDriver.cpp references those symbols.
target_include_directories(usb_audio_duplex_test.elf PRIVATE
    ${USBHOST} ${USBHOST}/utility
    ${EVKB_SDFAT_DIR}/src
    ${EVKB_SPI_DIR}
    ${EVKB_EEPROM_DIR})

target_link_libraries(usb_audio_duplex_test.elf stdc++)
target_link_libraries(usb_audio_duplex_test.elf m)
