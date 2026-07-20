cmake_minimum_required(VERSION 3.24)
project(usb_midi_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(SPI EVKB_SPI_DIR)
evkb_library_dir(SdFat EVKB_SDFAT_DIR)
evkb_library_dir(USBHost_t36 EVKB_USBHOST_T36_DIR)


teensy_add_executable(usb_midi_test usb_midi_test.cpp)
teensy_target_link_libraries(usb_midi_test cores)

# --- USBHost_t36 (Sub-project B: USB-MIDI host) ------------------------------
# The USBHost_t36 sources are compiled straight into the .elf (teensy_add_executable
# names the target <name>.elf) so they pick up teensy_flags -> __IMXRT1176__ + the
# core include dirs.  MIDI-only set: ehci/enumeration/hub/memory + midi.cpp.  NO
# hid/keyboard/mouse/joystick/MassStorageDriver/serial/antplus/adk/rawhid/digitizer.
# bluetooth.cpp is DROPPED (plan O3): unlike keyboard.cpp/mouse.cpp, MIDIDevice and
# USBHub carry NO BluetoothController references (verified -- no driver_ready_for_
# bluetooth / sendL2CapCommand / BTHIDInput across the MIDI-only source set), so the
# link needs no BluetoothController symbols.  This is the simplification the HID gate
# could not take.
set(USBHOST ${EVKB_USBHOST_T36_DIR})
target_sources(usb_midi_test.elf PRIVATE
    ${USBHOST}/ehci.cpp ${USBHOST}/enumeration.cpp ${USBHOST}/hub.cpp
    ${USBHOST}/memory.cpp ${USBHOST}/midi.cpp)

# USBHost_t36.h unconditionally does `#include <FS.h>` (resolves from the core,
# present from the SD work) AND `#include <SdFat.h>`.  SdFat.h uses relative
# internal includes, so its `src` root alone resolves the whole tree; the only
# external header it drags in is SPI.h (SdSpiDriver.h, default SPI_DRIVER_SELECT).
# These are INCLUDE dirs only -- no SdFat/SPI sources are compiled or linked: the
# USBHost_t36 sources we build reference no SdFat/SPI symbols (only the excluded
# MassStorageDriver.cpp does), so the header just needs to parse.  The EEPROM
# include dir is NOT needed (only bluetooth.cpp #includes <EEPROM.h>, and that is
# dropped from this MIDI-only gate).
target_include_directories(usb_midi_test.elf PRIVATE
    ${USBHOST} ${USBHOST}/utility
    ${EVKB_SDFAT_DIR}/src
    ${EVKB_SPI_DIR})

target_link_libraries(usb_midi_test.elf stdc++)
