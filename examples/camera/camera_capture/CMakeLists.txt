cmake_minimum_required(VERSION 3.24)
project(camera_capture)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_library(Wire)

teensy_add_executable(camera_capture camera_capture.cpp)
teensy_target_link_libraries(camera_capture cores Wire)

target_link_libraries(camera_capture.elf stdc++)
