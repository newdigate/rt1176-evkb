cmake_minimum_required(VERSION 3.24)
project(synthui_seven_segment_test)

add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
import_evkb_synthui()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(synthui_seven_segment_test
    synthui_seven_segment_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp
)
teensy_target_link_libraries(synthui_seven_segment_test cores MipiDisplay PXP)

target_link_libraries(synthui_seven_segment_test.elf SynthUI LVGL stdc++)

target_include_directories(synthui_seven_segment_test.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(SEVEN_SEGMENT_EYEBALL_HOLD)
    add_compile_definitions(SEVEN_SEGMENT_EYEBALL_HOLD=${SEVEN_SEGMENT_EYEBALL_HOLD})
endif()
