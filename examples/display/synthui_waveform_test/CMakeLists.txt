cmake_minimum_required(VERSION 3.24)
project(synthui_waveform_test)

# The landscape (rotated present) pipeline is 32 bpp only.
add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)
set(TEENSY_VERSION 117 CACHE STRING "")
include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
import_evkb_synthui(VGLITE)
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)                  # the widget's lane blit, and the port's rotation
import_evkb_library(Wire)
import_evkb_library(TouchPanel gt911)
evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(synthui_waveform_test
    synthui_waveform_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp
    ${_lvgl_dir}/port/lvgl_pxp_copy.cpp
    ${_lvgl_dir}/port/lvgl_gt911_indev.cpp)
teensy_target_link_libraries(synthui_waveform_test cores Wire MipiDisplay PXP TouchPanel)
target_link_libraries(synthui_waveform_test.elf SynthUI LVGL VGLite stdc++ m)

# Bench only: -DWF_HOLD=mid stops on that state (see the .cpp).
if(WF_HOLD)
    target_compile_definitions(synthui_waveform_test.elf PRIVATE WF_HOLD="${WF_HOLD}")
endif()
