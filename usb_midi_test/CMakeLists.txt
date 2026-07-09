cmake_minimum_required(VERSION 3.24)
project(usb_midi_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)

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
set(USBHOST $ENV{HOME}/Development/USBHost_t36)
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
    $ENV{HOME}/Development/SdFat/src
    $ENV{HOME}/Development/SPI)

target_link_libraries(usb_midi_test.elf stdc++)
