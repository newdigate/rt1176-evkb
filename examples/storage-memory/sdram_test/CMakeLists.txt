cmake_minimum_required(VERSION 3.24)
project(sdram_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../../../cores/imxrt1176)

teensy_add_executable(sdram_test sdram_test.cpp)
teensy_target_link_libraries(sdram_test cores)

target_link_libraries(sdram_test.elf stdc++)
