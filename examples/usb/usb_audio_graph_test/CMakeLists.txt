cmake_minimum_required(VERSION 3.24)
project(usb_audio_graph_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(EEPROM EVKB_EEPROM_DIR)
evkb_library_dir(SPI EVKB_SPI_DIR)
evkb_library_dir(SdFat EVKB_SDFAT_DIR)
evkb_library_dir(USBHost_t36 EVKB_USBHOST_T36_DIR)
evkb_library_dir(Audio EVKB_AUDIO_DIR)

teensy_add_executable(usb_audio_graph_test usb_audio_graph_test.cpp)
teensy_target_link_libraries(usb_audio_graph_test cores)

# --- rate-bias override for the drift harness ---------------------------------
# tools/driftrun.sh rebuilds per sweep point with a locked trim:
#   cmake -B build -DBIAS_LOCKED_PPM=-83 && cmake --build build
# Locking here (rather than editing the source) keeps every point's provenance
# in the build command, and BIAS_MODE_LOCKED forces the sweep/AB machinery off
# so a stale cache cannot leave two mechanisms fighting over the trim.
set(BIAS_LOCKED_PPM "" CACHE STRING "Lock the host rate trim to this ppm; empty = source default")
if(NOT BIAS_LOCKED_PPM STREQUAL "")
    target_compile_definitions(usb_audio_graph_test.elf PRIVATE
        BIAS_LOCKED_PPM=${BIAS_LOCKED_PPM} BIAS_MODE_LOCKED=1)
endif()

# Print the last-claimed device's raw config descriptors once, from loop()
# context (fixture capture for the UAC2 parser; see the DUMP_CONFIG block in
# the sketch). Default off so driftrun's console greps stay stable.
option(DUMP_CONFIG "Hex-dump the last device's configuration descriptors" OFF)
if(DUMP_CONFIG)
    target_compile_definitions(usb_audio_graph_test.elf PRIVATE DUMP_CONFIG=1)
endif()

# Cooperative mode for the UAC host validator: the payload becomes the
# validator's LFSR sequence so a UACV_COOPERATIVE=1 device build can judge
# sample continuity (rule R7). Off by default -- the pattern is full-scale
# noise at the device's outputs.
option(UACV_PATTERN "Send the UAC validator LFSR pattern instead of audio" OFF)
if(UACV_PATTERN)
    target_compile_definitions(usb_audio_graph_test.elf PRIVATE UACV_PATTERN=1)
endif()

# --- USBHost_t36 (UAC1 output) ------------------------------------------------
# Same source set as usb_audio_uac1_test: transport core plus the UAC1 driver.
set(USBHOST ${EVKB_USBHOST_T36_DIR})
target_sources(usb_audio_graph_test.elf PRIVATE
    ${USBHOST}/ehci.cpp ${USBHOST}/ehci_iso.cpp ${USBHOST}/enumeration.cpp ${USBHOST}/hub.cpp
    ${USBHOST}/memory.cpp
    ${USBHOST}/usb_audio.cpp ${USBHOST}/usb_audio_parse.cpp
    ${USBHOST}/usb_audio_fifo.cpp ${USBHOST}/usb_audio_feedback.cpp
    ${USBHOST}/usb_audio2_parse.cpp ${USBHOST}/usb_audio_ctrl.cpp)

# --- Audio graph --------------------------------------------------------------
# Only the nodes this graph actually uses, rather than import_evkb_audio_full():
# the sine source and the USB host sink. AudioStream itself lives in the core,
# so it arrives with `cores` above.
#
# memcpy_audio.S is deliberately absent -- it is needed by output_i2s.cpp's
# interleave path, and nothing here pulls that in. AudioOutputUSBHost
# interleaves in plain C, precisely so this graph has no I2S dependency.
target_sources(usb_audio_graph_test.elf PRIVATE
    ${EVKB_AUDIO_DIR}/synth_sine.cpp
    ${EVKB_AUDIO_DIR}/data_waveforms.c
    ${EVKB_AUDIO_DIR}/output_usbhost.cpp)

# USBHost_t36.h unconditionally includes <FS.h> (from the core) and <SdFat.h>.
# Include dirs only -- no SdFat/SPI sources are compiled or linked, since only
# the excluded MassStorageDriver.cpp references those symbols.
target_include_directories(usb_audio_graph_test.elf PRIVATE
    ${USBHOST} ${USBHOST}/utility
    ${EVKB_AUDIO_DIR}
    ${EVKB_SDFAT_DIR}/src
    ${EVKB_SPI_DIR}
    ${EVKB_EEPROM_DIR})

target_link_libraries(usb_audio_graph_test.elf stdc++)
target_link_libraries(usb_audio_graph_test.elf m)
