cmake_minimum_required(VERSION 3.24)
project(usb_enum_test)

# TEENSY_VERSION / CPU_CORE_SPEED / COMPILERPATH / COREPATH are supplied by the
# toolchain file (toolchain/rt1170-evkb.toolchain.cmake). Keep a fallback here
# so a bare `cmake -B build .` without the toolchain file still selects 117.
set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
# Use the LOCAL macros checkout (branch imxrt1176) rather than fetching from
# GitHub, so local edits to CMakeLists.include.txt are exercised.

# Compile the whole imxrt1176 core as the `cores` library. No avr/util subdirs
# in the minimal Phase-0 core, so none are listed.

teensy_add_executable(usb_enum_test usb_enum_test.cpp)
teensy_target_link_libraries(usb_enum_test cores)

# usb_enum_test.cpp / main.cpp use C++; link the std C++ runtime (nano specs set in
# the toolchain file).
target_link_libraries(usb_enum_test.elf stdc++)
