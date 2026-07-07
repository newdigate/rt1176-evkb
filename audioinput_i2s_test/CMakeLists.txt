cmake_minimum_required(VERSION 3.24)
project(audioinput_i2s_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)
import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)
import_arduino_library(Wire $ENV{HOME}/Development/Wire)
teensy_add_executable(audioinput_i2s_test audioinput_i2s_test.cpp)
teensy_target_link_libraries(audioinput_i2s_test cores Wire)

# Task 4 integration gate: pull in the fork's audio nodes directly (they live
# outside the core's glob, in ~/Development/Audio). config_i2s (output_i2s.cpp)
# is linked in even though this gate has no AudioOutputI2S instance, because
# AudioInputI2S::begin() calls AudioOutputI2S::config_i2s() to bring up SAI1 TX
# (RX runs synchronous to TX -- see input_i2s.cpp).
target_sources(audioinput_i2s_test.elf PRIVATE
    $ENV{HOME}/Development/Audio/input_i2s.cpp
    $ENV{HOME}/Development/Audio/output_i2s.cpp
    $ENV{HOME}/Development/Audio/control_wm8962.cpp
    $ENV{HOME}/Development/Audio/analyze_peak.cpp)
target_include_directories(audioinput_i2s_test.elf PRIVATE $ENV{HOME}/Development/Audio)

target_link_libraries(audioinput_i2s_test.elf stdc++)
target_link_libraries(audioinput_i2s_test.elf m)
