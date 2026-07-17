cmake_minimum_required(VERSION 3.24)
project(cm4_boot_test)

# TEENSY_VERSION / CPU_CORE_SPEED / COMPILERPATH / COREPATH are supplied by the
# toolchain file (toolchain/rt1170-evkb.toolchain.cmake). Keep a fallback here
# so a bare `cmake -B build .` without the toolchain file still selects 117.
set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
# Use the LOCAL macros checkout (branch imxrt1176) rather than fetching from
# GitHub, so local edits to CMakeLists.include.txt are exercised.
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

# Compile the whole imxrt1176 core as the `cores` library
# (Multicore.cpp + MessagingUnit.cpp are part of it).
import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)

teensy_add_executable(cm4_boot_test cm4_boot_test.cpp)
teensy_target_link_libraries(cm4_boot_test cores)

target_link_libraries(cm4_boot_test.elf stdc++)
