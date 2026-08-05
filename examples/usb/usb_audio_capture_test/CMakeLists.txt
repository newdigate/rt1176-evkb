cmake_minimum_required(VERSION 3.24)
project(usb_audio_capture_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(EEPROM EVKB_EEPROM_DIR)
evkb_library_dir(SPI EVKB_SPI_DIR)
evkb_library_dir(SdFat EVKB_SDFAT_DIR)
evkb_library_dir(USBHost_t36 EVKB_USBHOST_T36_DIR)

teensy_add_executable(usb_audio_capture_test usb_audio_capture_test.cpp)
teensy_target_link_libraries(usb_audio_capture_test cores)

# Capture format. Must match an alternate setting the device advertises on
# its INPUT interface exactly -- the driver refuses to approximate, because a
# format the device never offered is a negotiation error and not a rounding
# one. The MC200 witness captures 8ch/24 only.
set(CAPTURE_RATE_HZ "" CACHE STRING "Capture sample rate; empty = 44100")
set(CAPTURE_CHANNELS "" CACHE STRING "Device capture channels; empty = 8")
set(CAPTURE_BITS "" CACHE STRING "Device capture bit resolution; empty = 24")
set(CAPTURE_TAKE_CHANNELS "" CACHE STRING "Channels delivered to the sketch; empty = 2")
foreach(opt CAPTURE_RATE_HZ CAPTURE_CHANNELS CAPTURE_BITS CAPTURE_TAKE_CHANNELS)
    if(NOT ${opt} STREQUAL "")
        target_compile_definitions(usb_audio_capture_test.elf PRIVATE ${opt}=${${opt}})
    endif()
endforeach()

# --- USBHost_t36 --------------------------------------------------------------
# Same source set as usb_audio_graph_test. usb_audio_feedback/usb_audio2_parse/
# usb_audio_ctrl are not optional even though nothing streams OUT here:
# usb_audio.cpp references all three unconditionally.
set(USBHOST ${EVKB_USBHOST_T36_DIR})
target_sources(usb_audio_capture_test.elf PRIVATE
    ${USBHOST}/ehci.cpp ${USBHOST}/ehci_iso.cpp ${USBHOST}/enumeration.cpp ${USBHOST}/hub.cpp
    ${USBHOST}/memory.cpp
    ${USBHOST}/usb_audio.cpp ${USBHOST}/usb_audio_parse.cpp
    ${USBHOST}/usb_audio_fifo.cpp ${USBHOST}/usb_audio_feedback.cpp
    ${USBHOST}/usb_audio2_parse.cpp ${USBHOST}/usb_audio_ctrl.cpp)

# USBHost_t36.h unconditionally includes <FS.h> (from the core) and <SdFat.h>.
# Include dirs only -- no SdFat/SPI sources are compiled or linked, since only
# the excluded MassStorageDriver.cpp references those symbols.
target_include_directories(usb_audio_capture_test.elf PRIVATE
    ${USBHOST} ${USBHOST}/utility
    ${EVKB_SDFAT_DIR}/src
    ${EVKB_SPI_DIR}
    ${EVKB_EEPROM_DIR})

target_link_libraries(usb_audio_capture_test.elf stdc++)
target_link_libraries(usb_audio_capture_test.elf m)
