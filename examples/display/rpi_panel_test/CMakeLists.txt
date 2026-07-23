cmake_minimum_required(VERSION 3.24)
project(rpi_panel_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_library(RPiDisplay)

teensy_add_executable(rpi_panel_test rpi_panel_test.cpp)
teensy_target_link_libraries(rpi_panel_test cores RPiDisplay)

target_link_libraries(rpi_panel_test.elf stdc++)
