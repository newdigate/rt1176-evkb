cmake_minimum_required(VERSION 3.24)
project(cm4_usb_enum_probe)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(USBHost_t36 EVKB_USBHOST_T36_DIR)

# Phase 7.2c: the REAL USBHost_t36 enumeration path, compiled into the CM4
# image.  Not a distillation of it -- cm4_usb_irq_probe already did the
# register sequence by hand; this compiles the library itself.
#
# SOURCES: the transport core only -- exactly the set usb_descriptor_survey
# links on the CM7, plus usbhost_isr_entry.cpp.  No class drivers at all: no
# audio, HID, MIDI, bluetooth or mass storage.
#
# usbhost_isr_entry.cpp is the CM4-specific one: the plain C symbol the STATIC
# vector table names, because USBHost::isr() is private and C++-mangled and
# attachInterruptVector() is a documented no-op in this world.
#
# ehci_iso.cpp is here despite this probe running no isochronous schedule, and
# that is NOT optional: ehci.cpp's add_qh_to_periodic_schedule() calls
# sitd_skip_iso() (ehci.cpp:1321), and that function is reached from
# allocate_interrupt_pipe_bandwidth() <- new_Pipe(), so --gc-sections keeps it
# for any image that can open a pipe at all -- which is every image that
# enumerates.  Dropping it is a link error, not a size saving.  Its cost is the
# siTD/iTD pools in OCRAM2 (USBHOST_DMAMEM, KEEP()d), which is why the DMAMEM
# figures below are larger than an enumeration-only probe suggests.
#
# INCLUDE_DIRS ordering is load-bearing: the gate's own cm4/ first, then
# cm4_shim (so <Arduino.h> resolves to the Arduino-lite shim), then the core
# (register definitions), then USBHost_t36 and its utility/.  Notably ABSENT:
# SdFat, SPI and EEPROM.  The CM7 examples still need those three purely to
# PARSE USBHost_t36.h; USBHOST_NO_MASSSTORAGE below compiles out the block that
# derives from FS, which is the only thing that ever required them.  If a
# filesystem include directory reappears here, something has regressed.
teensy_add_cm4_image(cm4_usb_enum
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.cpp
            ${CMAKE_CURRENT_LIST_DIR}/cm4/runtime_stubs.c
            ${EVKB_USBHOST_T36_DIR}/ehci.cpp
            ${EVKB_USBHOST_T36_DIR}/ehci_iso.cpp
            ${EVKB_USBHOST_T36_DIR}/enumeration.cpp
            ${EVKB_USBHOST_T36_DIR}/hub.cpp
            ${EVKB_USBHOST_T36_DIR}/memory.cpp
            ${EVKB_USBHOST_T36_DIR}/usbhost_isr_entry.cpp
    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/cm4
                 ${EVKB_CORES_DIR}/cm4_shim
                 ${EVKB_CORES_DIR}
                 ${EVKB_USBHOST_T36_DIR}
                 ${EVKB_USBHOST_T36_DIR}/utility
    DEFINES  __IMXRT1176__ USBHOST_NO_MASSSTORAGE
             EVKB_CM4_SOC_REGS EVKB_CM4_ARDUINO_CXX)

# The last two are OPT-INs into blocks of cm4_shim/Arduino.h that are OFF by
# default, and they are named separately rather than bundled because they are
# different kinds of exception to the shim's own never-list (see its README):
#
#   EVKB_CM4_SOC_REGS    pulls in imxrt1176.h for the USB register map.  A
#                        library reaches hardware through the Teensy-idiom
#                        macros; re-spelling ~70 of them as CM4-local literals
#                        would be a second source of truth for addresses that
#                        must not drift.
#   EVKB_CM4_ARDUINO_CXX defines abstract Print/Stream + elapsedMillis/Micros
#                        so USBHost_t36.h PARSES (USBSerialBase and
#                        USBSerialEmu derive from Stream).  No vtable is
#                        emitted and nothing here can print.
#
# An image that forgets them fails with "'USB2_USBCMD' was not declared" or
# "expected class-name before '{'" -- errors that point at the right place.

teensy_add_executable(cm4_usb_enum_probe cm4_usb_enum_probe.cpp)
teensy_target_link_libraries(cm4_usb_enum_probe cores)
target_link_libraries(cm4_usb_enum_probe.elf stdc++)
teensy_target_link_cm4_image(cm4_usb_enum_probe cm4_usb_enum)
