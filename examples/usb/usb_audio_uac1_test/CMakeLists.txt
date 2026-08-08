cmake_minimum_required(VERSION 3.24)
project(usb_audio_uac1_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(EEPROM EVKB_EEPROM_DIR)
evkb_library_dir(SPI EVKB_SPI_DIR)
evkb_library_dir(SdFat EVKB_SDFAT_DIR)
evkb_library_dir(USBHost_t36 EVKB_USBHOST_T36_DIR)

teensy_add_executable(usb_audio_uac1_test usb_audio_uac1_test.cpp)
teensy_target_link_libraries(usb_audio_uac1_test cores)

# --- USBHost_t36 (UAC1 enumeration) ------------------------------------------
# Compiled straight into the .elf so they pick up teensy_flags -> __IMXRT1176__
# and the core include dirs, same as usb_host_hid_test.
#
# Source set is the transport core plus the UAC1 driver.  Deliberately EXCLUDED:
# hid/keyboard/mouse/joystick/midi/serial/MassStorageDriver/antplus/adk/rawhid/
# digitizer/SerEMU/USBFilesystemFormatter.  bluetooth.cpp is NOT needed here --
# unlike keyboard.cpp/mouse.cpp, usb_audio.cpp hard-references no
# BluetoothController symbols.
#
# usb_audio_feedback / usb_audio2_parse / usb_audio_ctrl are not optional even
# for a UAC1-only sketch: usb_audio.cpp references all three unconditionally.
# They arrived with the feedback work (USBHost_t36 99cd466) and this list was
# not extended, so the example stopped linking then and stayed that way until
# 2026-08-05 -- nothing builds it but a person, and no gate covers it, because
# QEMU has no UAC device model to enumerate.
set(USBHOST ${EVKB_USBHOST_T36_DIR})
target_sources(usb_audio_uac1_test.elf PRIVATE
    ${USBHOST}/ehci.cpp ${USBHOST}/ehci_iso.cpp ${USBHOST}/enumeration.cpp ${USBHOST}/hub.cpp
    ${USBHOST}/memory.cpp ${USBHOST}/print.cpp
    ${USBHOST}/usb_audio.cpp ${USBHOST}/usb_audio_parse.cpp
    ${USBHOST}/usb_audio_fifo.cpp ${USBHOST}/usb_audio_feedback.cpp
    ${USBHOST}/usb_audio2_parse.cpp ${USBHOST}/usb_audio_ctrl.cpp)

# USBHost_t36.h unconditionally does `#include <FS.h>` (resolves from the core)
# AND `#include <SdFat.h>`.  SdFat.h uses relative internal includes, so its
# `src` root alone resolves the whole tree; the only external header it drags in
# is SPI.h.  INCLUDE dirs only -- no SdFat/SPI sources are compiled or linked,
# since only the excluded MassStorageDriver.cpp references those symbols.
target_include_directories(usb_audio_uac1_test.elf PRIVATE
    ${USBHOST} ${USBHOST}/utility
    ${EVKB_SDFAT_DIR}/src
    ${EVKB_SPI_DIR}
    ${EVKB_EEPROM_DIR})

target_link_libraries(usb_audio_uac1_test.elf stdc++)
