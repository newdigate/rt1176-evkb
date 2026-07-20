cmake_minimum_required(VERSION 3.24)
project(ssd1306_display)

set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../../../cores/imxrt1176)
import_arduino_library(Wire $ENV{HOME}/Development/Wire)

teensy_add_executable(ssd1306_display ssd1306_display.cpp)
teensy_target_link_libraries(ssd1306_display cores Wire)

target_link_libraries(ssd1306_display.elf stdc++)
