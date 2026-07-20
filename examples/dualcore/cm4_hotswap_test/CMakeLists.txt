cmake_minimum_required(VERSION 3.24)
project(cm4_hotswap_test)
set(TEENSY_VERSION 117 CACHE STRING "")
include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../../../cores/imxrt1176)

teensy_add_cm4_image(cm4_hs_a
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
    DEFINES HS_IDENTITY=0xA1A1A1A1u)
teensy_add_cm4_image(cm4_hs_b
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
    DEFINES HS_IDENTITY=0xB2B2B2B2u)

teensy_add_executable(cm4_hotswap_test cm4_hotswap_test.cpp)
teensy_target_link_libraries(cm4_hotswap_test cores)
target_link_libraries(cm4_hotswap_test.elf stdc++)
teensy_target_link_cm4_image(cm4_hotswap_test cm4_hs_a)
teensy_target_link_cm4_image(cm4_hotswap_test cm4_hs_b)
