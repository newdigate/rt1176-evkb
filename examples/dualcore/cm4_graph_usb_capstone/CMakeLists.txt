cmake_minimum_required(VERSION 3.24)
project(cm4_graph_usb_capstone)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(USBHost_t36 EVKB_USBHOST_T36_DIR)
evkb_library_dir(Audio       EVKB_AUDIO_DIR)

# Phase 7.4, the CAPSTONE: an AudioStream GRAPH on the CM4 feeding the USB
# isochronous stream the same core drives.
#
# 7.3 (cm4_usb_audio_probe) is the base image, unchanged: the same transport
# core and the same six UAC sources.  Four things are added on top, and the
# AUDIO LIBRARY IS NO LONGER ABSENT -- that absence was 7.3's whole point (a red
# run there could not be the graph's fault) and removing it is this phase.
#
#   AudioStream.cpp   the core graph engine.  Phase 5 (cm4_audiostream_test)
#                     proved it compiles and runs UNMODIFIED on the M4 against
#                     the cm4_shim Arduino-lite header.
#   output_usbhost    AudioOutputUSBHost, the adapter: an AudioStream sink that
#                     copies interleaved blocks into USBAudioOut's sample FIFO
#                     and, via onFrameConsumed(), makes the USB frame clock the
#                     graph's clock.
#   synth_sine        the source, plus data_waveforms.c for the 257-entry
#                     AudioWaveformSine table it declares extern.
#   analyze_peak      the audio oracle.  Blocks flowing and a FIFO filling look
#                     identical whether the samples are a sine or silence; this
#                     is what tells them apart, and it is pure CPU so it works
#                     in QEMU as well as on silicon.
#
# The six USBHost_t36 audio sources are NOT independently optional (7.3's note):
# usb_audio.cpp references all of them unconditionally, UAC2 parser and feedback
# endpoint included, even in a UAC1-only image.  Listed together as one unit.
#
# ★ NO fader table, no other Audio sources.  data_waveforms.c also defines
# fader_table[257], which nothing here references; -fdata-sections plus
# --gc-sections drops it, so the cost is the sine table alone (514 B).

# PROBE_RATE_HZ: the rate asked of the device.  Default 48000 because that is
# the ONLY rate QEMU's emulated usb-audio offers (USBAUDIO_SAMPLE_RATE is a
# compile-time #define in its descriptor tables), so the QEMU gate must use it.
# Real devices generally differ -- the UAC1 dongle this tree tests against is
# 44100 -- so a silicon run needs -DPROBE_RATE_HZ=44100.
#
# ★★ IN THIS EXAMPLE IT MOVES TWO THINGS, and they are not allowed to diverge.
# AudioOutputUSBHost's constructor forces the USB rate to the GRAPH's rate
# ((uint32_t)AUDIO_SAMPLE_RATE_EXACT) rather than trusting the sketch to match
# it -- a graph producing 44100 into a stream sized for 48000 is 8.8% sharp and
# drains the FIFO for good.  So the graph rate is the master, and this option
# sets BOTH: AUDIO_SAMPLE_RATE_EXACT for the CM4 image (which is what the
# adapter reads) and PROBE_RATE_HZ for the CM7 relay (which asserts the adapter
# actually did it).  Setting only one would make the GRAPH_RATE token unable to
# fail, or unable to pass.
#
# It is a cache option rather than a hand-edit of DEFINES below for a reason
# worth recording (carried from 7.3): editing this file and restoring it with
# `mv` preserves the ORIGINAL mtime, so CMake does not re-run and the next build
# silently links a STALE image.  A gate then reports the previous rate's result
# from a tree that looks reverted.  Use the option; do not hand-edit.
set(PROBE_RATE_HZ "" CACHE STRING "Rate for BOTH the graph and the USB stream; empty = 48000 (QEMU's only rate)")
if(PROBE_RATE_HZ STREQUAL "")
    set(_probe_rate 48000)
else()
    set(_probe_rate ${PROBE_RATE_HZ})
endif()
# AUDIO_SAMPLE_RATE_EXACT is compared as a float (AudioStream.h defaults it to
# 44100.0f), so it must be spelled as a float literal, not an integer.
set(_rate_defs PROBE_RATE_HZ=${_probe_rate}u
               AUDIO_SAMPLE_RATE_EXACT=${_probe_rate}.0f)

teensy_add_cm4_image(cm4_graph_usb
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
            ${EVKB_CORES_DIR}/AudioStream.cpp
            ${EVKB_AUDIO_DIR}/output_usbhost.cpp
            ${EVKB_AUDIO_DIR}/synth_sine.cpp
            ${EVKB_AUDIO_DIR}/analyze_peak.cpp
            ${EVKB_AUDIO_DIR}/data_waveforms.c
    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/cm4
                 ${EVKB_CORES_DIR}/cm4_shim
                 ${EVKB_CORES_DIR}
                 ${EVKB_USBHOST_T36_DIR}
                 ${EVKB_USBHOST_T36_DIR}/utility
                 ${EVKB_AUDIO_DIR}
    DEFINES  __IMXRT1176__ USBHOST_NO_MASSSTORAGE
             EVKB_CM4_SOC_REGS EVKB_CM4_ARDUINO_CXX ${_rate_defs})

# INCLUDE_DIRS ordering and the two EVKB_CM4_* opt-ins are 7.2's and are
# explained there (cm4_usb_enum_probe/CMakeLists.txt) -- gate's own cm4/ first,
# then the shim so <Arduino.h> is the Arduino-lite one, then the core for the
# register map AND for AudioStream.h, then USBHost_t36, then the Audio library.
# The Audio dir goes LAST for the same reason the shim goes early: synth_sine.h
# and analyze_peak.h both open with `#include <Arduino.h>`, and the shim must
# win that.  It also has to be present at all, because synth_sine.cpp includes
# "utility/dspinst.h", which is Audio's, not USBHost_t36's.
#
# Still ABSENT, and still the thing to check if it reappears: SdFat, SPI and
# EEPROM.  USBHOST_NO_MASSSTORAGE compiles out the only block that ever needed a
# filesystem library.

teensy_add_executable(cm4_graph_usb_capstone cm4_graph_usb_capstone.cpp)
teensy_target_link_libraries(cm4_graph_usb_capstone cores)
target_link_libraries(cm4_graph_usb_capstone.elf stdc++)
# The relay asserts the rate the CM4's ADAPTER configured, so it needs the same
# build-time expectation.  Nothing else about the Audio library reaches the CM7
# image: it links no Audio source and constructs no AudioStream node.
target_compile_definitions(cm4_graph_usb_capstone.elf PRIVATE PROBE_RATE_HZ=${_probe_rate}u)
teensy_target_link_cm4_image(cm4_graph_usb_capstone cm4_graph_usb)
