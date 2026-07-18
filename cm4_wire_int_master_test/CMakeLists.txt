cmake_minimum_required(VERSION 3.24)
project(cm4_wire_int_master_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)

# CM4 image via the Phase-2B first-class dual-target macro (emits cm4_wire_int_master.h).
teensy_add_cm4_image(cm4_wire_int_master
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
            $ENV{HOME}/Development/Wire/lpi2c1176.c
    INCLUDE_DIRS $ENV{HOME}/Development/Wire)

teensy_add_executable(cm4_wire_int_master_test cm4_wire_int_master_test.cpp)
teensy_target_link_libraries(cm4_wire_int_master_test cores)
target_link_libraries(cm4_wire_int_master_test.elf stdc++)
teensy_target_link_cm4_image(cm4_wire_int_master_test cm4_wire_int_master)
