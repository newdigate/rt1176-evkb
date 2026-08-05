cmake_minimum_required(VERSION 3.24)
project(corpus_host)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../../evkb.cmake)
evkb_library_dir(EEPROM EVKB_EEPROM_DIR)
evkb_library_dir(SPI EVKB_SPI_DIR)
evkb_library_dir(SdFat EVKB_SDFAT_DIR)
evkb_library_dir(USBHost_t36 EVKB_USBHOST_T36_DIR)

teensy_add_executable(corpus_host corpus_host.cpp)
teensy_target_link_libraries(corpus_host cores)

# --- USBHost_t36 sources, per-commit ------------------------------------------
# The corpus builds this against checkouts spanning months of history, and the
# driver's FILE LIST changed over that span as much as its API did:
# usb_audio_feedback.cpp arrives with the UAC1 feedback read (99cd466),
# usb_audio2_parse.cpp with UAC2, usb_audio_ctrl.cpp with the control watchdog
# (46c3daf). A fixed list would fail to configure at exactly the commits the
# corpus exists to exercise.
#
# So: name every source the driver has ever had, compile the ones that exist.
# EXISTS is checked at configure time, which is why each case does a fresh
# `cmake -B` after checking out its commit rather than reusing a build dir.
set(USBHOST ${EVKB_USBHOST_T36_DIR})
set(_candidates
    ehci.cpp ehci_iso.cpp enumeration.cpp hub.cpp memory.cpp
    usb_audio.cpp usb_audio_parse.cpp usb_audio_fifo.cpp
    usb_audio_feedback.cpp usb_audio2_parse.cpp usb_audio_ctrl.cpp)
set(_present "")
foreach(_src ${_candidates})
    if(EXISTS ${USBHOST}/${_src})
        list(APPEND _present ${USBHOST}/${_src})
    endif()
endforeach()
target_sources(corpus_host.elf PRIVATE ${_present})
message(STATUS "corpus_host: USBHost_t36 sources compiled: ${_present}")

# USBHost_t36.h unconditionally includes <FS.h> and <SdFat.h>. Include dirs
# only -- no SdFat/SPI sources are compiled, since only the excluded
# MassStorageDriver.cpp references those symbols.
target_include_directories(corpus_host.elf PRIVATE
    ${USBHOST} ${USBHOST}/utility
    ${EVKB_SDFAT_DIR}/src
    ${EVKB_SPI_DIR}
    ${EVKB_EEPROM_DIR})

target_link_libraries(corpus_host.elf stdc++)
target_link_libraries(corpus_host.elf m)
