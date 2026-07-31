cmake_minimum_required(VERSION 3.24)
project(lvgl_rk055_panel_test)

# v7: XRGB8888.  Directory-scope so the definitions reach the LVGL and
# MipiDisplay objects created by the imports below, not just this target.
# The link-time depth probes (lvgl_rt1176.h / lcdifv2.h) turn a definition
# that fails to propagate into an undefined reference; the nm check in the
# gate-transcript ritual is the belt-and-braces.
# -DDEPTH_DEMO_565=ON builds the IDENTICAL scene at the default RGB565 --
# the banding "before" for the v7 depth ritual.  Demo variant: no gate, no
# golden.
option(DEPTH_DEMO_565 "RGB565 banding-demo variant of the same scene" OFF)
if(NOT DEPTH_DEMO_565)
    add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)
endif()

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
# panels/rk055 selects the panel; the RT1176's single MIPI-DSI host means
# exactly one panel directory may ever be on the include path.
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)    # Display::fillScreen() paints via the PXP

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(lvgl_rk055_panel_test
    lvgl_rk055_panel_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp)
teensy_target_link_libraries(lvgl_rk055_panel_test cores MipiDisplay PXP)

# LVGL is a plain CMake static-lib target (like CMSIS-DSP), not a
# teensy_add_library() one: teensy_target_link_libraries() rewrites each name to
# <name>.o and would not propagate LVGL's PUBLIC include dirs. Link it directly,
# exactly as lvgl_rpi_panel_test does.
target_link_libraries(lvgl_rk055_panel_test.elf LVGL stdc++)
