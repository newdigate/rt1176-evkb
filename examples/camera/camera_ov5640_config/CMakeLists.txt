cmake_minimum_required(VERSION 3.24)
project(camera_ov5640_config)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_library(Wire)

teensy_add_executable(camera_ov5640_config camera_ov5640_config.cpp)
teensy_target_link_libraries(camera_ov5640_config cores Wire)

target_link_libraries(camera_ov5640_config.elf stdc++)
