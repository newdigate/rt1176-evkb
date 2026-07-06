cmake_minimum_required(VERSION 3.24)
project(audiooutput_i2s_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)
import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)
teensy_add_executable(audiooutput_i2s_test audiooutput_i2s_test.cpp)
teensy_target_link_libraries(audiooutput_i2s_test cores)

# Task 1 gate: prove AudioSynthWaveformSine compiles + runs on RT1176 (the
# dspinst.h __ARM_ARCH_7EM__ path) via synth -> analyze_peak. No peripheral
# yet -- output_i2s/control_wm8962 come in Task 3.
target_sources(audiooutput_i2s_test.elf PRIVATE
    audiooutput_i2s_test.cpp
    $ENV{HOME}/Development/Audio/synth_sine.cpp
    $ENV{HOME}/Development/Audio/data_waveforms.c
    $ENV{HOME}/Development/Audio/analyze_peak.cpp
)
target_include_directories(audiooutput_i2s_test.elf PRIVATE $ENV{HOME}/Development/Audio)

target_link_libraries(audiooutput_i2s_test.elf stdc++)
target_link_libraries(audiooutput_i2s_test.elf m)
