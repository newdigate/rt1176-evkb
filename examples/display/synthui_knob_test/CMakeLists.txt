cmake_minimum_required(VERSION 3.24)
project(synthui_knob_test)

# XRGB8888 exactly like lvgl_rk055_panel_test (v7).  Directory scope on
# purpose: the definitions must reach the LVGL, MipiDisplay AND SynthUI
# objects created by the imports below, not just this target.
add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
# Fresh clone (no $TEENSY_LIB_ROOT/SynthUI checkout): import_evkb_synthui()
# FATAL_ERRORs by design, so this example cannot configure and its gate
# reports SKIP -- see docs/KNOWN-BROKEN-GATES.md (entry lands with the
# gate's docs task).
import_evkb_synthui()
# panels/rk055 selects the panel; one MIPI-DSI host, one panel dir ever live.
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)    # Display::fillScreen() paints via the PXP

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(synthui_knob_test
    synthui_knob_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp)
teensy_target_link_libraries(synthui_knob_test cores MipiDisplay PXP)

# SynthUI and LVGL are plain CMake static-lib targets (see lvgl_rk055_panel_test
# for why teensy_target_link_libraries cannot link them). LVGL is named
# directly here too, not only pulled in transitively via SynthUI's PUBLIC
# edge, because this target also compiles port/lvgl_mipi_panel.cpp itself.
target_link_libraries(synthui_knob_test.elf SynthUI LVGL stdc++)
