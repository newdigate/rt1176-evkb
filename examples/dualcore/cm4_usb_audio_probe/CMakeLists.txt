cmake_minimum_required(VERSION 3.24)
project(cm4_usb_audio_probe)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(USBHost_t36 EVKB_USBHOST_T36_DIR)

# Phase 7.3: the CM4 drives an ISOCHRONOUS OUT STREAM.
#
# 7.2 (cm4_usb_enum_probe) compiled the USBHost_t36 transport core into the CM4
# image and enumerated a device.  This adds the UAC driver on top -- the same
# six sources the CM7's usb_audio_uac1_test links -- so the CM4 claims the
# device, runs the control sequence, arms 32 siTDs and streams.
#
# The six audio sources are NOT independently optional; usb_audio.cpp references
# all of them unconditionally, UAC2 parser and feedback endpoint included, even
# in a UAC1-only image.  usb_audio_uac1_test learned that the expensive way: the
# feedback work added three of them, its source list was not extended, and the
# example simply stopped linking and stayed that way until someone next built it
# by hand -- nothing but a person builds it and no gate covered it.  So they are
# listed together and commented as one unit, rather than split by whether a
# UAC1-only sketch looks like it "needs" them.
#
# Nothing else is added.  In particular the AUDIO LIBRARY IS ABSENT: the stream
# is fed by the driver's own tone generator (usb_audio.cpp topUpFromTone),
# which is a producer into the same FIFO an AudioStream adapter would write to.
# Porting AudioStream to this image is Phase 7.4; doing it here would mean a
# failure could be the graph's rather than the transport's.
teensy_add_cm4_image(cm4_usb_audio
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
            ${EVKB_USBHOST_T36_DIR}/usb_audio.cpp
            ${EVKB_USBHOST_T36_DIR}/usb_audio_parse.cpp
            ${EVKB_USBHOST_T36_DIR}/usb_audio2_parse.cpp
            ${EVKB_USBHOST_T36_DIR}/usb_audio_fifo.cpp
            ${EVKB_USBHOST_T36_DIR}/usb_audio_feedback.cpp
            ${EVKB_USBHOST_T36_DIR}/usb_audio_ctrl.cpp
    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/cm4
                 ${EVKB_CORES_DIR}/cm4_shim
                 ${EVKB_CORES_DIR}
                 ${EVKB_USBHOST_T36_DIR}
                 ${EVKB_USBHOST_T36_DIR}/utility
    DEFINES  __IMXRT1176__ USBHOST_NO_MASSSTORAGE
             EVKB_CM4_SOC_REGS EVKB_CM4_ARDUINO_CXX)

# INCLUDE_DIRS ordering and the two EVKB_CM4_* opt-ins are 7.2's and are
# explained there (cm4_usb_enum_probe/CMakeLists.txt) -- gate's own cm4/ first,
# then the shim so <Arduino.h> is the Arduino-lite one, then the core for the
# register map.  Still ABSENT, and still the thing to check if it reappears:
# SdFat, SPI and EEPROM.  USBHOST_NO_MASSSTORAGE compiles out the only block
# that ever needed a filesystem library.

teensy_add_executable(cm4_usb_audio_probe cm4_usb_audio_probe.cpp)
teensy_target_link_libraries(cm4_usb_audio_probe cores)
target_link_libraries(cm4_usb_audio_probe.elf stdc++)
teensy_target_link_cm4_image(cm4_usb_audio_probe cm4_usb_audio)
