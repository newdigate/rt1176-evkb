cmake_minimum_required(VERSION 3.24)
project(camera_ov5640_id)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_library(Wire)

teensy_add_executable(camera_ov5640_id camera_ov5640_id.cpp)
teensy_target_link_libraries(camera_ov5640_id cores Wire)

target_link_libraries(camera_ov5640_id.elf stdc++)
