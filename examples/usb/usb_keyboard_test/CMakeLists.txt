cmake_minimum_required(VERSION 3.24)
project(usb_keyboard_test)

# TEENSY_VERSION / CPU_CORE_SPEED / COMPILERPATH / COREPATH come from the
# toolchain file (toolchain/rt1170-evkb.toolchain.cmake); fallback keeps 117.
set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../../../cores/imxrt1176)

teensy_add_executable(usb_keyboard_test usb_keyboard_test.cpp)
teensy_target_link_libraries(usb_keyboard_test cores)
target_link_libraries(usb_keyboard_test.elf stdc++)

# Hardware-observation variant (periodic type + CDC echo); not used by the QEMU gate.
teensy_add_executable(usb_keyboard_hw usb_keyboard_hw.cpp)
teensy_target_link_libraries(usb_keyboard_hw cores)
target_link_libraries(usb_keyboard_hw.elf stdc++)
