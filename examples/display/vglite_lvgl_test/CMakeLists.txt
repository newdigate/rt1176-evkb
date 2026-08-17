cmake_minimum_required(VERSION 3.24)
project(vglite_lvgl_test)

# XRGB8888, like synthui_knob_test and lvgl_rk055_panel_test. Directory scope
# on purpose: these must reach the LVGL, MipiDisplay and SynthUI objects the
# imports below create, not just this target.
add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# ★ VGLITE is what makes this example different from synthui_knob_test. It
# enables LV_USE_DRAW_VG_LITE, and with it LV_USE_MATRIX and LV_USE_FLOAT --
# the backend needs both, and LV_USE_FLOAT MOVES PIXELS (lv_value_precise_t
# becomes float). That is why this is a separate example rather than a flag on
# the existing one: synthui_knob_test keeps its software goldens untouched.
import_evkb_lvgl(VGLITE)
import_evkb_synthui()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)    # Display::fillScreen() paints via the PXP

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(vglite_lvgl_test
    vglite_lvgl_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp)
teensy_target_link_libraries(vglite_lvgl_test cores MipiDisplay PXP)

# SynthUI, LVGL and VGLite are plain CMake static-lib targets;
# teensy_target_link_libraries() rewrites names to <name>.o and would lose
# their PUBLIC include dirs. LVGL is named directly as well as transitively
# because this target compiles port/lvgl_mipi_panel.cpp itself.
target_link_libraries(vglite_lvgl_test.elf SynthUI LVGL VGLite stdc++)
