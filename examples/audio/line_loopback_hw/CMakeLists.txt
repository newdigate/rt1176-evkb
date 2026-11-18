cmake_minimum_required(VERSION 3.24)
project(line_loopback_hw)
# Fallback only: the toolchain file sets this FORCE-fully.
if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()
include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# Bench-only (no boards file, no run_qemu.sh -- the sweep does not see it, like
# tone_hw): the assertion is a real cable from line out (J8) to line in (J9)
# on the Nosh board, which QEMU cannot model. Nosh only: it drives the SGTL5000's
# line out and line in, and records into the 128 MB SEMC SDRAM.
if(NOT EVKB_BOARD STREQUAL "nosh")
    message(FATAL_ERROR "line_loopback_hw builds for the Nosh board only:\n"
        "  cmake -B build-nosh -DEVKB_BOARD=nosh "
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_LIST_DIR}/../../../toolchain/nosh.toolchain.cmake")
endif()

evkb_library_dir(Audio EVKB_AUDIO_DIR)
import_evkb_library(Wire)
teensy_add_executable(line_loopback_hw line_loopback_hw.cpp)
teensy_target_link_libraries(line_loopback_hw cores Wire)

# The graph uses AudioOutputI2S and AudioInputI2S TOGETHER, which needs Audio
# >= 0db101d: before it, the RT1176 config_i2s() re-ran from AudioInputI2S::begin()
# and dropped the TX DMA request, so no audio block ever moved.
# memcpy_audio.S: output_i2s.cpp's isr() calls memcpy_tointerleave{LR,L,R}; same
# LANGUAGE C + assembler-with-cpp trick as audiooutput_i2s_test.
target_sources(line_loopback_hw.elf PRIVATE
    ${EVKB_AUDIO_DIR}/input_i2s.cpp
    ${EVKB_AUDIO_DIR}/output_i2s.cpp
    ${EVKB_AUDIO_DIR}/synth_sine.cpp
    ${EVKB_AUDIO_DIR}/data_waveforms.c
    ${EVKB_AUDIO_DIR}/record_queue.cpp
    ${EVKB_AUDIO_DIR}/control_sgtl5000.cpp
    ${EVKB_AUDIO_DIR}/memcpy_audio.S)
set_source_files_properties(${EVKB_AUDIO_DIR}/memcpy_audio.S PROPERTIES
    LANGUAGE C
    COMPILE_OPTIONS "-x;assembler-with-cpp")
target_include_directories(line_loopback_hw.elf PRIVATE ${EVKB_AUDIO_DIR})

target_link_libraries(line_loopback_hw.elf stdc++)
target_link_libraries(line_loopback_hw.elf m)
