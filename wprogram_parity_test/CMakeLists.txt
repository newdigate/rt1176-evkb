cmake_minimum_required(VERSION 3.24)
project(wprogram_parity_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)
# Stock, unmodified Bounce2 (MIT) — the "stock Teensy library compiles" smoke test.
import_arduino_library(Bounce2 $ENV{HOME}/Development/Bounce2/src)

teensy_add_executable(wprogram_parity_test wprogram_parity_test.cpp)
teensy_target_link_libraries(wprogram_parity_test Bounce2 cores)

target_link_libraries(wprogram_parity_test.elf stdc++)
