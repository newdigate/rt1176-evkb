cmake_minimum_required(VERSION 3.24)
project(rotary_knob_bench)

# XRGB8888 like every RK055 LVGL example. Directory scope so the definitions
# reach the LVGL and MipiDisplay objects, not just this target.
add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# ★ ONE ELF, DELIBERATELY -- and it is safe where vglite_lvgl_test's single
# binary was not. That example compiles LVGL's VG_LITE DRAW UNIT, which
# registers unconditionally in lv_init() and claims tasks it cannot execute on
# absent hardware (black screen, every liveness check green) -- hence its
# build-time split. THIS example never enables LV_USE_DRAW_VG_LITE: LVGL is
# always the software renderer, and the GPU is reached only by DIRECT
# vg_lite_* calls guarded by the chip-ID probe (vglite_probe's pattern, one
# binary two outcomes). The bench needs both engines in one boot -- that is
# what makes the A/B controlled.
import_evkb_lvgl()
import_evkb_vglite()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)    # Display::fillScreen() paints via the PXP

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(rotary_knob_bench
    rotary_knob_bench.cpp
    rk_geometry.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp)
teensy_target_link_libraries(rotary_knob_bench cores MipiDisplay PXP)

# LVGL and VGLite are plain CMake static-lib targets;
# teensy_target_link_libraries() would rewrite the names and lose their PUBLIC
# include dirs. LVGL is named directly because this target compiles
# port/lvgl_mipi_panel.cpp itself.
target_link_libraries(rotary_knob_bench.elf LVGL VGLite stdc++)
