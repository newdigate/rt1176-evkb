cmake_minimum_required(VERSION 3.24)
project(lvgl_pxp_copy_bench)

# The panel scans out during the measurement (NEW-55): the LCDIFv2's SDRAM
# reads are what turned the v6 bench's 174 ms full-frame CPU copy into the
# pipeline's 260 ms.  MipiDisplay needs the depth macros every db example sets.
add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()      # lv_draw_buf_copy IS the thing under test (CPU side)
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(lvgl_pxp_copy_bench
    lvgl_pxp_copy_bench.cpp
    ${_lvgl_dir}/port/lvgl_edma_copy.cpp)
teensy_target_link_libraries(lvgl_pxp_copy_bench cores MipiDisplay PXP)

target_link_libraries(lvgl_pxp_copy_bench.elf LVGL stdc++)
