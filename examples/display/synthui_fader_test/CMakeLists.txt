cmake_minimum_required(VERSION 3.24)
project(synthui_fader_test)

# XRGB8888 exactly like synthui_knob_test. Directory scope on purpose: the
# definitions must reach the LVGL, MipiDisplay AND SynthUI objects.
add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
# Plain import, no VGLITE: the fader is sw-only BY DESIGN (spec section 1) --
# the silicon checkpoint decides whether a GPU TU ever exists.
import_evkb_synthui()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)    # Display::fillScreen() paints via the PXP

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(synthui_fader_test
    synthui_fader_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp)
teensy_target_link_libraries(synthui_fader_test cores MipiDisplay PXP)

# SynthUI and LVGL are plain CMake static-lib targets (see lvgl_rk055_panel_test
# for why teensy_target_link_libraries cannot link them). LVGL is named
# directly here too, not only pulled in transitively via SynthUI's PUBLIC
# edge, because this target also compiles port/lvgl_mipi_panel.cpp itself.
target_link_libraries(synthui_fader_test.elf SynthUI LVGL stdc++)

# Diagnostic eyeball build (separate build dir, NEVER a golden):
#   cmake -B build-eye -DFD_EYEBALL_HOLD=<1..3> -DCMAKE_TOOLCHAIN_FILE=...
# holds after the n-th checksummed frame for a QEMU-monitor pmemsave
# (recipe in acid_box/transcript_qemu.txt). 1=bank, 2=delta result, 3=fresh.
if(FD_EYEBALL_HOLD)
    add_compile_definitions(FD_EYEBALL_HOLD=${FD_EYEBALL_HOLD})
endif()
