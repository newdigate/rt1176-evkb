cmake_minimum_required(VERSION 3.24)
project(ssd1306_display)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_library(Wire)

teensy_add_executable(ssd1306_display ssd1306_display.cpp)
teensy_target_link_libraries(ssd1306_display cores Wire)

target_link_libraries(ssd1306_display.elf stdc++)
