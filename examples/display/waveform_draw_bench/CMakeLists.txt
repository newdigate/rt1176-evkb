cmake_minimum_required(VERSION 3.24)
project(waveform_draw_bench)

# Bench-only (no gate): which way of drawing a full-width scrolling waveform
# lane is fast enough on the Nosh board, on the landscape (rotated) pipeline.
# The result decides synthui_waveform's draw path; see transcript_hw_nosh.txt.
add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)
set(TEENSY_VERSION 117 CACHE STRING "")
include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)
evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(waveform_draw_bench
    waveform_draw_bench.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp
    ${_lvgl_dir}/port/lvgl_pxp_copy.cpp)
teensy_target_link_libraries(waveform_draw_bench cores MipiDisplay PXP)
target_link_libraries(waveform_draw_bench.elf LVGL stdc++ m)
