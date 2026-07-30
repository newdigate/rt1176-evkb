cmake_minimum_required(VERSION 3.24)
project(lvgl_pxp_copy_bench)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()      # lv_draw_buf_copy IS the thing under test (CPU side)
import_evkb_library(PXP)

teensy_add_executable(lvgl_pxp_copy_bench lvgl_pxp_copy_bench.cpp)
teensy_target_link_libraries(lvgl_pxp_copy_bench cores PXP)

target_link_libraries(lvgl_pxp_copy_bench.elf LVGL stdc++)
