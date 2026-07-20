cmake_minimum_required(VERSION 3.24)
project(tone_hw)

# TEENSY_VERSION / CPU_CORE_SPEED / COMPILERPATH / COREPATH are supplied by the
# toolchain file (toolchain/rt1170-evkb.toolchain.cmake). Keep a fallback here
# so a bare `cmake -B build .` without the toolchain file still selects 117.
set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
# Use the LOCAL macros checkout (branch imxrt1176) rather than fetching from
# GitHub, so local edits to CMakeLists.include.txt are exercised.
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

# Compile the whole imxrt1176 core as the `cores` library. No avr/util subdirs
# in the minimal Phase-0 core, so none are listed.
import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../../../cores/imxrt1176)

teensy_add_executable(tone_hw tone_hw.cpp)
teensy_target_link_libraries(tone_hw cores)

# tone_hw.cpp / main.cpp use C++; link the std C++ runtime (nano specs set in
# the toolchain file).
target_link_libraries(tone_hw.elf stdc++)
