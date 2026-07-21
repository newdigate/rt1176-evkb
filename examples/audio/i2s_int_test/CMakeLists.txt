cmake_minimum_required(VERSION 3.24)
project(i2s_int_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
# This firmware compiles SAI I/O node sources: it claims audio for the CM7
# (spec §4 -- exactly one core owns audio per firmware).
evkb_claim_audio_owner(CM7)
evkb_library_dir(Audio EVKB_AUDIO_DIR)
import_evkb_library(Wire)
teensy_add_executable(i2s_int_test i2s_int_test.cpp)
teensy_target_link_libraries(i2s_int_test cores Wire)

# Task 1 gate (Plan 2): sine -> AudioOutputI2SInt (interrupt-driven SAI1 TX,
# no DMA) + AudioInputI2SInt -> peak, WM8962 via AudioControlWM8962 + Wire.
# Audio sources are added directly (audiooutput_i2s_test pattern); the int
# nodes and the sai1176 shared C core need no memcpy_audio.S (plain loops).
target_sources(i2s_int_test.elf PRIVATE
    i2s_int_test.cpp
    ${EVKB_AUDIO_DIR}/synth_sine.cpp
    ${EVKB_AUDIO_DIR}/data_waveforms.c
    ${EVKB_AUDIO_DIR}/analyze_peak.cpp
    ${EVKB_AUDIO_DIR}/sai1176.c
    ${EVKB_AUDIO_DIR}/output_i2s_int.cpp
    ${EVKB_AUDIO_DIR}/input_i2s_int.cpp
    ${EVKB_AUDIO_DIR}/control_wm8962.cpp
)
target_include_directories(i2s_int_test.elf PRIVATE ${EVKB_AUDIO_DIR})

target_link_libraries(i2s_int_test.elf stdc++)
target_link_libraries(i2s_int_test.elf m)
