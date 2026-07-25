cmake_minimum_required(VERSION 3.24)
project(pxp_decimate_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_library(PXP)

teensy_add_executable(pxp_decimate_test pxp_decimate_test.cpp)
teensy_target_link_libraries(pxp_decimate_test cores PXP)

target_link_libraries(pxp_decimate_test.elf stdc++)
