cmake_minimum_required(VERSION 3.24)
project(usb_mouse_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)

teensy_add_executable(usb_mouse_test usb_mouse_test.cpp)
teensy_target_link_libraries(usb_mouse_test cores)
target_link_libraries(usb_mouse_test.elf stdc++)

# Hardware-observation variant (periodic cursor jiggle + CDC echo); not used by the QEMU gate.
teensy_add_executable(usb_mouse_hw usb_mouse_hw.cpp)
teensy_target_link_libraries(usb_mouse_hw cores)
target_link_libraries(usb_mouse_hw.elf stdc++)
