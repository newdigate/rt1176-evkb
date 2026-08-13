cmake_minimum_required(VERSION 3.24)
project(usb_audio_capstone_test)

# Fallback only: the toolchain file sets this FORCE-fully, so a toolchain-driven
# build wins. A bare `cmake -B build .` with no toolchain still selects 117.
# Left unguarded this caches 117 and silently builds an RT1176 image into
# build-rt1062/, which boots the wrong QEMU machine and fails looking like a
# board or model problem rather than a build misconfiguration.
if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(EEPROM EVKB_EEPROM_DIR)
evkb_library_dir(SPI EVKB_SPI_DIR)
evkb_library_dir(SdFat EVKB_SDFAT_DIR)
evkb_library_dir(USBHost_t36 EVKB_USBHOST_T36_DIR)
evkb_library_dir(Audio EVKB_AUDIO_DIR)
import_evkb_library(Wire)

teensy_add_executable(usb_audio_capstone_test usb_audio_capstone_test.cpp)
teensy_target_link_libraries(usb_audio_capstone_test cores Wire)

# --- USBHost_t36 (UAC1 duplex) ------------------------------------------------
# Transport core plus the UAC1 driver, the same set usb_audio_uac1_test and
# usb_audio_duplex_test compile. usb_audio_feedback / usb_audio2_parse /
# usb_audio_ctrl are not optional even for a UAC1-only sketch: usb_audio.cpp
# references all three unconditionally.
set(USBHOST ${EVKB_USBHOST_T36_DIR})
target_sources(usb_audio_capstone_test.elf PRIVATE
    ${USBHOST}/ehci.cpp ${USBHOST}/ehci_iso.cpp ${USBHOST}/enumeration.cpp ${USBHOST}/hub.cpp
    ${USBHOST}/memory.cpp ${USBHOST}/print.cpp
    ${USBHOST}/usb_audio.cpp ${USBHOST}/usb_audio_parse.cpp
    ${USBHOST}/usb_audio_fifo.cpp ${USBHOST}/usb_audio_feedback.cpp
    ${USBHOST}/usb_audio2_parse.cpp ${USBHOST}/usb_audio_ctrl.cpp)

# --- Audio graph --------------------------------------------------------------
# Both legs: the sine source and USB sink (as usb_audio_graph_test), plus the
# USB source and I2S sink with its codec (as audiooutput_i2s_test). This is the
# first example to compile all four together.
#
# output_i2s.cpp's isr() calls into memcpy_tointerleave{LR,L,R} (memcpy_audio.S,
# guarded __ARM_ARCH_7EM__ -- defined for -mcpu=cortex-m7 too) which
# import_arduino_library's own .S handling never sees here because these Audio
# sources are added directly, not via that helper's ROOT glob. Mirror its trick
# (LANGUAGE C + assembler-with-cpp) so the plain C compiler assembles it.
#
# utility/imxrt_hw.cpp is unconditional here, unlike the two-board audio
# examples: this example is rt1062-only, and output_i2s.cpp's __IMXRT1062__
# path drives PLL4 through its set_audioClock().
target_sources(usb_audio_capstone_test.elf PRIVATE
    ${EVKB_AUDIO_DIR}/synth_sine.cpp
    ${EVKB_AUDIO_DIR}/data_waveforms.c
    ${EVKB_AUDIO_DIR}/analyze_peak.cpp
    ${EVKB_AUDIO_DIR}/output_usbhost.cpp
    ${EVKB_AUDIO_DIR}/input_usbhost.cpp
    ${EVKB_AUDIO_DIR}/output_i2s.cpp
    ${EVKB_AUDIO_DIR}/control_wm8960.cpp
    ${EVKB_AUDIO_DIR}/utility/imxrt_hw.cpp
    ${EVKB_AUDIO_DIR}/memcpy_audio.S)
set_source_files_properties(${EVKB_AUDIO_DIR}/memcpy_audio.S PROPERTIES
    LANGUAGE C
    COMPILE_OPTIONS "-x;assembler-with-cpp")

# USBHost_t36.h unconditionally includes <FS.h> (from the core) and <SdFat.h>.
# Include dirs only -- no SdFat/SPI sources are compiled or linked, since only
# the excluded MassStorageDriver.cpp references those symbols.
target_include_directories(usb_audio_capstone_test.elf PRIVATE
    ${USBHOST} ${USBHOST}/utility
    ${EVKB_AUDIO_DIR}
    ${EVKB_SDFAT_DIR}/src
    ${EVKB_SPI_DIR}
    ${EVKB_EEPROM_DIR})

target_link_libraries(usb_audio_capstone_test.elf stdc++)
target_link_libraries(usb_audio_capstone_test.elf m)
